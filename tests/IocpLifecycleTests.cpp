#include "server/IocpServer.h"
#include <psapi.h>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <semaphore>
#include <stdexcept>

struct IocpServerTestAccess {
    struct Snapshot {
        std::size_t reads = 0;
        std::size_t writes = 0;
        std::size_t handlers = 0;
        std::size_t closed = 0;

        bool Empty() const { return reads == 0 && writes == 0 && handlers == 0; }
    };

    static Snapshot Inspect(IocpServer& server) {
        std::lock_guard lock(server.registryMutex_);
        Snapshot snapshot;
        for (const auto& entry : server.connections_) {
            const auto& active = entry.second;
            if (!active.pending) {
                ++snapshot.handlers;
            } else if (active.pending->operation == IOOperation::Read) {
                ++snapshot.reads;
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
            server.workers_.empty() && !server.acceptThread_.joinable() && Inspect(server).Empty();
    }

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

    SocketHandle Connect(int port, int receiveBuffer = 0) {
        auto client = OpenSocket();
        SetOption(client.Get(), SO_RCVTIMEO, SocketTimeoutMs);
        SetOption(client.Get(), SO_SNDTIMEO, SocketTimeoutMs);
        if (receiveBuffer > 0) {
            SetOption(client.Get(), SO_RCVBUF, receiveBuffer);
        }
        const auto address = Loopback(port);
        Check(connect(client.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
              "loopback connect failed");
        return client;
    }

    using Frame = std::array<char, Protocol::HeaderBytes + Protocol::FloatBytes>;

    Frame SendRequest(SOCKET socket, float value) {
        Frame frame{1};
        std::memcpy(frame.data() + Protocol::HeaderBytes, &value, sizeof(value));
        const auto size = static_cast<int>(frame.size());
        int offset = 0;
        while (offset < size) {
            const int sent = send(socket, frame.data() + offset, size - offset, 0);
            Check(sent > 0, "failed to send request");
            offset += sent;
        }
        return frame;
    }

    void CheckResponse(SOCKET socket, const Frame& expected) {
        Frame actual{};
        const auto size = static_cast<int>(actual.size());
        int offset = 0;
        while (offset < size) {
            const int received = recv(socket, actual.data() + offset, size - offset, 0);
            Check(received > 0, "failed to receive complete response");
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
        } catch (...) {
            release.release();
            throw;
        }
        release.release();
        stoppers.clear();
        Check(returned.load() == 4 && Inspect::Stopped(server), "concurrent Stop did not drain");
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
            sample = PrivateBytes();
        }
        server.Stop();
        Check(Inspect::Stopped(server), "stress server did not drain");
        std::cout << "Concurrent churn passed (1600 clients plus one marker per wave). "
                     "Private bytes after each drained wave:";
        for (const auto sample : samples) {
            std::cout << ' ' << sample;
        }
        std::cout << '\n';
    }
}

int main() {
    try {
        {
            Winsock winsock;
            TestReceiveDisconnect();
            TestSendDisconnect();
            TestCancelActiveIo();
            TestConcurrentStopDuringInference();
            TestPartialStartup();
            TestConcurrentChurn();
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
