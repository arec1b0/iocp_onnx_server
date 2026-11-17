#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Protocol {
    inline constexpr std::size_t HeaderBytes = 8;
    inline constexpr std::size_t FloatBytes = 4;
    inline constexpr std::uint32_t DefaultMaxPayloadBytes = 16 * 1024 * 1024;
    // WSABUF.len must hold the entire response frame, including the prefix.
    inline constexpr std::uint32_t MaxPayloadBytes =
        (std::numeric_limits<std::uint32_t>::max)() - HeaderBytes;

    enum class ReadResult { Incomplete, Invalid, Complete };

    class RequestFramer {
    public:
        // The caller supplies the model's checked input payload size.
        explicit RequestFramer(std::size_t expectedPayloadBytes);

        // Advances bytes past the data consumed for one request. The caller
        // retains an incomplete header or any bytes after a complete frame.
        ReadResult Consume(std::span<const char>& bytes, std::vector<float>& input);

    private:
        const std::size_t expectedPayloadBytes_;
        bool readingPayload_ = false;
        std::vector<char> payload_;
    };
}
