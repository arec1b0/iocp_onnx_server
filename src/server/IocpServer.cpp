#include "IocpServer.h"
#include <ws2tcpip.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <numeric>
#include <cmath>
#include <limits>
#include <iterator>
#include <new>
#ifdef IOCP_TEST_THREAD_FAILURE
#include <system_error>
#endif
#include "../utils/Logger.h"
#include "../utils/Metrics.h"

namespace {
    constexpr DWORD ShutdownCheckMs = 100;

    void IncrementTotal(std::uint64_t& counter) noexcept {
        if (counter != (std::numeric_limits<std::uint64_t>::max)()) {
            ++counter;
        }
    }

    const char* OperationName(IOOperation operation) noexcept {
        return operation == IOOperation::Read ? "recv" : "send";
    }
}

IocpServer::IocpServer(const std::string& ipAddress, int port, int workerThreads,
                       std::size_t inputElements, Inference inference, ConnectionLimits limits, Now now,
                       ConnectionTrace* trace, InferenceLimits inferenceLimits)
    : ipAddress_(ipAddress), port_(port), workerThreads_(workerThreads),
      listenSocket_(INVALID_SOCKET), iocpHandle_(nullptr),
      inputElements_(inputElements), inference_(std::move(inference)), limits_(limits), now_(std::move(now)),
      trace_(trace), inferenceLimits_(inferenceLimits) {}

IocpServer::~IocpServer() {
    Stop();
}

bool IocpServer::Start() {
    std::lock_guard lifecycle(lifecycleMutex_);
    if (winsockStarted_) {
        return true;
    }
    if (port_ < 0 || port_ > 65535 || workerThreads_ <= 0 || !inference_ || !now_ || !limits_.IsValid() ||
        !inferenceLimits_.IsValid() ||
        inputElements_ > Protocol::MaxPayloadBytes / Protocol::FloatBytes) {
        LOG_ERR("Invalid server configuration.");
        return false;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LOG_ERR("WSAStartup failed.");
        return false;
    }
    winsockStarted_ = true;

    try {
        iocpHandle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, workerThreads_);
        if (!iocpHandle_ || !OpenListener()) {
            if (!iocpHandle_) {
                LOG_ERR("CreateIoCompletionPort failed.");
            }
            StopLocked();
            return false;
        }

        peerEvents_ = WSACreateEvent();
        if (peerEvents_ == WSA_INVALID_EVENT) {
            LOG_ERR("Could not create the peer close event.");
            StopLocked();
            return false;
        }

        stopping_.store(false);
        inferenceWorkers_.reserve(inferenceLimits_.workers);
        for (std::uint32_t i = 0; i < inferenceLimits_.workers; ++i) {
            inferenceWorkers_.emplace_back(&IocpServer::InferenceThread, this);
        }
        workers_.reserve(workerThreads_);
        for (int i = 0; i < workerThreads_; ++i) {
#ifdef IOCP_TEST_THREAD_FAILURE
            if (i == failWorkerLaunchAt_) {
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            }
#endif
            workers_.emplace_back(&IocpServer::WorkerThread, this);
        }
        acceptThread_ = std::thread(&IocpServer::AcceptThread, this);
        LOG_INFO("IOCP Server started on " + ipAddress_ + ":" + std::to_string(boundPort_));
        return true;
    } catch (const std::exception& error) {
        StopLocked();
        LOG_ERR("Server startup failed: " + std::string(error.what()));
        return false;
    }
}

bool IocpServer::OpenListener() {
    listenSocket_ = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (listenSocket_ == INVALID_SOCKET) {
        LOG_ERR("WSASocket failed.");
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port_);
    if (inet_pton(AF_INET, ipAddress_.c_str(), &serverAddr.sin_addr) != 1) {
        LOG_ERR("Invalid IPv4 listen address.");
        return false;
    }

    if (bind(listenSocket_, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        LOG_ERR("Bind failed.");
        return false;
    }

    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERR("Listen failed.");
        return false;
    }

    u_long nonblocking = 1;
    if (ioctlsocket(listenSocket_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        LOG_ERR("Could not make the listener nonblocking.");
        return false;
    }
    int addrLength = sizeof(serverAddr);
    if (getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&serverAddr), &addrLength) == SOCKET_ERROR) {
        LOG_ERR("Could not query the bound address.");
        return false;
    }
    boundPort_ = ntohs(serverAddr.sin_port);
    return true;
}

