#include "Connection.h"
#include <cstring>

static_assert(sizeof(ULONG) == sizeof(std::uint32_t));

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

Connection::Connection(SocketHandle socket, std::size_t expectedPayloadBytes)
    : socket_(std::move(socket)),
      readContext_(IOOperation::Read, 65536),
      writeContext_(IOOperation::Write, 65536),
      requestFramer_(expectedPayloadBytes) {}

SOCKET Connection::GetSocket() const {
    return socket_.Get();
}

IOContext* Connection::PrepareRead() {
    const auto retained = readEnd_ - readBegin_;
    if (retained != 0 && readBegin_ != 0) {
        auto* buffer = readContext_.buffer.data();
        std::memmove(buffer, buffer + readBegin_, retained);
    }
    readBegin_ = 0;
    readEnd_ = retained;
    readContext_.Reset();
    readContext_.wsaBuf.buf += retained;
    readContext_.wsaBuf.len -= static_cast<ULONG>(retained);
    return &readContext_;
}

Protocol::ReadResult Connection::ReadRequest(std::size_t bytesReceived, std::vector<float>& input) {
    if (bytesReceived > readContext_.buffer.size() - readEnd_) {
        return Protocol::ReadResult::Invalid;
    }

    readEnd_ += bytesReceived;
    return ReadBufferedRequest(input);
}

Protocol::ReadResult Connection::ReadBufferedRequest(std::vector<float>& input) {
    std::span<const char> bytes(readContext_.buffer.data() + readBegin_, readEnd_ - readBegin_);
    const auto result = requestFramer_.Consume(bytes, input);
    readBegin_ = readEnd_ - bytes.size();
    return result;
}

IOContext* Connection::PrepareResponse(std::span<const float> output) {
    if (sendTotalBytes_ != 0 || output.size() > Protocol::MaxPayloadBytes / Protocol::FloatBytes) {
        return nullptr;
    }

    const std::uint64_t elementCount = output.size();
    const auto payloadBytes = output.size_bytes();
    const auto frameBytes = Protocol::HeaderBytes + payloadBytes;
    auto& buffer = writeContext_.buffer;
    if (buffer.size() < frameBytes) {
        buffer.resize(frameBytes);
    }
    std::memcpy(buffer.data(), &elementCount, Protocol::HeaderBytes);
    if (payloadBytes != 0) {
        std::memcpy(buffer.data() + Protocol::HeaderBytes, output.data(), payloadBytes);
    }

    sendTotalBytes_ = frameBytes;
    return PrepareSendSuffix();
}

SendCompletion Connection::CompleteSend(bool succeeded, std::size_t bytesTransferred) {
    if (!succeeded || bytesTransferred == 0 || bytesTransferred > sendTotalBytes_ - sendTransferredBytes_) {
        return {false, nullptr};
    }

    sendTransferredBytes_ += bytesTransferred;
    if (sendTransferredBytes_ < sendTotalBytes_) {
        return {true, PrepareSendSuffix()};
    }

    sendTotalBytes_ = 0;
    sendTransferredBytes_ = 0;
    return {true, nullptr};
}

IOContext* Connection::PrepareSendSuffix() {
    writeContext_.Reset();
    writeContext_.wsaBuf.buf += sendTransferredBytes_;
    writeContext_.wsaBuf.len = static_cast<ULONG>(sendTotalBytes_ - sendTransferredBytes_);
    return &writeContext_;
}

void Connection::Close() {
    socket_.Close();
}
