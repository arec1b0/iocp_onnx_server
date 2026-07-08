#include "Connection.h"

IOContext::IOContext(IOOperation op, size_t bufferSize) : operation(op) {
    ZeroMemory(&overlapped, sizeof(WSAOVERLAPPED));
    buffer.resize(bufferSize);
    wsaBuf.buf = buffer.data();
    wsaBuf.len = static_cast<ULONG>(buffer.size());
}

void IOContext::Reset() {
    ZeroMemory(&overlapped, sizeof(WSAOVERLAPPED));
    wsaBuf.buf = buffer.data();
    wsaBuf.len = static_cast<ULONG>(buffer.size());
}

Connection::Connection(SOCKET socket) 
    : socket_(socket), 
      readContext_(IOOperation::Read, 65536), 
      writeContext_(IOOperation::Write, 65536),
      state(ClientState::ReadHeader),
      expectedElements(0) {
    rxBuffer.reserve(131072); // 128KB accumulation buffer
}

Connection::~Connection() {
    Close();
}

SOCKET Connection::GetSocket() const {
    return socket_;
}

IOContext* Connection::GetReadContext() {
    return &readContext_;
}

IOContext* Connection::GetWriteContext() {
    return &writeContext_;
}

void Connection::Close() {
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}