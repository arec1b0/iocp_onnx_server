#pragma once
#include <cstdint>

struct InferenceLimits {
    static constexpr std::uint32_t MaxWorkers = 64;
    static constexpr std::uint32_t MaxQueueCapacity = 4096;

    std::uint32_t workers = 4;
    std::uint32_t queueCapacity = 64;

    bool IsValid() const noexcept {
        return workers >= 1 && workers <= MaxWorkers && queueCapacity >= 1 && queueCapacity <= MaxQueueCapacity;
    }
};
