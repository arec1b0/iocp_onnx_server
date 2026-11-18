#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "Connection.h"
#include "ConnectionLimits.h"
#include "ConnectionMetrics.h"
#include "ConnectionTrace.h"

/// Asynchronous TCP inference server built on Windows I/O Completion Ports.
///
/// Runs one accept thread plus a pool of worker threads
/// that service completions from a shared IOCP handle. Workers reassemble
/// length-prefixed float requests (see docs/PROTOCOL.md), execute inference
/// synchronously through the supplied inference function, and post overlapped
/// sends with the result. The inference function must support concurrent calls.
class IocpServer {
public:
    using Inference = std::function<std::vector<float>(const std::vector<float>&)>;
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;

    /// The clock must be monotonic and safe to call from multiple threads.
    IocpServer(const std::string& ipAddress, int port, int workerThreads,
               std::size_t inputElements, Inference inference, ConnectionLimits limits = {}, Now now = Clock::now,
               ConnectionTrace* trace = nullptr);

    /// Stops the server if still running.
    ~IocpServer();

    /// Initializes WinSock, creates the completion port and listening socket,
    /// and launches the accept and worker threads.
    /// A failed start releases resources and allows another start attempt.
    bool Start();

    /// Stops admission, closes active sockets, and waits for I/O and workers
    /// to drain. Concurrent lifecycle calls are serialized.
    void Stop();
    /// Returns the bound port while started, including an OS-assigned port.
    int BoundPort() const;
    /// Copies all gauges and counters at one registry-lock boundary.
    ConnectionMetricsSnapshot GetConnectionMetrics() const;

private:
    friend struct IocpServerTestAccess;

    struct ActiveConnection {
        std::unique_ptr<Connection> owner;
        IOContext* pending = nullptr;
        Clock::time_point deadline{};
        bool receivingFrame = false;
        ConnectionTrace::Peer peer;
    };
    using ConnectionRegistry = std::unordered_map<Connection*, ActiveConnection>;

    bool OpenListener();
    void StopLocked();
    void WorkerThread();
    void AcceptThread();
    void Admit(SocketHandle socket, const sockaddr_in* remote = nullptr);
    void ProcessPacket(Connection* conn, OVERLAPPED* overlapped, DWORD error, DWORD bytesTransferred);
    IOContext* HandleCompletion(Connection& conn, IOOperation operation, DWORD bytesTransferred);

    // These require the registry lock. A reserved operation pins its entry
    // until the worker dequeues the completion or posting fails immediately.
    bool PostLocked(ActiveConnection& active, IOContext& context, Clock::time_point observedAt);
    bool CloseActiveSocketLocked(ActiveConnection& active);
    void RecordTimeoutLocked(const ActiveConnection& active, IOOperation operation, const char* phase, DWORD bytes = 0);
    void TraceLocked(ConnectionTrace::Peer peer, const char* event, const char* phase, const char* operation,
                     DWORD code = 0, DWORD bytes = 0) const noexcept;
    void CloseExpiredLocked(Clock::time_point now);
    void RetireLocked(ConnectionRegistry::iterator entry);

    std::string ipAddress_;
    int port_;
    int workerThreads_;
    SOCKET listenSocket_;
    HANDLE iocpHandle_;
    int boundPort_ = 0;
    bool winsockStarted_ = false;

    std::vector<std::thread> workers_;
    std::thread acceptThread_;
#ifdef IOCP_TEST_THREAD_FAILURE
    int failWorkerLaunchAt_ = -1;
#endif
    const std::size_t inputElements_;
    const Inference inference_;
    const ConnectionLimits limits_;
    const Now now_;
    ConnectionTrace* const trace_;

    mutable std::mutex lifecycleMutex_;
    std::atomic<bool> stopping_{true};
    mutable std::mutex registryMutex_;
    std::condition_variable registryDrained_;
    ConnectionRegistry connections_;
    ConnectionMetricsSnapshot connectionMetrics_;
};