int IocpServer::BoundPort() const {
    std::lock_guard lifecycle(lifecycleMutex_);
    return boundPort_;
}

ConnectionMetricsSnapshot IocpServer::GetConnectionMetrics() const {
    std::lock_guard registry(registryMutex_);
    auto snapshot = connectionMetrics_;
    snapshot.connections_active = connections_.size();
    return snapshot;
}

void IocpServer::Stop() {
    std::lock_guard lifecycle(lifecycleMutex_);
    StopLocked();
}

void IocpServer::StopLocked() {
    if (!winsockStarted_) {
        return;
    }

    {
        std::lock_guard registry(registryMutex_);
        stopping_.store(true);
        while (!inferenceQueue_.empty()) {
            CancelQueuedLocked(connections_.find(inferenceQueue_.front().connection));
        }
    }
    inferenceAvailable_.notify_all();
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    {
        std::unique_lock registry(registryMutex_);
        for (auto& entry : connections_) {
            if (trace_ && entry.second.owner->GetSocket() != INVALID_SOCKET) {
                TraceLocked(entry.second.peer, "shutdown_close", "stop", entry.second.pending ?
                            OperationName(entry.second.pending->operation) : "handler");
            }
            CloseActiveSocketLocked(entry.second);
        }
        registryDrained_.wait(registry, [this] { return connections_.empty(); });
    }

    for (auto& worker : inferenceWorkers_) {
        worker.join();
    }
    inferenceWorkers_.clear();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    if (peerEvents_ != WSA_INVALID_EVENT) {
        WSACloseEvent(peerEvents_);
        peerEvents_ = WSA_INVALID_EVENT;
    }
    if (iocpHandle_) {
        CloseHandle(iocpHandle_);
        iocpHandle_ = nullptr;
    }
    WSACleanup();
    winsockStarted_ = false;
    boundPort_ = 0;
    LOG_INFO("IOCP Server stopped.");
}

void IocpServer::AcceptThread() {
    auto nextTimeoutCheck = now_() + limits_.checkInterval;
    while (!stopping_.load()) {
        const auto now = now_();
        if (now >= nextTimeoutCheck) {
            std::lock_guard registry(registryMutex_);
            CloseExpiredLocked(now);
            nextTimeoutCheck = now + limits_.checkInterval;
        }

        using std::chrono::milliseconds;
        const auto wait = (std::min)(milliseconds(ShutdownCheckMs),
                                    std::chrono::ceil<milliseconds>(nextTimeoutCheck - now));
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenSocket_, &readable);
        const timeval timeout{0, static_cast<long>(wait.count() * 1000)};
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            {
                std::lock_guard registry(registryMutex_);
                IncrementTotal(connectionMetrics_.io_failures_total);
                if (trace_) TraceLocked({}, "socket_error", "select", "accept", error);
            }
            LOG_ERR("Listener readiness check failed.");
            return;
        }
        if (ready == 0 || stopping_.load()) {
            continue;
        }

        sockaddr_in remote{};
        int remoteSize = sizeof(remote);
        SocketHandle accepted(WSAAccept(listenSocket_, trace_ ? reinterpret_cast<sockaddr*>(&remote) : nullptr,
                                        trace_ ? &remoteSize : nullptr, nullptr, 0));
        if (accepted.Get() == INVALID_SOCKET) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAECONNRESET) {
                {
                    std::lock_guard registry(registryMutex_);
                    IncrementTotal(connectionMetrics_.io_failures_total);
                    if (trace_) TraceLocked({}, "socket_error", "winsock", "accept", error);
                }
                LOG_ERR("WSAAccept failed: " + std::to_string(error));
            } else if (trace_ && error == WSAECONNRESET) {
                std::lock_guard registry(registryMutex_);
                TraceLocked({}, "socket_error", "winsock", "accept", error);
            }
            continue;
        }
        try {
            Admit(std::move(accepted), trace_ ? &remote : nullptr);
        } catch (const std::exception& error) {
            LOG_ERR("Could not admit connection: " + std::string(error.what()));
        }
    }
}

