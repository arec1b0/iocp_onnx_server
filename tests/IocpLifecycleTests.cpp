#include "server/IocpServer.h"
#include <psapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <latch>
#include <semaphore>
#include <stdexcept>
#include <string_view>

namespace {
    struct ManualClock {
        std::atomic<int> ticks{0};

        IocpServer::Clock::time_point Now() const {
            return IocpServer::Clock::time_point(std::chrono::milliseconds(ticks.load()));
        }
    };
}

struct IocpServerTestAccess {
    struct Snapshot {
        std::size_t reads = 0;
        std::size_t writes = 0;
        std::size_t handlers = 0;
        std::size_t queued = 0;
        std::size_t running = 0;
        std::size_t submitting = 0;
        std::size_t closed = 0;
        std::size_t frames = 0;
        std::size_t readCapacity = 0;
        ConnectionMetricsSnapshot metrics;

        bool Empty() const {
            return reads == 0 && writes == 0 && handlers == 0 && queued == 0 && running == 0 && submitting == 0;
        }
    };

    static Snapshot Inspect(IocpServer& server) {
        std::lock_guard lock(server.registryMutex_);
        return InspectLocked(server);
    }

    static Snapshot SweepAt(IocpServer& server, ManualClock& clock, int ticks) {
        std::lock_guard lock(server.registryMutex_);
        clock.ticks.store(ticks);
        server.CloseExpiredLocked(clock.Now());
        return InspectLocked(server);
    }

    static void Admit(IocpServer& server, SocketHandle socket) {
        server.Admit(std::move(socket));
    }

    static Snapshot InspectLocked(const IocpServer& server) {
        Snapshot snapshot;
        snapshot.queued = server.inferenceQueue_.size();
        snapshot.metrics = server.connectionMetrics_;
        for (const auto& entry : server.connections_) {
            const auto& active = entry.second;
            snapshot.running += (active.activity == IocpServer::Activity::Running);
            if (active.activity == IocpServer::Activity::SubmittingSend) {
                ++snapshot.submitting;
            } else if (!active.pending) {
                ++snapshot.handlers;
            } else if (active.pending->operation == IOOperation::Read) {
                ++snapshot.reads;
                snapshot.frames += active.receivingFrame;
                snapshot.readCapacity += active.pending->wsaBuf.len;
            } else {
                ++snapshot.writes;
            }
            if (active.owner->GetSocket() == INVALID_SOCKET) {
                ++snapshot.closed;
            }
        }
        return snapshot;
    }

    static bool Stopped(IocpServer& server) {
        std::lock_guard lifecycle(server.lifecycleMutex_);
        return !server.winsockStarted_ && !server.iocpHandle_ &&
            server.listenSocket_ == INVALID_SOCKET && server.boundPort_ == 0 &&
            server.workers_.empty() && server.inferenceWorkers_.empty() && !server.acceptThread_.joinable() &&
            server.peerEvents_ == WSA_INVALID_EVENT && Inspect(server).Empty();
    }

    // Called before Start or after the failed Start has fully rolled back.
    static void FailWorkerLaunchAt(IocpServer& server, int workerIndex) {
        server.failWorkerLaunchAt_ = workerIndex;
    }

    static void AfterSendSubmission(IocpServer& server, std::function<void()> callback) {
        server.afterSendSubmission_ = std::move(callback);
    }

    static bool HasEarlyCompletion(IocpServer& server) {
        std::lock_guard lock(server.registryMutex_);
        return std::any_of(server.connections_.begin(), server.connections_.end(), [](const auto& entry) {
            return entry.second.waitingForSubmission;
        });
    }

    static bool StopRequested(IocpServer& server) { return server.stopping_.load(); }

    static bool DisableSendBuffer(IocpServer& server) {
        std::lock_guard lock(server.registryMutex_);
        if (server.connections_.size() != 1) {
            return false;
        }
        const auto& active = server.connections_.begin()->second;
        const int zero = 0;
        return setsockopt(active.owner->GetSocket(), SOL_SOCKET, SO_SNDBUF,
                          reinterpret_cast<const char*>(&zero), sizeof(zero)) == 0;
    }
};

namespace {
    using namespace std::chrono_literals;
    using Inspect = IocpServerTestAccess;
    constexpr int SocketTimeoutMs = 4000;
    constexpr std::size_t LargeOutputElements = 2 * 1024 * 1024;

    void Check(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool MetricsAreConsistent(const ConnectionMetricsSnapshot& snapshot, std::uint64_t capacity) {
        return snapshot.connections_accepted_total >= snapshot.connections_closed_total &&
            snapshot.connections_accepted_total - snapshot.connections_closed_total == snapshot.connections_active &&
            snapshot.pending_io_operations <= snapshot.connections_active &&
            snapshot.connections_active <= snapshot.connections_high_watermark &&
            snapshot.connections_high_watermark <= capacity;
    }

    void CheckDrainedMetrics(const IocpServer& server, std::uint64_t capacity) {
        const auto metrics = server.GetConnectionMetrics();
        Check(MetricsAreConsistent(metrics, capacity) && metrics.connections_active == 0 &&
                  metrics.pending_io_operations == 0, "metrics did not balance after drain");
    }

    struct Winsock {
        Winsock() {
            WSADATA data;
            Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "test WSAStartup failed");
        }
        ~Winsock() { WSACleanup(); }
    };

    template<class Predicate>
    void WaitFor(Predicate matches, const char* message) {
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (!matches()) {
            Check(std::chrono::steady_clock::now() < deadline, message);
            std::this_thread::sleep_for(1ms);
        }
    }

