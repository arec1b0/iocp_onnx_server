#include "IocpServer.h"
#include <ws2tcpip.h>
#include <cstring>
#include <chrono>
#include <numeric>
#include <cmath>
#include "../utils/Logger.h"
#include "../utils/Metrics.h"

IocpServer::IocpServer(const std::string& ipAddress, int port, int workerThreads, OnnxModel* model)
    : ipAddress_(ipAddress), port_(port), workerThreads_(workerThreads),
      listenSocket_(INVALID_SOCKET), iocpHandle_(NULL), isRunning_(false), model_(model) {}

IocpServer::~IocpServer() {
    Stop();
}

bool IocpServer::Start() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LOG_ERR("WSAStartup failed.");
        return false;
    }

    iocpHandle_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, workerThreads_);
    if (iocpHandle_ == NULL) {
        LOG_ERR("CreateIoCompletionPort failed.");
        return false;
    }

    listenSocket_ = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (listenSocket_ == INVALID_SOCKET) {
        LOG_ERR("WSASocket failed.");
        return false;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port_);
    inet_pton(AF_INET, ipAddress_.c_str(), &serverAddr.sin_addr);

    if (bind(listenSocket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        LOG_ERR("Bind failed.");
        return false;
    }

    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        LOG_ERR("Listen failed.");
        return false;
    }

    isRunning_ = true;

    for (int i = 0; i < workerThreads_; ++i) {
        workers_.emplace_back(&IocpServer::WorkerThread, this);
    }

    acceptThread_ = std::thread(&IocpServer::AcceptThread, this);

    LOG_INFO("IOCP Server started on " + ipAddress_ + ":" + std::to_string(port_));
    return true;
}