void IocpServer::Admit(SocketHandle socket, const sockaddr_in* remote) {
    std::lock_guard registry(registryMutex_);
    const auto peer = trace_ ? trace_->Identify(socket.Get(), remote) : ConnectionTrace::Peer{};
    if (stopping_.load()) {
        if (trace_) TraceLocked(peer, "shutdown_reject", "admission", "accept");
        return;
    }
    if (connections_.size() >= limits_.maxConnections) {
        IncrementTotal(connectionMetrics_.connections_rejected_capacity_total);
        if (trace_) TraceLocked(peer, "capacity_reject", "admission", "accept");
        return;
    }
    // A shared event enables a separate FD_CLOSE record on each socket.
    // We enumerate the records without waiting on or resetting the event.
    if (WSAEventSelect(socket.Get(), peerEvents_, FD_CLOSE) == SOCKET_ERROR) {
        const auto error = WSAGetLastError();
        IncrementTotal(connectionMetrics_.io_failures_total);
        if (trace_) TraceLocked(peer, "socket_error", "associate", "close", error);
        return;
    }
    auto owner = std::make_unique<Connection>(std::move(socket), inputElements_ * Protocol::FloatBytes);
    auto* conn = owner.get();
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(conn->GetSocket()), iocpHandle_,
                                reinterpret_cast<ULONG_PTR>(conn), 0)) {
        const auto error = GetLastError();
        IncrementTotal(connectionMetrics_.io_failures_total);
        if (trace_) TraceLocked(peer, "socket_error", "associate", "accept", error);
        LOG_ERR("Could not associate client socket with IOCP.");
        return;
    }
    auto entry = connections_.emplace(conn, ActiveConnection{std::move(owner)}).first;
    entry->second.peer = peer;
    IncrementTotal(connectionMetrics_.connections_accepted_total);
    connectionMetrics_.connections_high_watermark =
        (std::max)(connectionMetrics_.connections_high_watermark, static_cast<std::uint64_t>(connections_.size()));
    if (trace_) TraceLocked(peer, "admitted", "admission", "accept");
    if (!PostLocked(entry->second, *conn->PrepareRead(), now_())) {
        RetireLocked(entry);
    }
}

bool IocpServer::PostLocked(ActiveConnection& active, IOContext& context, Clock::time_point observedAt) {
    assert(active.pending == nullptr);
    assert(active.activity == Activity::Network);
    const auto socket = active.owner->GetSocket();
    if (stopping_.load() || socket == INVALID_SOCKET) {
        return false;
    }

    const auto now = now_();
    const bool receivingFrame = context.operation == IOOperation::Read && active.owner->HasPartialRequest();
    if (!receivingFrame) {
        active.deadline = now + limits_.idleTimeout;
    } else if (!active.receivingFrame) {
        active.deadline = observedAt + limits_.frameTimeout;
    }
    active.receivingFrame = receivingFrame;
    if (now >= active.deadline) {
        RecordTimeoutLocked(active, context.operation, "post");
        return false;
    }

    // Reserve before Winsock can queue a completion. The registry lock keeps
    // a dequeuing worker from claiming it until the submission returns.
    active.pending = &context;
    DWORD flags = 0;
    const int result = context.operation == IOOperation::Read
        ? WSARecv(socket, &context.wsaBuf, 1, nullptr, &flags, &context.overlapped, nullptr)
        : WSASend(socket, &context.wsaBuf, 1, nullptr, 0, &context.overlapped, nullptr);
    const auto error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (result != SOCKET_ERROR || error == WSA_IO_PENDING) {
        ++connectionMetrics_.pending_io_operations;
        return true;
    }
    active.pending = nullptr;
    IncrementTotal(connectionMetrics_.io_failures_total);
    if (trace_) TraceLocked(active.peer, "socket_error", "post", OperationName(context.operation), error);
    return false;
}