    sockaddr_in Loopback(int port) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u_short>(port));
        return address;
    }

    SocketHandle OpenSocket() {
        SocketHandle client(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        Check(client.Get() != INVALID_SOCKET, "could not create test socket");
        return client;
    }

    void SetOption(SOCKET socket, int option, const auto& value) {
        Check(setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char*>(&value),
                         sizeof(value)) == 0, "setsockopt failed");
    }

    std::runtime_error SocketFailure(const char* operation, SOCKET socket, int code, int remotePort = 0) {
        using namespace std::chrono;
        const auto timestamp = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
        sockaddr_in local{}, remote{};
        int localSize = sizeof(local), remoteSize = sizeof(remote);
        getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localSize);
        if (!remotePort && getpeername(socket, reinterpret_cast<sockaddr*>(&remote), &remoteSize) == 0) {
            remotePort = ntohs(remote.sin_port);
        }
        return std::runtime_error(std::string("Loopback operation failed: operation=") + operation +
            " code=" + std::to_string(code) + " timestamp_us=" + std::to_string(timestamp) +
            " local_port=" + std::to_string(ntohs(local.sin_port)) + " remote_port=" + std::to_string(remotePort));
    }

    SocketHandle Connect(int port, int receiveBuffer = 0) {
        auto client = OpenSocket();
        SetOption(client.Get(), SO_RCVTIMEO, SocketTimeoutMs);
        SetOption(client.Get(), SO_SNDTIMEO, SocketTimeoutMs);
        if (receiveBuffer > 0) {
            SetOption(client.Get(), SO_RCVBUF, receiveBuffer);
        }
        const auto address = Loopback(port);
        if (connect(client.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw SocketFailure("connect", client.Get(), WSAGetLastError(), port);
        }
        return client;
    }

    using Frame = std::array<char, Protocol::HeaderBytes + Protocol::FloatBytes>;

    Frame MakeRequest(float value) {
        Frame frame{1};
        std::memcpy(frame.data() + Protocol::HeaderBytes, &value, sizeof(value));
        return frame;
    }

    void SendBytes(SOCKET socket, std::span<const char> bytes) {
        const auto size = static_cast<int>(bytes.size());
        int offset = 0;
        while (offset < size) {
            const int sent = send(socket, bytes.data() + offset, size - offset, 0);
            if (sent <= 0) throw SocketFailure("send", socket, sent == SOCKET_ERROR ? WSAGetLastError() : 0);
            offset += sent;
        }
    }

    Frame SendRequest(SOCKET socket, float value) {
        const auto frame = MakeRequest(value);
        SendBytes(socket, frame);
        return frame;
    }

    void CheckResponse(SOCKET socket, const Frame& expected) {
        Frame actual{};
        const auto size = static_cast<int>(actual.size());
        int offset = 0;
        while (offset < size) {
            const int received = recv(socket, actual.data() + offset, size - offset, 0);
            if (received <= 0) throw SocketFailure("recv", socket, received == SOCKET_ERROR ? WSAGetLastError() : 0);
            offset += received;
        }
        Check(actual == expected, "inference response differs from request");
    }

    void CheckPeerClosed(SOCKET socket, std::size_t maxBufferedBytes = 0) {
        std::array<char, 8192> buffer;
        std::size_t read = 0;
        for (;;) {
            const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received == 0 || (received == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET)) {
                return;
            }
            Check(received > 0, "socket stayed open after server shutdown");
            read += static_cast<std::size_t>(received);
            Check(read <= maxBufferedBytes, "received unexpected bytes after shutdown");
        }
    }

    std::vector<float> Echo(const std::vector<float>& input) {
        return input;
    }

    std::vector<float> LargeOutput(const std::vector<float>&) {
        return std::vector<float>(LargeOutputElements, 42.0f);
    }

    SocketHandle ConnectBackpressuredClient(IocpServer& server) {
        auto client = Connect(server.BoundPort(), 1024);
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "writer's initial receive did not post");
        // Disable dynamic send buffering before submitting the request so
        // Winsock retains the overlapped send when the peer stops reading.
        Check(Inspect::DisableSendBuffer(server), "could not disable send buffering for the test");
        return client;
    }

    void TestReceiveDisconnect() {
        IocpServer server("127.0.0.1", 0, 4, 1, Echo);
        Check(server.Start(), "server failed to start");
        constexpr std::size_t clientCount = 16;
        std::vector<SocketHandle> clients;
        for (std::size_t i = 0; i < clientCount; ++i) {
            clients.push_back(Connect(server.BoundPort()));
        }
        WaitFor([&] { return Inspect::Inspect(server).reads == clientCount; }, "receives did not become pending");
        std::vector<std::jthread> disconnectors;
        for (auto& client : clients) {
            disconnectors.emplace_back([&client] { client.Close(); });
        }
        disconnectors.clear();
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "peer receive disconnects retained connections");
        server.Stop();
        Check(Inspect::Stopped(server), "receive test server did not drain");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        const auto metrics = server.GetConnectionMetrics();
        Check(metrics.connections_accepted_total == clientCount && metrics.connections_closed_total == clientCount &&
                  metrics.connections_high_watermark == clientCount, "simultaneous disconnects miscounted retirements");
        std::cout << "Pending receive and simultaneous disconnects passed (16 clients).\n";
    }

    void TestSendDisconnect() {
        IocpServer server("127.0.0.1", 0, 4, 1, LargeOutput);
        Check(server.Start(), "server failed to start");
        auto client = ConnectBackpressuredClient(server);
        SendRequest(client.Get(), 1.0f);
        WaitFor([&] { return Inspect::Inspect(server).writes == 1; }, "backpressured send never became pending");
        std::this_thread::sleep_for(25ms);
        Check(Inspect::Inspect(server).writes == 1, "backpressured send finished before peer reset");
        SetOption(client.Get(), SO_LINGER, linger{1, 0});
        client.Close();
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "peer reset retained a connection");
        server.Stop();
        Check(Inspect::Stopped(server), "send test server did not drain");
        std::cout << "Peer reset during a pending Winsock send passed.\n";
    }

    void TestCancelActiveIo() {
        IocpServer server("127.0.0.1", 0, 4, 1, LargeOutput);
        Check(server.Start(), "server failed to start");
        auto writer = ConnectBackpressuredClient(server);
        auto reader = Connect(server.BoundPort());
        SendRequest(writer.Get(), 2.0f);
        WaitFor([&] {
            const auto state = Inspect::Inspect(server);
            return state.reads == 1 && state.writes == 1;
        }, "receive and send were not simultaneously pending");

        server.Stop();
        CheckPeerClosed(reader.Get());
        CheckPeerClosed(writer.Get(), Protocol::HeaderBytes + LargeOutputElements * Protocol::FloatBytes);
        Check(Inspect::Stopped(server), "active I/O did not drain");
        server.Stop();
        Check(Inspect::Stopped(server), "repeated Stop changed the stopped state");
        std::cout << "Shutdown canceled and drained pending receive and send. Repeated Stop passed.\n";
    }

    void TestConcurrentStopDuringInference() {
        std::binary_semaphore entered(0);
        std::binary_semaphore release(0);
        IocpServer server("127.0.0.1", 0, 4, 1, [&](const std::vector<float>& input) {
            entered.release();
            Check(release.try_acquire_for(15s), "inference test gate timed out");
            return input;
        });
        Check(server.Start(), "server failed to start");
        auto client = Connect(server.BoundPort());
        SendRequest(client.Get(), 3.0f);
        Check(entered.try_acquire_for(4s), "inference did not start");

        std::atomic<int> returned{0};
        std::vector<std::jthread> stoppers;
        try {
            for (int i = 0; i < 4; ++i) {
                stoppers.emplace_back([&] { server.Stop(); ++returned; });
            }
            WaitFor([&] {
                const auto state = Inspect::Inspect(server);
                return state.closed == 1 && state.handlers == 1 && state.reads == 0 && state.writes == 0;
            }, "Stop failed to close the socket and retain the inference handler");
            CheckPeerClosed(client.Get());
            Check(returned.load() == 0, "Stop returned while inference was still blocked");
            const auto handling = server.GetConnectionMetrics();
            Check(handling.connections_active == 1 && handling.pending_io_operations == 0 &&
                      handling.cancellation_requests_total == 0, "active handler was counted as pending I/O");
        } catch (...) {
            release.release();
            throw;
        }
        release.release();
        stoppers.clear();
        Check(returned.load() == 4 && Inspect::Stopped(server), "concurrent Stop did not drain");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        Check(server.GetConnectionMetrics().connections_closed_total == 1, "concurrent Stop counted retirement twice");
        std::cout << "Shutdown during inference and four concurrent Stop calls passed.\n";
    }

    void TestPartialStartup() {
        auto blocker = OpenSocket();
        SetOption(blocker.Get(), SO_EXCLUSIVEADDRUSE, TRUE);
        auto address = Loopback(0);
        auto addr = reinterpret_cast<sockaddr*>(&address);
        Check(bind(blocker.Get(), addr, sizeof(address)) == 0, "could not bind blocking socket");
        int length = sizeof(address);
        Check(getsockname(blocker.Get(), addr, &length) == 0, "could not get blocking socket port");
        IocpServer server("127.0.0.1", ntohs(address.sin_port), 2, 1, Echo);
        Check(!server.Start(), "start unexpectedly bound an occupied address");
        Check(Inspect::Stopped(server), "failed start retained resources");
        server.Stop();
        server.Stop();
        blocker.Close();
        Check(server.Start(), "server could not start after partial startup failure");
        Check(server.Start(), "repeated Start failed");
        auto client = Connect(server.BoundPort());
        CheckResponse(client.Get(), SendRequest(client.Get(), 4.0f));
        server.Stop();
        Check(Inspect::Stopped(server), "server retained resources after restart");
        std::cout << "Partial startup rollback, repeated Stop, and restart passed.\n";
    }

    void TestWorkerLaunchRollback() {
        IocpServer server("127.0.0.1", 0, 2, 1, Echo);
        Inspect::FailWorkerLaunchAt(server, 1);
        Check(!server.Start(), "injected second worker launch unexpectedly succeeded");
        Check(Inspect::Stopped(server), "failed worker launch left acquired resources");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        server.Stop();
        Inspect::FailWorkerLaunchAt(server, -1);
        Check(server.Start(), "server could not restart after worker-launch rollback");
        const auto client = Connect(server.BoundPort());
        CheckResponse(client.Get(), SendRequest(client.Get(), 12.0f));
        server.Stop();
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        std::cout << "Injected second worker launch failure rolled back the listener, IOCP, and first worker.\n";
    }

    void ReportSnapshotLatency(const char* phase, std::span<long long> nanoseconds) {
        Check(!nanoseconds.empty(), "no snapshot latency measurements were recorded");
        std::sort(nanoseconds.begin(), nanoseconds.end());
        const auto percentile = [&](std::size_t rank) {
            return nanoseconds[(nanoseconds.size() * rank + 99) / 100 - 1];
        };
        std::cout << "Snapshot latency ns (" << phase << ", n=" << nanoseconds.size() << "): p50="
                  << percentile(50) << " p95=" << percentile(95) << " p99=" << percentile(99)
                  << " max=" << nanoseconds.back() << '\n';
    }

    std::size_t PrivateBytes() {
        PROCESS_MEMORY_COUNTERS_EX counters{};
        Check(K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                     sizeof(counters)) != FALSE, "could not query process memory");
        return counters.PrivateUsage;
    }

    void TestConcurrentChurn() {
        IocpServer server("127.0.0.1", 0, 4, 1, Echo);
        Check(server.Start(), "server failed to start");
        const int port = server.BoundPort();
        std::vector<long long> idleSamples(10000);
        for (auto& sample : idleSamples) {
            const auto start = IocpServer::Clock::now();
            const auto snapshot = server.GetConnectionMetrics();
            sample = std::chrono::duration_cast<std::chrono::nanoseconds>(IocpServer::Clock::now() - start).count();
            Check(snapshot.connections_active == 0, "idle snapshot unexpectedly had active connections");
        }
        std::vector<long long> churnSamples;
        churnSamples.reserve(10000);
        bool consistent = true;
        std::size_t observations = 0;
        std::jthread observer([&](std::stop_token stop) {
            auto previous = server.GetConnectionMetrics();
            while (!stop.stop_requested()) {
                const auto start = IocpServer::Clock::now();
                const auto current = server.GetConnectionMetrics();
                const auto elapsed = IocpServer::Clock::now() - start;
                if (current.connections_active > 0 && churnSamples.size() < churnSamples.capacity()) {
                    churnSamples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
                }
                consistent = consistent && MetricsAreConsistent(current, ConnectionLimits{}.maxConnections) &&
                    current.connections_accepted_total >= previous.connections_accepted_total &&
                    current.connections_closed_total >= previous.connections_closed_total &&
                    current.connections_high_watermark >= previous.connections_high_watermark;
                previous = current;
                ++observations;
                std::this_thread::sleep_for(1ms);
            }
        });
        std::array<std::size_t, 5> samples{};
        for (auto& sample : samples) {
            std::vector<std::future<void>> clients;
            for (int i = 0; i < 8; ++i) {
                clients.push_back(std::async(std::launch::async, [port] {
                    for (int request = 0; request < 40; ++request) {
                        const auto client = Connect(port);
                        if (request % 2 == 0) {
                            CheckResponse(client.Get(), SendRequest(client.Get(), static_cast<float>(request)));
                        }
                    }
                }));
            }
            for (auto& client : clients) {
                client.get();
            }
            // A request to the listener after all producers exit is also handled
            // before measuring the registry once it drains.
            {
                const auto marker = Connect(port);
                CheckResponse(marker.Get(), SendRequest(marker.Get(), -1.0f));
            }
            WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "stress wave retained connections");
            // Churn already finished before this snapshot. The separate observer
            // also checks the invariants while connections are active.
            CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
            sample = PrivateBytes();
        }
        server.Stop();
        observer.request_stop();
        observer.join();
        Check(Inspect::Stopped(server), "stress server did not drain");
        Check(consistent && observations > 1, "concurrent snapshots violated lifecycle invariants");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        ReportSnapshotLatency("idle", idleSamples);
        if (!churnSamples.empty()) {
            ReportSnapshotLatency("active churn", churnSamples);
        }
        std::cout << "Concurrent snapshot invariants passed (" << observations << " samples).\n";
        std::cout << "Concurrent churn passed (1600 clients plus one marker per wave). "
                     "Private bytes after each drained wave:";
        for (const auto sample : samples) {
            std::cout << ' ' << sample;
        }
        std::cout << '\n';
    }

    // Deadlines are advanced explicitly. Winsock still supplies real completion packets.
    constexpr ConnectionLimits TestLimits{2, 100ms, 50ms, 1000ms};

    void TestAdmissionAndRecovery() {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 4, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "admission test server could not start");
        const int port = server.BoundPort();

        auto unconnected = OpenSocket();
        const SOCKET failedSocket = unconnected.Get();
        Inspect::Admit(server, std::move(unconnected));
        Check(Inspect::Inspect(server).Empty(), "immediate initial receive failure retained a slot");
        const auto postingFailure = server.GetConnectionMetrics();
        Check(postingFailure.io_failures_total == 1 && postingFailure.connections_accepted_total == 1 &&
                  postingFailure.connections_closed_total == 1 && postingFailure.pending_io_operations == 0,
              "immediate posting failure was not accounted for exactly once");
        int socketType = 0;
        int length = sizeof(socketType);
        Check(getsockopt(failedSocket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socketType), &length) == SOCKET_ERROR
                  && WSAGetLastError() == WSAENOTSOCK, "failed admission retained its socket");

        auto first = Connect(port);
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "connection below capacity was not admitted");
        const auto belowCapacity = server.GetConnectionMetrics();
        Check(belowCapacity.connections_active == 1 && belowCapacity.pending_io_operations == 1 &&
                  belowCapacity.connections_high_watermark == 1, "first admitted slot was not accounted for");
        auto second = Connect(port);
        WaitFor([&] { return Inspect::Inspect(server).reads == 2; }, "connection at capacity was not admitted");

        std::vector<std::future<void>> overload;
        for (int i = 0; i < 12; ++i) {
            overload.push_back(std::async(std::launch::async, [port] {
                const auto excess = Connect(port);
                CheckPeerClosed(excess.Get());
            }));
        }
        for (auto& rejected : overload) {
            rejected.get();
        }
        Check(Inspect::Inspect(server).reads == 2, "overload changed the two admitted slots");
        const auto atCapacity = server.GetConnectionMetrics();
        Check(atCapacity.connections_accepted_total == 3 && atCapacity.connections_rejected_capacity_total == 12 &&
                  atCapacity.connections_high_watermark == 2 && atCapacity.pending_io_operations == 2,
              "capacity counters did not match admitted and rejected clients");
        first.Close();
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "disconnect did not recover capacity");
        auto replacement = Connect(port);
        CheckResponse(replacement.Get(), SendRequest(replacement.Get(), 1.0f));
        CheckResponse(second.Get(), SendRequest(second.Get(), 2.0f));
        server.Stop();
        Check(Inspect::Stopped(server), "saturation test server did not drain");
        CheckDrainedMetrics(server, TestLimits.maxConnections);
        Check(server.GetConnectionMetrics().connections_closed_total == 4, "saturation lifecycle miscounted retirements");
        server.Stop();
        Check(server.Start(), "saturation server restart failed");
        const auto restarted = server.GetConnectionMetrics();
        Check(restarted.connections_accepted_total == 4 && restarted.connections_closed_total == 4 &&
                  restarted.connections_rejected_capacity_total == 12 && restarted.connections_high_watermark == 2,
              "server restart reset cumulative connection metrics");
        auto afterRestart = Connect(server.BoundPort());
        CheckResponse(afterRestart.Get(), SendRequest(afterRestart.Get(), 3.0f));
        server.Stop();
        CheckDrainedMetrics(server, TestLimits.maxConnections);
        Check(server.GetConnectionMetrics().connections_closed_total == 5, "restarted lifecycle did not add one retirement");
        std::cout << "Admission below and at capacity, 12 simultaneous rejections, failed admission cleanup, and slot recovery passed.\n";
    }

    void TestIdleExpiry() {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "idle test server could not start");
        const int port = server.BoundPort();
        auto client = Connect(port);
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "idle receive was not posted");
        Check(Inspect::SweepAt(server, clock, 99).closed == 0, "idle receive expired before its deadline");
        const auto expired = Inspect::SweepAt(server, clock, 100);
        Check(expired.closed == 1 && expired.reads == 1, "expiration freed the pending receive before dequeue");
        Check(expired.metrics.idle_timeouts_total == 1 && expired.metrics.cancellation_requests_total == 1 &&
                  expired.metrics.cancellation_completions_total == 0 && expired.metrics.pending_io_operations == 1,
              "timeout request was counted as a completion before dequeue");
        CheckPeerClosed(client.Get());
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "idle expiration did not retire the slot");
        Check(Inspect::SweepAt(server, clock, 100).Empty(), "repeated timeout pass changed the drained registry");
        const auto drained = server.GetConnectionMetrics();
        Check(drained.idle_timeouts_total == 1 && drained.connections_closed_total == 1 &&
                  drained.cancellation_requests_total == 1 && drained.pending_io_operations == 0 &&
                  drained.cancellation_completions_total + drained.io_failures_total <= 1,
              "idle receive did not drain exactly one operation after timeout");
        std::cout << "Idle timeout drain reported " << drained.cancellation_completions_total
                  << " cancellation completions and " << drained.io_failures_total << " I/O failures.\n";
        auto replacement = Connect(port);
        CheckResponse(replacement.Get(), SendRequest(replacement.Get(), 3.0f));
        std::cout << "Idle equality boundary, pending receive cancellation, and single retirement passed.\n";
    }

    void TestShutdownAtCapacity() {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "saturated shutdown test server could not start");
        const int port = server.BoundPort();
        auto first = Connect(port);
        auto second = Connect(port);
        WaitFor([&] { return Inspect::Inspect(server).reads == 2; }, "server did not fill both admission slots");
        auto excess = Connect(port);
        CheckPeerClosed(excess.Get());
        Check(server.GetConnectionMetrics().connections_rejected_capacity_total == 1,
              "server did not reject the excess connection before shutdown");
        server.Stop();
        CheckPeerClosed(first.Get());
        CheckPeerClosed(second.Get());
        Check(Inspect::Stopped(server), "saturated server did not release all acquired resources");
        CheckDrainedMetrics(server, TestLimits.maxConnections);
        const auto metrics = server.GetConnectionMetrics();
        Check(metrics.connections_accepted_total == 2 && metrics.connections_closed_total == 2 &&
                  metrics.connections_high_watermark == 2 && metrics.cancellation_requests_total == 2,
              "saturated shutdown did not close and drain its two registered reads");
        std::cout << "Shutdown drained both pending reads at capacity after rejecting an excess client.\n";
    }

    void TestFrameExpiry(std::size_t prefixBytes) {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "frame timeout test server could not start");
        auto client = Connect(server.BoundPort());
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "initial receive was not posted");
        const auto readCapacity = Inspect::Inspect(server).readCapacity;
        const auto retained = prefixBytes < Protocol::HeaderBytes ? prefixBytes : 0;
        const auto frame = MakeRequest(4.0f);
        clock.ticks.store(10);
        SendBytes(client.Get(), std::span(frame).first(prefixBytes));
        WaitFor([&] {
            const auto state = Inspect::Inspect(server);
            return state.frames == 1 && state.readCapacity == readCapacity - retained;
        }, "partial frame did not enter the expected receive phase");
        Check(Inspect::SweepAt(server, clock, 59).closed == 0, "partial frame expired before its deadline");
        const auto expired = Inspect::SweepAt(server, clock, 60);
        Check(expired.closed == 1 && expired.frames == 1, "partial frame timer did not cancel the pending read");
        Check(expired.metrics.incomplete_frame_timeouts_total == 1 && expired.metrics.idle_timeouts_total == 0,
              "partial frame timeout was misclassified");
        CheckPeerClosed(client.Get());
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "partial frame cancellation retained its slot");
    }

    void TestIncrementalFrame() {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "incremental frame test server could not start");
        auto client = Connect(server.BoundPort());
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "initial receive was not posted");
        const auto readCapacity = Inspect::Inspect(server).readCapacity;
        const auto frame = MakeRequest(5.0f);
        const std::array ticks{5, 25, 54};
        for (std::size_t i = 0; i < ticks.size(); ++i) {
            clock.ticks.store(ticks[i]);
            SendBytes(client.Get(), std::span(frame).subspan(i, 1));
            WaitFor([&] {
                const auto state = Inspect::Inspect(server);
                return state.frames == 1 && state.readCapacity == readCapacity - i - 1;
            }, "incremental byte was not observed");
        }
        Check(Inspect::SweepAt(server, clock, 55).closed == 1, "the last fragment extended the absolute deadline");
        CheckPeerClosed(client.Get());
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "incremental frame cancellation retained its slot");
        std::cout << "Incremental frame delivery kept its original deadline.\n";
    }

    void TestFrameCompletionBoundary(int lastFragmentAt) {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "fragment completion test server could not start");
        auto client = Connect(server.BoundPort());
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "initial receive was not posted");
        const auto frame = MakeRequest(6.0f);
        clock.ticks.store(10);
        SendBytes(client.Get(), std::span(frame).first(5));
        WaitFor([&] { return Inspect::Inspect(server).frames == 1; }, "partial request was not observed");
        clock.ticks.store(lastFragmentAt);
        SendBytes(client.Get(), std::span(frame).subspan(5));
        if (lastFragmentAt < 60) {
            CheckResponse(client.Get(), frame);
        } else {
            CheckPeerClosed(client.Get());
            WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "expired completion retained its slot");
            const auto metrics = server.GetConnectionMetrics();
            Check(metrics.incomplete_frame_timeouts_total == 1 && metrics.cancellation_requests_total == 0,
                  "expired completion counted cancellation after dequeue");
        }
    }

    void TestInferenceAndBufferedFrameTiming() {
        ManualClock clock;
        std::binary_semaphore entered(0);
        std::binary_semaphore release(0);
        IocpServer server("127.0.0.1", 0, 2, 1, [&](const std::vector<float>& input) {
            if (input.front() == 7.0f) {
                entered.release();
                Check(release.try_acquire_for(10s), "inference timing test gate expired");
            }
            return input;
        }, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "inference timing test server could not start");
        auto client = Connect(server.BoundPort());
        const auto first = MakeRequest(7.0f);
        const auto second = MakeRequest(8.0f);
        std::vector<char> batch(first.begin(), first.end());
        batch.insert(batch.end(), second.begin(), second.begin() + 3);
        SendBytes(client.Get(), batch);
        Check(entered.try_acquire_for(4s), "inference did not start");
        const auto handling = Inspect::SweepAt(server, clock, 150);
        release.release();
        Check(handling.handlers == 1 && handling.closed == 0, "inference spent the network idle budget");
        CheckResponse(client.Get(), first);
        WaitFor([&] { return Inspect::Inspect(server).frames == 1; }, "buffered partial request was not exposed");
        clock.ticks.store(199);
        SendBytes(client.Get(), std::span(second).subspan(3));
        CheckResponse(client.Get(), second);
        std::cout << "Inference and buffered partial request received independent network budgets.\n";
    }

    void TestSendExpiry() {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, LargeOutput, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "send timeout test server could not start");
        auto client = ConnectBackpressuredClient(server);
        SendRequest(client.Get(), 9.0f);
        WaitFor([&] { return Inspect::Inspect(server).writes == 1; }, "backpressured send was not pending");
        const auto expired = Inspect::SweepAt(server, clock, 100);
        Check(expired.closed == 1 && expired.writes == 1, "send timeout did not retain the pending write");
        Check(expired.metrics.send_timeouts_total == 1 && expired.metrics.cancellation_requests_total == 1 &&
                  expired.metrics.pending_io_operations == 1 && expired.metrics.cancellation_completions_total == 0,
              "stalled send timeout accounting advanced before dequeue");
        CheckPeerClosed(client.Get(), Protocol::HeaderBytes + LargeOutputElements * Protocol::FloatBytes);
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "send cancellation retained its slot");
        CheckDrainedMetrics(server, TestLimits.maxConnections);
        std::cout << "Stalled send timeout canceled and drained a real WSASend.\n";
    }

    void TestTimeoutRacingWithShutdown() {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "shutdown race test server could not start");
        auto client = Connect(server.BoundPort());
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "race test receive was not pending");
        std::latch start(1);
        std::jthread stop([&] { start.wait(); server.Stop(); });
        start.count_down();
        Inspect::SweepAt(server, clock, 100);
        stop.join();
        CheckPeerClosed(client.Get());
        Check(Inspect::Stopped(server), "shutdown and timeout did not drain exactly once");
        CheckDrainedMetrics(server, TestLimits.maxConnections);
        const auto metrics = server.GetConnectionMetrics();
        Check(metrics.connections_closed_total == 1 && metrics.idle_timeouts_total <= 1 &&
                  metrics.cancellation_requests_total <= 1 && metrics.cancellation_completions_total <= 1,
              "timeout racing with shutdown counted one lifecycle twice");
        std::cout << "Timeout sweep racing with shutdown passed.\n";
    }

    void TestTimeoutRacingWithPeerReset() {
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Check(server.Start(), "timeout and reset test server could not start");
        auto client = Connect(server.BoundPort());
        SetOption(client.Get(), SO_LINGER, linger{1, 0});
        WaitFor([&] { return Inspect::Inspect(server).reads == 1; }, "reset test receive was not pending");
        std::latch start(1);
        std::jthread disconnect([&] { start.wait(); client.Close(); });
        start.count_down();
        Inspect::SweepAt(server, clock, 100);
        disconnect.join();
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "timeout and peer reset retained a connection");
        server.Stop();
        CheckDrainedMetrics(server, TestLimits.maxConnections);
        const auto metrics = server.GetConnectionMetrics();
        Check(metrics.connections_accepted_total == 1 && metrics.connections_closed_total == 1 &&
                  metrics.idle_timeouts_total <= 1 && metrics.cancellation_requests_total <= 1 &&
                  metrics.cancellation_completions_total + metrics.io_failures_total <= 1,
              "timeout and peer reset counted one operation twice");
        std::cout << "Timeout sweep racing with a real peer reset drained exactly once.\n";
    }

    void TestWallClockSweep() {
        const ConnectionLimits limits{2, 750ms, 250ms, 10ms};
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, limits);
        Check(server.Start(), "wall-clock test server could not start");
        auto idle = Connect(server.BoundPort());
        auto partial = Connect(server.BoundPort());
        const auto frame = MakeRequest(10.0f);
        SendBytes(partial.Get(), std::span(frame).first(9));
        WaitFor([&] { return Inspect::Inspect(server).frames == 1; }, "wall-clock partial request was not observed");
        CheckPeerClosed(partial.Get());
        CheckPeerClosed(idle.Get());
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "automatic timeout sweep left connections registered");
        auto valid = Connect(server.BoundPort());
        CheckResponse(valid.Get(), SendRequest(valid.Get(), 11.0f));
        std::cout << "Automatic loopback sweep closed idle and incomplete clients, then served a valid request.\n";
    }

    struct InferenceGate {
        std::counting_semaphore<2> entered{0};
        std::promise<void> release;
        const std::shared_future<void> ready = release.get_future().share();

        void Block() {
            entered.release();
            Check(ready.wait_for(15s) == std::future_status::ready, "inference gate expired");
        }
    };

    void TestInferenceQueue() {
        InferenceGate gate;
        std::vector<float> started;
        IocpServer server("127.0.0.1", 0, 1, 1, [&](const std::vector<float>& input) {
            started.push_back(input.front());
            if (input.front() == 1.0f) gate.Block();
            return input;
        }, {}, IocpServer::Clock::now, nullptr, {1, 2});
        Check(server.Start(), "inference queue server could not start");
        const auto port = server.BoundPort();
        auto first = Connect(port);
        const auto firstFrame = SendRequest(first.Get(), 1.0f);
        Check(gate.entered.try_acquire_for(4s), "first job did not start");

        auto cancelled = Connect(port);
        SendRequest(cancelled.Get(), 2.0f);
        WaitFor([&] { return Inspect::Inspect(server).queued == 1; }, "cancellation test job was not queued");
        auto pipeline = Connect(port);
        const auto second = MakeRequest(3.0f);
        const auto third = MakeRequest(6.0f);
        std::array<char, Frame{}.size() * 2> frames{};
        std::copy(second.begin(), second.end(), frames.begin());
        std::copy(third.begin(), third.end(), frames.begin() + second.size());
        SendBytes(pipeline.Get(), frames);
        WaitFor([&] { return Inspect::Inspect(server).queued == 2; }, "queue did not fill");

        auto rejected = Connect(port);
        SendRequest(rejected.Get(), 4.0f);
        CheckPeerClosed(rejected.Get());
        Check(Inspect::Inspect(server).queued == 2, "queue-full rejection displaced a job");
        std::array<double, 16> probeMs{};
        const auto probesStarted = std::chrono::steady_clock::now();
        for (auto& ms : probeMs) {
            const auto startedAt = std::chrono::steady_clock::now();
            auto malformed = Connect(port);
            SendBytes(malformed.Get(), std::array<char, Protocol::HeaderBytes>{});
            CheckPeerClosed(malformed.Get());
            ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt).count();
        }
        const auto probeSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - probesStarted).count();
        std::sort(probeMs.begin(), probeMs.end());
        const auto blocked = Inspect::Inspect(server);
        Check(blocked.running == 1 && blocked.queued == 2, "probe workload escaped the blocked inference gate");
        std::cout << "{\"event\":\"blocked_inference_io_probe\",\"count\":" << probeMs.size()
                  << ",\"probes_per_sec\":" << probeMs.size() / probeSeconds << ",\"p50_ms\":" << probeMs[7]
                  << ",\"p95_ms\":" << probeMs[15] << ",\"p99_ms\":" << probeMs[15] << "}\n";

        SetOption(cancelled.Get(), SO_LINGER, linger{1, 0});
        cancelled.Close();
        WaitFor([&] { return Inspect::Inspect(server).queued == 1; }, "reset did not release a queue slot");
        auto next = Connect(port);
        const auto nextFrame = SendRequest(next.Get(), 5.0f);
        WaitFor([&] { return Inspect::Inspect(server).queued == 2; }, "freed queue slot was not reusable");
        Check(shutdown(pipeline.Get(), SD_SEND) == 0, "could not half-close pipelined client");

        gate.release.set_value();
        CheckResponse(first.Get(), firstFrame);
        CheckResponse(pipeline.Get(), second);
        CheckResponse(pipeline.Get(), third);
        CheckPeerClosed(pipeline.Get());
        CheckResponse(next.Get(), nextFrame);
        server.Stop();
        Check(started == std::vector<float>{1.0f, 3.0f, 5.0f, 6.0f}, "scheduler violated FIFO or started a rejected job");
        Check(Inspect::Stopped(server), "queue test retained resources");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        std::cout << "Inference FIFO, overload, queued cancellation, IOCP progress, half-close and response ordering passed.\n";
    }

    void TestConcurrentInferenceAndReset() {
        InferenceGate gate;
        IocpServer server("127.0.0.1", 0, 1, 1, [&](const std::vector<float>& input) {
            gate.Block();
            return input;
        }, {}, IocpServer::Clock::now, nullptr, {2, 1});
        Check(server.Start(), "concurrent inference server could not start");
        auto reset = Connect(server.BoundPort());
        SendRequest(reset.Get(), 1.0f);
        Check(gate.entered.try_acquire_for(4s), "first inference worker did not start");
        auto healthy = Connect(server.BoundPort());
        const auto frame = SendRequest(healthy.Get(), 2.0f);
        Check(gate.entered.try_acquire_for(4s), "second inference worker did not run concurrently");
        SetOption(reset.Get(), SO_LINGER, linger{1, 0});
        reset.Close();
        WaitFor([&] {
            const auto state = Inspect::Inspect(server);
            return state.running == 2 && state.closed == 1 && state.metrics.pending_io_operations == 0;
        }, "running reset did not close the socket and retain the job");
        gate.release.set_value();
        CheckResponse(healthy.Get(), frame);
        healthy.Close();
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "running reset left an undrained connection");
        server.Stop();
        Check(Inspect::Stopped(server), "concurrent inference test retained resources");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        std::cout << "Concurrent inference and a reset during inference passed.\n";
    }

    void TestInferenceQueueShutdownAndRestart() {
        InferenceGate gate;
        std::vector<float> started;
        IocpServer server("127.0.0.1", 0, 1, 1, [&](const std::vector<float>& input) {
            started.push_back(input.front());
            if (input.front() == 1.0f) gate.Block();
            return input;
        }, {}, IocpServer::Clock::now, nullptr, {1, 2});
        Check(server.Start(), "queue shutdown server could not start");
        auto running = Connect(server.BoundPort());
        SendRequest(running.Get(), 1.0f);
        Check(gate.entered.try_acquire_for(4s), "shutdown test job did not start");
        auto queued = Connect(server.BoundPort());
        SendRequest(queued.Get(), 2.0f);
        WaitFor([&] { return Inspect::Inspect(server).queued == 1; }, "shutdown test job did not queue");
        std::atomic<bool> stopped{false};
        std::jthread stop([&] {
            server.Stop();
            stopped.store(true);
        });
        CheckPeerClosed(queued.Get());
        CheckPeerClosed(running.Get());
        const auto draining = Inspect::Inspect(server);
        Check(draining.queued == 0 && draining.running == 1 && draining.metrics.pending_io_operations == 0 &&
              !stopped.load(), "shutdown failed to cancel queued work or wait for running work");
        gate.release.set_value();
        stop.join();
        Check(Inspect::Stopped(server), "scheduler did not drain during shutdown");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);

        Check(server.Start(), "scheduler could not restart");
        auto restarted = Connect(server.BoundPort());
        CheckResponse(restarted.Get(), SendRequest(restarted.Get(), 3.0f));
        server.Stop();
        server.Stop();
        Check(started == std::vector<float>{1.0f, 3.0f}, "cancelled job ran during shutdown or restart");
        Check(Inspect::Stopped(server), "scheduler restart retained resources");
        CheckDrainedMetrics(server, ConnectionLimits{}.maxConnections);
        std::cout << "Shutdown with queued and running inference, restart and cleanup passed.\n";
    }

    void TestEarlyCompletionDuringSendSubmission(bool stopDuringSubmission) {
        InferenceGate gate;
        ManualClock clock;
        IocpServer server("127.0.0.1", 0, 2, 1, Echo, TestLimits, [&] { return clock.Now(); });
        Inspect::AfterSendSubmission(server, [&] {
            gate.entered.release();
            (void)gate.ready.wait_for(15s);
        });
        Check(server.Start(), "early-completion server could not start");
        auto client = Connect(server.BoundPort());
        CheckResponse(client.Get(), SendRequest(client.Get(), 1.0f));
        Check(gate.entered.try_acquire_for(4s), "successful send did not reach the submission gate");
        WaitFor([&] { return Inspect::HasEarlyCompletion(server); }, "early send completion did not reach the gate");

        // The early packet must leave unrelated IOCP completions and the registry available.
        auto malformed = Connect(server.BoundPort());
        SendBytes(malformed.Get(), std::array<char, Protocol::HeaderBytes>{});
        CheckPeerClosed(malformed.Get());
        std::jthread stop;
        if (stopDuringSubmission) {
            stop = std::jthread([&] { server.Stop(); });
            WaitFor([&] { return Inspect::StopRequested(server); }, "Stop could not reach the registry during submission");
        } else {
            Inspect::SweepAt(server, clock, 100);
        }
        const auto reserved = Inspect::Inspect(server);
        Check(reserved.submitting == 1 && reserved.closed == 0 && reserved.metrics.pending_io_operations == 0,
              "a shutdown or timeout closed or accounted for a send before submission was published");
        gate.release.set_value();
        if (stop.joinable()) stop.join();
        CheckPeerClosed(client.Get());
        WaitFor([&] { return Inspect::Inspect(server).Empty(); }, "early send completion did not drain");
        const auto metrics = server.GetConnectionMetrics();
        Check(metrics.cancellation_requests_total == 1 && metrics.send_timeouts_total == !stopDuringSubmission,
              "deferred socket closure did not account for shutdown or timeout exactly once");
        server.Stop();
        Check(Inspect::Stopped(server), "early completion retained resources");
        CheckDrainedMetrics(server, TestLimits.maxConnections);
        std::cout << (stopDuringSubmission ? "Shutdown" : "Timeout")
                  << " raced safely with a send submission and early completion.\n";
    }
}

