#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include "Connection.h"
#include "../inference/OnnxModel.h"

/// Asynchronous TCP inference server built on Windows I/O Completion Ports.
///
/// Runs one accept thread (blocking WSAAccept) plus a pool of worker threads
/// that service completions from a shared IOCP handle. Workers reassemble
/// length-prefixed float requests (see docs/PROTOCOL.md), execute inference
/// synchronously on the provided OnnxModel, and post overlapped sends with
/// the result.
///
/// The model pointer is not owned; it must outlive the server and its
/// RunInference must be safe to call concurrently from multiple workers.
class IocpServer {
public:
    /// @param ipAddress      IPv4 address to bind (e.g. "0.0.0.0").
    /// @param port           TCP port to listen on.
    /// @param workerThreads  Size of the IOCP worker pool.
    /// @param model          Loaded model used for inference (not owned).
    IocpServer(const std::string& ipAddress, int port, int workerThreads, OnnxModel* model);

    /// Stops the server if still running.
    ~IocpServer();

    /// Initializes WinSock, creates the completion port and listening socket,
    /// and launches the accept and worker threads.
    /// @return true on success; on failure the server is unusable.
    bool Start();

    /// Graceful shutdown: closes the listener (unblocking the accept thread),
    /// wakes each worker with a sentinel completion packet, joins all
    /// threads, and releases WinSock resources. Safe to call more than once.
    void Stop();

private:
    /// Worker loop: dequeues completions, drives the per-connection request
    /// state machine, runs inference, and posts responses.
    void WorkerThread();

    /// Accept loop: accepts clients, wraps each in a Connection, associates
    /// the socket with the completion port, and posts the initial WSARecv.
    void AcceptThread();

    std::string ipAddress_;
    int port_;
    int workerThreads_;
    SOCKET listenSocket_;
    HANDLE iocpHandle_;
    bool isRunning_;

    std::vector<std::thread> workers_;
    std::thread acceptThread_;
    OnnxModel* model_;
};
