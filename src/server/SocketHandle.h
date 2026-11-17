#pragma once
#include <winsock2.h>
#include <utility>

class SocketHandle {
public:
    explicit SocketHandle(SOCKET socket = INVALID_SOCKET) noexcept : socket_(socket) {}
    ~SocketHandle() { Close(); }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept
        : socket_(std::exchange(other.socket_, INVALID_SOCKET)) {}

    SOCKET Get() const noexcept { return socket_; }

    void Close() noexcept {
        const auto socket = std::exchange(socket_, INVALID_SOCKET);
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
        }
    }

private:
    SOCKET socket_;
};
