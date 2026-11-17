#pragma once
#include <cstddef>
#include <vector>
#include <cstdint>
#include <optional>

namespace TensorUtils {
    /// Total number of elements in a tensor of the given shape (product of
    /// all dimensions). Empty shapes retain the existing count of 0.
    /// Returns no value if the count exceeds maxElements. Pin dynamic
    /// dimensions before calling.
    std::optional<std::size_t> GetTensorElementsCount(
        const std::vector<std::int64_t>& shape, std::size_t maxElements);
}
