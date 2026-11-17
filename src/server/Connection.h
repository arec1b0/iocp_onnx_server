#pragma once
#include <winsock2.h>
#include <windows.h>
#include <vector>
#include "Protocol.h"
#include "SocketHandle.h"

/// Direction of an overlapped socket operation, used to dispatch completions.
enum class IOOperation { Read, Write };

/// Context for a single overlapped WinSock operation.
struct IOContext {
    WSAOVERLAPPED overlapped;
    IOOperation operation;
    WSABUF wsaBuf;
    std::vector<char> buffer;

    /// @param op          Operation this context is dedicated to.
    /// @param bufferSize  Fixed I/O buffer size in bytes.
    IOContext(IOOperation op, size_t bufferSize);

    /// Re-arms the context for the next overlapped call: zeroes the
    /// OVERLAPPED and points wsaBuf back at the full buffer.
    void Reset();
};

struct SendCompletion {
    bool succeeded;
    // Present when a successful completion leaves bytes to send.
    IOContext* remaining;
};

/// Per-client connection state.
///
/// Owns the socket, both I/O contexts, and the request framer. The server
/// registry keeps the connection alive from admission through final completion.
class Connection {
public:
    Connection(SocketHandle socket, std::size_t expectedPayloadBytes);
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    SOCKET GetSocket() const;
    IOContext* PrepareRead();
    Protocol::ReadResult ReadRequest(std::size_t bytesReceived, std::vector<float>& input);
    Protocol::ReadResult ReadBufferedRequest(std::vector<float>& input);
    bool HasPartialRequest() const noexcept;

    /// Encodes a response and prepares its first send. Rejects a new response
    /// while a previous response is in progress.
    IOContext* PrepareResponse(std::span<const float> output);
    /// Called only after dequeuing the write completion. On failure the caller
    /// closes the connection. On success it posts the suffix or inspects the buffered stream.
    SendCompletion CompleteSend(bool succeeded, std::size_t bytesTransferred);

    /// Idempotent. The server serializes socket closure with I/O posting.
    void Close();

private:
    IOContext* PrepareSendSuffix();

    SocketHandle socket_;
    IOContext readContext_;
    IOContext writeContext_;
    Protocol::RequestFramer requestFramer_;
    std::size_t readBegin_ = 0;
    std::size_t readEnd_ = 0;
    std::size_t sendTotalBytes_ = 0;
    std::size_t sendTransferredBytes_ = 0;
};
