#include "IocpServer.h"
#include <ws2tcpip.h>
#include <cassert>
#include <chrono>
#include <numeric>
#include <cmath>
#include "../utils/Logger.h"
#include "../utils/Metrics.h"

namespace {
    constexpr DWORD ShutdownCheckMs = 100;
}

IocpServer::IocpServer(const std::string& ipAddress, int port, int workerThreads,
                       std::size_t inputElements, Inference inference)
    : ipAddress_(ipAddress), port_(port), workerThreads_(workerThreads),
      listenSocket_(INVALID_SOCKET), iocpHandle_(nullptr),
      inputElements_(inputElements), inference_(std::move(inference)) {}

IocpServer::~IocpServer() {
    Stop();
}

bool IocpServer::Start() {
    std::lock_guard lifecycle(lifecycleMutex_);
    if (winsockStarted_) {
        return true;
    }
    if (port_ < 0 || port_ > 65535 || workerThreads_ <= 0 || !inference_ ||
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

        stopping_.store(false);
        workers_.reserve(workerThreads_);
        for (int i = 0; i < workerThreads_; ++i) {
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
    }
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
            entry.second.owner->Close();
        }
        registryDrained_.wait(registry, [this] { return connections_.empty(); });
    }

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

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
    while (!stopping_.load()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenSocket_, &readable);
        const timeval timeout{0, ShutdownCheckMs * 1000};
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) {
            LOG_ERR("Listener readiness check failed.");
            return;
        }
        if (ready == 0 || stopping_.load()) {
            continue;
        }

        SocketHandle accepted(WSAAccept(listenSocket_, nullptr, nullptr, nullptr, 0));
        if (accepted.Get() == INVALID_SOCKET) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAECONNRESET) {
                LOG_ERR("WSAAccept failed: " + std::to_string(error));
            }
            continue;
        }
        try {
            Admit(std::move(accepted));
        } catch (const std::exception& error) {
            LOG_ERR("Could not admit connection: " + std::string(error.what()));
        }
    }
}

void IocpServer::Admit(SocketHandle socket) {
    auto owner = std::make_unique<Connection>(std::move(socket), inputElements_ * Protocol::FloatBytes);
    auto* conn = owner.get();
    std::lock_guard registry(registryMutex_);
    if (stopping_.load()) {
        return;
    }
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(conn->GetSocket()), iocpHandle_,
                                reinterpret_cast<ULONG_PTR>(conn), 0)) {
        LOG_ERR("Could not associate client socket with IOCP.");
        return;
    }
    auto entry = connections_.emplace(conn, ActiveConnection{std::move(owner)}).first;
    if (!PostLocked(entry->second, *conn->PrepareRead())) {
        RetireLocked(entry);
    }
}

bool IocpServer::PostLocked(ActiveConnection& active, IOContext& context) {
    assert(active.pending == nullptr);
    const auto socket = active.owner->GetSocket();
    if (stopping_.load() || socket == INVALID_SOCKET) {
        return false;
    }

    // Reserve before Winsock can queue a completion. The registry lock keeps
    // a dequeuing worker from claiming it until the submission returns.
    active.pending = &context;
    DWORD flags = 0;
    const int result = context.operation == IOOperation::Read
        ? WSARecv(socket, &context.wsaBuf, 1, nullptr, &flags, &context.overlapped, nullptr)
        : WSASend(socket, &context.wsaBuf, 1, nullptr, 0, &context.overlapped, nullptr);
    if (result != SOCKET_ERROR || WSAGetLastError() == WSA_IO_PENDING) {
        return true;
    }
    active.pending = nullptr;
    return false;
}

void IocpServer::RetireLocked(ConnectionRegistry::iterator entry) {
    assert(entry->second.pending == nullptr);
    connections_.erase(entry);
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
        if (overlapped) {
            ProcessPacket(reinterpret_cast<Connection*>(completionKey), overlapped, result != FALSE, bytesTransferred);
        } else if (stopping_.load()) {
            std::lock_guard registry(registryMutex_);
            if (connections_.empty()) {
                return;
            }
        }
    }
}

void IocpServer::ProcessPacket(Connection* conn, OVERLAPPED* overlapped, bool succeeded, DWORD bytesTransferred) {
    IOOperation operation;
    {
        std::lock_guard registry(registryMutex_);
        auto entry = connections_.find(conn);
        if (entry == connections_.end() || !entry->second.pending ||
            &entry->second.pending->overlapped != overlapped) {
            LOG_ERR("Received an IOCP packet with no matching pending operation.");
            return;
        }
        operation = entry->second.pending->operation;
        entry->second.pending = nullptr;
        if (stopping_.load() || !succeeded || bytesTransferred == 0) {
            RetireLocked(entry);
            return;
        }
    }

    // This worker alone owns protocol state until it hands the entry back.
    // Shutdown may close the socket but retains the entry while it is in use.
    IOContext* next = nullptr;
    try {
        next = HandleCompletion(*conn, operation, bytesTransferred);
    } catch (...) {
        LOG_ERR("Client handler failed.");
    }

    std::lock_guard registry(registryMutex_);
    auto entry = connections_.find(conn);
    assert(entry != connections_.end());
    if (!next || !PostLocked(entry->second, *next)) {
        RetireLocked(entry);
    }
}

IOContext* IocpServer::HandleCompletion(Connection& conn, IOOperation operation, DWORD bytesTransferred) {
    if (operation == IOOperation::Write) {
        const auto completion = conn.CompleteSend(true, bytesTransferred);
        if (!completion.succeeded) {
            return nullptr;
        }
        if (completion.remaining) {
            return completion.remaining;
        }
    }

    std::vector<float> inputTensor;
    const auto request = operation == IOOperation::Read
        ? conn.ReadRequest(bytesTransferred, inputTensor)
        : conn.ReadBufferedRequest(inputTensor);
    if (request == Protocol::ReadResult::Invalid) {
        return nullptr;
    }
    if (request == Protocol::ReadResult::Incomplete) {
        return conn.PrepareRead();
    }
    if (stopping_.load()) {
        return nullptr;
    }

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
    return stopping_.load() ? nullptr : conn.PrepareResponse(outputTensor);
}
