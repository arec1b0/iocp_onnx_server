#pragma once
#include <chrono>
#include <cstdint>

struct ConnectionLimits {
    static constexpr std::uint32_t MaxConnectionCount = 65535;
    static constexpr std::uint32_t MaxTimeoutMs = 86400000;
    static constexpr std::uint32_t MaxCheckIntervalMs = 1000;

    std::uint32_t maxConnections = 256;
    std::chrono::milliseconds idleTimeout{60000};
    std::chrono::milliseconds frameTimeout{15000};
    std::chrono::milliseconds checkInterval{100};

    bool IsValid() const noexcept {
        using std::chrono::milliseconds;
        return maxConnections != 0 && maxConnections <= MaxConnectionCount &&
            idleTimeout >= milliseconds(1) && idleTimeout <= milliseconds(MaxTimeoutMs) &&
            frameTimeout >= milliseconds(1) && frameTimeout <= milliseconds(MaxTimeoutMs) &&
            checkInterval >= milliseconds(1) && checkInterval <= milliseconds(MaxCheckIntervalMs);
    }
};