void IocpServer::Stop() {
    isRunning_ = false;

    if (listenSocket_ != INVALID_SOCKET) {
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    if (iocpHandle_ != NULL) {
        for (int i = 0; i < workerThreads_; ++i) {
            PostQueuedCompletionStatus(iocpHandle_, 0, 0, NULL);
        }
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    if (iocpHandle_ != NULL) {
        CloseHandle(iocpHandle_);
        iocpHandle_ = NULL;
    }

    WSACleanup();
    LOG_INFO("IOCP Server stopped.");
}

void IocpServer::AcceptThread() {
    while (isRunning_) {
        sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSocket = WSAAccept(listenSocket_, (sockaddr*)&clientAddr, &clientAddrLen, NULL, 0);

        if (clientSocket == INVALID_SOCKET) {
            if (isRunning_) LOG_ERR("WSAAccept failed.");
            continue;
        }

        Connection* conn = new Connection(clientSocket);
        CreateIoCompletionPort((HANDLE)clientSocket, iocpHandle_, (ULONG_PTR)conn, 0);

        DWORD flags = 0;
        DWORD bytesReceived = 0;
        IOContext* readCtx = conn->GetReadContext();
        readCtx->Reset();

        if (WSARecv(clientSocket, &readCtx->wsaBuf, 1, &bytesReceived, &flags, &readCtx->overlapped, NULL) == SOCKET_ERROR) {
            if (WSAGetLastError() != WSA_IO_PENDING) {
                LOG_ERR("Initial WSARecv failed.");
                delete conn;
            }
        }
    }
}

void IocpServer::WorkerThread() {
    DWORD bytesTransferred;
    ULONG_PTR completionKey;
    LPOVERLAPPED overlapped;

    while (isRunning_) {
        BOOL result = GetQueuedCompletionStatus(iocpHandle_, &bytesTransferred, &completionKey, &overlapped, INFINITE);

        if (completionKey == 0 && bytesTransferred == 0 && overlapped == NULL) {
            break; 
        }

        Connection* conn = reinterpret_cast<Connection*>(completionKey);
        if (!conn) continue;

        IOContext* context = reinterpret_cast<IOContext*>(overlapped);

        if (!result || bytesTransferred == 0) {
            delete conn;
            continue;
        }

        if (context->operation == IOOperation::Read) {
            conn->rxBuffer.insert(conn->rxBuffer.end(), context->buffer.data(), context->buffer.data() + bytesTransferred);

            bool keepProcessing = true;
            bool responseSent = false;

            while (keepProcessing) {
                if (conn->state == ClientState::ReadHeader) {
                    if (conn->rxBuffer.size() >= sizeof(size_t)) {
                        std::memcpy(&conn->expectedElements, conn->rxBuffer.data(), sizeof(size_t));
                        conn->rxBuffer.erase(conn->rxBuffer.begin(), conn->rxBuffer.begin() + sizeof(size_t));
                        conn->state = ClientState::ReadPayload;
                    } else {
                        keepProcessing = false;
                    }
                }

                if (conn->state == ClientState::ReadPayload) {
                    size_t expectedBytes = conn->expectedElements * sizeof(float);
                    if (conn->rxBuffer.size() >= expectedBytes) {
                        std::vector<float> inputTensor(conn->expectedElements);
                        std::memcpy(inputTensor.data(), conn->rxBuffer.data(), expectedBytes);
                        conn->rxBuffer.erase(conn->rxBuffer.begin(), conn->rxBuffer.begin() + expectedBytes);

                        auto start_time = std::chrono::high_resolution_clock::now();
                        std::vector<float> outputTensor = model_->RunInference(inputTensor);
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

                        IOContext* writeCtx = conn->GetWriteContext();
                        writeCtx->Reset();

                        size_t outElementCount = outputTensor.size();
                        size_t outTotalBytes = sizeof(size_t) + (outElementCount * sizeof(float));

                        if (writeCtx->buffer.size() < outTotalBytes) {
                            writeCtx->buffer.resize(outTotalBytes);
                        }
                        
                        writeCtx->wsaBuf.buf = writeCtx->buffer.data();
                        writeCtx->wsaBuf.len = static_cast<ULONG>(outTotalBytes);

                        std::memcpy(writeCtx->wsaBuf.buf, &outElementCount, sizeof(size_t));
                        std::memcpy(writeCtx->wsaBuf.buf + sizeof(size_t), outputTensor.data(), outElementCount * sizeof(float));

                        DWORD bytesSent = 0;
                        if (WSASend(conn->GetSocket(), &writeCtx->wsaBuf, 1, &bytesSent, 0, &writeCtx->overlapped, NULL) == SOCKET_ERROR) {
                            if (WSAGetLastError() != WSA_IO_PENDING) {
                                delete conn;
                                responseSent = true; 
                                keepProcessing = false;
                                break;
                            }
                        }
                        
                        conn->state = ClientState::ReadHeader;
                        responseSent = true;
                        keepProcessing = false; 
                    } else {
                        keepProcessing = false;
                    }
                }
            }

            if (!responseSent) {
                DWORD flags = 0;
                DWORD bytesReceived = 0;
                IOContext* readCtx = conn->GetReadContext();
                readCtx->Reset();

                if (WSARecv(conn->GetSocket(), &readCtx->wsaBuf, 1, &bytesReceived, &flags, &readCtx->overlapped, NULL) == SOCKET_ERROR) {
                    if (WSAGetLastError() != WSA_IO_PENDING) {
                        delete conn;
                    }
                }
            }
        }
        else if (context->operation == IOOperation::Write) {
            DWORD flags = 0;
            DWORD bytesReceived = 0;
            IOContext* readCtx = conn->GetReadContext();
            readCtx->Reset();

            if (WSARecv(conn->GetSocket(), &readCtx->wsaBuf, 1, &bytesReceived, &flags, &readCtx->overlapped, NULL) == SOCKET_ERROR) {
                if (WSAGetLastError() != WSA_IO_PENDING) {
                    delete conn;
                }
            }
        }
    }
}