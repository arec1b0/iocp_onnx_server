#pragma once
#include <winsock2.h>
#include <windows.h>
#include <vector>

/// Direction of an overlapped socket operation, used to dispatch completions.
enum class IOOperation { Read, Write };

/// Position in the request state machine: waiting for the 8-byte length
/// prefix, or accumulating the float payload it announced.
enum class ClientState { ReadHeader, ReadPayload };

/// Context for a single overlapped WinSock operation.
///
/// `overlapped` must remain the first member: workers cast the LPOVERLAPPED
/// returned by GetQueuedCompletionStatus back to an IOContext* to recover the
/// operation type and buffer.
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

/// Per-client connection state.
///
/// Owns the socket, one read and one write IOContext (at most one overlapped
/// operation is outstanding at a time), and the receive accumulation buffer
/// used to reassemble requests fragmented across TCP segments. A Connection*
/// is registered as the IOCP completion key for its socket; workers delete it
/// on I/O failure or peer disconnect, which closes the socket.
class Connection {
public:
    Connection(SOCKET socket);
    ~Connection();

    SOCKET GetSocket() const;
    IOContext* GetReadContext();
    IOContext* GetWriteContext();

    /// Closes the underlying socket (idempotent). Also called by the destructor.
    void Close();

    /// Current position in the header/payload state machine.
    ClientState state;
    /// Element count parsed from the last request header; valid in ReadPayload.
    size_t expectedElements;
    /// Accumulation buffer: bytes received so far but not yet consumed as a
    /// complete header or payload.
    std::vector<char> rxBuffer;

private:
    SOCKET socket_;
    IOContext readContext_;
    IOContext writeContext_;
};
