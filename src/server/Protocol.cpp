#include "Protocol.h"
#include <bit>
#include <cstring>

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(std::uint64_t) == Protocol::HeaderBytes);
static_assert(sizeof(float) == Protocol::FloatBytes && std::numeric_limits<float>::is_iec559);

Protocol::RequestFramer::RequestFramer(std::size_t expectedPayloadBytes)
    : expectedPayloadBytes_(expectedPayloadBytes) {}

Protocol::ReadResult Protocol::RequestFramer::Consume(
    std::span<const char>& bytes, std::vector<float>& input) {
    const std::size_t expectedElements = expectedPayloadBytes_ / FloatBytes;
    if (!readingPayload_) {
        if (bytes.size() < HeaderBytes) {
            return ReadResult::Incomplete;
        }

        std::uint64_t wireCount;
        std::memcpy(&wireCount, bytes.data(), HeaderBytes);
        if (wireCount != expectedElements) {
            return ReadResult::Invalid;
        }

        bytes = bytes.subspan(HeaderBytes);
        readingPayload_ = true;
    }

    const auto missing = expectedPayloadBytes_ - payload_.size();
    if (bytes.size() < missing) {
        if (!bytes.empty()) {
            payload_.insert(payload_.end(), bytes.begin(), bytes.end());
            bytes = bytes.subspan(bytes.size());
        }
        return ReadResult::Incomplete;
    }

    input.resize(expectedElements);
    auto* output = reinterpret_cast<char*>(input.data());
    if (!payload_.empty()) {
        std::memcpy(output, payload_.data(), payload_.size());
    }
    if (missing != 0) {
        std::memcpy(output + payload_.size(), bytes.data(), missing);
    }
    bytes = bytes.subspan(missing);
    payload_.clear();
    readingPayload_ = false;
    return ReadResult::Complete;
}