bool IocpServer::QueueInferenceLocked(ActiveConnection& active, std::vector<float> input) {
    assert(active.pending == nullptr && active.activity == Activity::Network);
    if (stopping_.load()) {
        return false;
    }
    if (inferenceQueue_.size() >= inferenceLimits_.queueCapacity) {
        if (trace_) TraceLocked(active.peer, "inference_rejected", "scheduler", "queue_full");
        return false;
    }
    try {
        inferenceQueue_.push_back({active.owner.get(), std::move(input)});
    } catch (const std::bad_alloc&) {
        if (trace_) TraceLocked(active.peer, "inference_rejected", "scheduler", "allocation", ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    active.queued = std::prev(inferenceQueue_.end());
    active.activity = Activity::Queued;
    inferenceAvailable_.notify_one();
    return true;
}

bool IocpServer::PeerAbortedLocked(const ActiveConnection& active) {
    const auto socket = active.owner->GetSocket();
    if (socket == INVALID_SOCKET) {
        return true;
    }
    WSANETWORKEVENTS events{};
    int error = 0;
    if (WSAEnumNetworkEvents(socket, nullptr, &events) == SOCKET_ERROR) {
        error = WSAGetLastError();
    } else if (events.lNetworkEvents & FD_CLOSE) {
        error = events.iErrorCode[FD_CLOSE_BIT];
    }
    if (!error) {
        // A graceful FIN may be a client half-close with a response still due.
        return false;
    }
    IncrementTotal(connectionMetrics_.io_failures_total);
    if (trace_) TraceLocked(active.peer, "socket_error", "scheduler", "close", error);
    return true;
}

void IocpServer::CancelQueuedLocked(ConnectionRegistry::iterator entry) {
    auto& active = entry->second;
    assert(active.pending == nullptr && active.activity == Activity::Queued);
    inferenceQueue_.erase(active.queued);
    active.activity = Activity::Network;
    if (trace_) TraceLocked(active.peer, "inference_cancelled", "scheduler", "queued");
    RetireLocked(entry);
}

bool IocpServer::CloseActiveSocketLocked(ActiveConnection& active) {
    if (active.owner->GetSocket() == INVALID_SOCKET) {
        return false;
    }
    if (active.pending) {
        IncrementTotal(connectionMetrics_.cancellation_requests_total);
    }
    active.owner->Close();
    return true;
}

void IocpServer::RecordTimeoutLocked(const ActiveConnection& active, IOOperation operation, const char* phase, DWORD bytes) {
    const char* event;
    if (operation == IOOperation::Write) {
        IncrementTotal(connectionMetrics_.send_timeouts_total);
        event = "send_timeout";
    } else if (active.receivingFrame) {
        IncrementTotal(connectionMetrics_.incomplete_frame_timeouts_total);
        event = "frame_timeout";
    } else {
        IncrementTotal(connectionMetrics_.idle_timeouts_total);
        event = "idle_timeout";
    }
    if (trace_) TraceLocked(active.peer, event, phase, OperationName(operation), 0, bytes);
}

void IocpServer::TraceLocked(ConnectionTrace::Peer peer, const char* event, const char* phase, const char* operation,
                             DWORD code, DWORD bytes) const noexcept {
    trace_->Record(peer, event, phase, operation, code, bytes, connections_.size(),
                   connectionMetrics_.pending_io_operations, limits_.maxConnections);
}

void IocpServer::CloseExpiredLocked(Clock::time_point now) {
    if (stopping_.load()) {
        return;
    }
    for (auto next = connections_.begin(); next != connections_.end();) {
        const auto entry = next++;
        auto& active = entry->second;
        if (active.activity != Activity::Network && active.owner->GetSocket() != INVALID_SOCKET &&
            PeerAbortedLocked(active)) {
            if (active.activity == Activity::Queued) {
                CancelQueuedLocked(entry);
            } else {
                CloseActiveSocketLocked(active);
            }
            continue;
        }
        if (active.pending && now >= active.deadline && CloseActiveSocketLocked(active)) {
            RecordTimeoutLocked(active, active.pending->operation, "scan");
        }
    }
}

void IocpServer::RetireLocked(ConnectionRegistry::iterator entry) {
    assert(entry->second.pending == nullptr);
    assert(entry->second.activity == Activity::Network);
    const auto peer = entry->second.peer;
    connections_.erase(entry);
    IncrementTotal(connectionMetrics_.connections_closed_total);
    if (trace_) TraceLocked(peer, "retired", "registry", "none");
    if (connections_.empty()) {
        registryDrained_.notify_all();
    }
}

void IocpServer::WorkerThread() {
    for (;;) {
        DWORD bytesTransferred;
        ULONG_PTR completionKey;
        OVERLAPPED* overlapped;
        const BOOL result = GetQueuedCompletionStatus(
            iocpHandle_, &bytesTransferred, &completionKey, &overlapped, ShutdownCheckMs);
        const DWORD error = result ? ERROR_SUCCESS : GetLastError();
        if (overlapped) {
            ProcessPacket(reinterpret_cast<Connection*>(completionKey), overlapped, error, bytesTransferred);
        } else {
            const bool failed = error != ERROR_SUCCESS && error != WAIT_TIMEOUT;
            if (!failed && !stopping_.load()) {
                continue;
            }
            std::lock_guard registry(registryMutex_);
            if (failed) {
                IncrementTotal(connectionMetrics_.io_failures_total);
                if (trace_) TraceLocked({}, "dequeue_error", "completion", "none", error);
            }
            if (stopping_.load() && connections_.empty()) {
                return;
            }
        }
    }
}

void IocpServer::ProcessPacket(Connection* conn, OVERLAPPED* overlapped, DWORD error, DWORD bytesTransferred) {
    IOOperation operation;
    Clock::time_point observedAt;
    {
        std::lock_guard registry(registryMutex_);
        auto entry = connections_.find(conn);
        if (entry == connections_.end() || !entry->second.pending ||
            &entry->second.pending->overlapped != overlapped) {
            IncrementTotal(connectionMetrics_.io_failures_total);
            if (trace_) TraceLocked(entry == connections_.end() ? ConnectionTrace::Peer{} : entry->second.peer,
                                    "unmatched", "completion", "none", error, bytesTransferred);
            LOG_ERR("Received an IOCP packet with no matching pending operation.");
            return;
        }
        operation = entry->second.pending->operation;
        entry->second.pending = nullptr;
        assert(connectionMetrics_.pending_io_operations > 0);
        --connectionMetrics_.pending_io_operations;
        if (error == ERROR_OPERATION_ABORTED) {
            IncrementTotal(connectionMetrics_.cancellation_completions_total);
        } else if (error != ERROR_SUCCESS) {
            IncrementTotal(connectionMetrics_.io_failures_total);
        }
        observedAt = now_();
        if (stopping_.load() || error != ERROR_SUCCESS || bytesTransferred == 0 ||
            entry->second.owner->GetSocket() == INVALID_SOCKET) {
            if (trace_) TraceLocked(entry->second.peer, "terminal_completion", "completion",
                                    OperationName(operation), error, bytesTransferred);
            RetireLocked(entry);
            return;
        }
        if (observedAt >= entry->second.deadline) {
            RecordTimeoutLocked(entry->second, operation, "completion", bytesTransferred);
            RetireLocked(entry);
            return;
        }
    }

    // This worker alone owns protocol state until it hands the entry back.
    // Shutdown may close the socket but retains the entry while it is in use.
    CompletionAction action;
    try {
        action = HandleCompletion(*conn, operation, bytesTransferred);
    } catch (...) {
        LOG_ERR("Client handler failed.");
    }

    std::lock_guard registry(registryMutex_);
    auto entry = connections_.find(conn);
    assert(entry != connections_.end());
    if (action.input) {
        if (!QueueInferenceLocked(entry->second, std::move(*action.input))) {
            RetireLocked(entry);
        }
        return;
    }
    if (!action.next || !PostLocked(entry->second, *action.next, observedAt)) {
        if (trace_ && !action.next) TraceLocked(entry->second.peer, "handler_closed", "handler", OperationName(operation));
        RetireLocked(entry);
    }
}

IocpServer::CompletionAction IocpServer::HandleCompletion(Connection& conn, IOOperation operation, DWORD bytesTransferred) {
    if (operation == IOOperation::Write) {
        const auto completion = conn.CompleteSend(true, bytesTransferred);
        if (!completion.succeeded) {
            return {};
        }
        if (completion.remaining) {
            return {completion.remaining};
        }
    }

    std::vector<float> inputTensor;
    const auto request = operation == IOOperation::Read
        ? conn.ReadRequest(bytesTransferred, inputTensor)
        : conn.ReadBufferedRequest(inputTensor);
    if (request == Protocol::ReadResult::Invalid) {
        return {};
    }
    if (request == Protocol::ReadResult::Incomplete) {
        return {conn.PrepareRead()};
    }
    return {nullptr, std::move(inputTensor)};
}

void IocpServer::InferenceThread() {
    for (;;) {
        InferenceJob job{};
        {
            std::unique_lock registry(registryMutex_);
            inferenceAvailable_.wait(registry, [this] { return stopping_.load() || !inferenceQueue_.empty(); });
            if (stopping_.load()) {
                return;
            }
            const auto entry = connections_.find(inferenceQueue_.front().connection);
            assert(entry != connections_.end() && entry->second.activity == Activity::Queued);
            if (PeerAbortedLocked(entry->second)) {
                CancelQueuedLocked(entry);
                continue;
            }
            job = std::move(inferenceQueue_.front());
            inferenceQueue_.pop_front();
            entry->second.activity = Activity::Running;
        }

        // The Running state pins the connection and grants exclusive protocol
        // access until this worker returns it to a posted send or retires it.
        IOContext* response = nullptr;
        try {
            const auto output = RunInference(job.input);
            if (!stopping_.load()) response = job.connection->PrepareResponse(output);
        } catch (...) {
            LOG_ERR("Inference handler failed.");
        }

        std::lock_guard registry(registryMutex_);
        const auto entry = connections_.find(job.connection);
        assert(entry != connections_.end() && entry->second.activity == Activity::Running);
        auto& active = entry->second;
        active.activity = Activity::Network;
        if (!response || stopping_.load() || PeerAbortedLocked(active) || !PostLocked(active, *response, now_())) {
            RetireLocked(entry);
        }
    }
}

std::vector<float> IocpServer::RunInference(const std::vector<float>& inputTensor) {
    auto start_time = std::chrono::high_resolution_clock::now();
    std::vector<float> outputTensor = inference_(inputTensor);
    auto end_time = std::chrono::high_resolution_clock::now();

    double latency = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    float mean = 0.0f;
    float stddev = 0.0f;
    if (!inputTensor.empty()) {
        float sum = std::accumulate(inputTensor.begin(), inputTensor.end(), 0.0f);
        mean = sum / inputTensor.size();
        float sq_sum = std::inner_product(inputTensor.begin(), inputTensor.end(), inputTensor.begin(), 0.0f);
        float variance = (sq_sum / inputTensor.size()) - (mean * mean);
        stddev = variance > 0.0f ? std::sqrt(variance) : 0.0f;
    }

    InferenceStats stats{latency, mean, stddev, inputTensor.size()};
    Metrics::GetInstance().RecordInference(stats);
    return outputTensor;
}