int main(int argc, char** argv) {
    try {
        {
            Winsock winsock;
            if (argc == 2 && std::string_view(argv[1]) == "--connection-policies") {
                TestAdmissionAndRecovery();
                TestShutdownAtCapacity();
                TestIdleExpiry();
                TestFrameExpiry(3);
                TestFrameExpiry(Protocol::HeaderBytes + 2);
                TestIncrementalFrame();
                TestFrameCompletionBoundary(59);
                TestFrameCompletionBoundary(60);
                std::cout << "Partial-header and partial-payload expiry, valid fragmentation, and late completion passed.\n";
                TestInferenceAndBufferedFrameTiming();
                TestSendExpiry();
                TestTimeoutRacingWithShutdown();
                TestTimeoutRacingWithPeerReset();
                TestWallClockSweep();
                std::cout << "Connection metrics accounting passed.\n";
            } else if (argc == 2 && std::string_view(argv[1]) == "--inference-scheduler") {
                TestInferenceQueue();
                TestConcurrentInferenceAndReset();
                TestInferenceQueueShutdownAndRestart();
                TestEarlyCompletionDuringSendSubmission(true);
                TestEarlyCompletionDuringSendSubmission(false);
            } else {
                TestReceiveDisconnect();
                TestSendDisconnect();
                TestCancelActiveIo();
                TestConcurrentStopDuringInference();
                TestPartialStartup();
                TestWorkerLaunchRollback();
                TestConcurrentChurn();
            }
        }
        const SocketHandle afterCleanup(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        Check(afterCleanup.Get() == INVALID_SOCKET && WSAGetLastError() == WSANOTINITIALISED,
              "Winsock reference leaked");
        std::cout << "Winsock startup and cleanup references balanced.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
