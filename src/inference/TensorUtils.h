#pragma once
#include <vector>
#include <cstdint>

namespace TensorUtils {
    /// Total number of elements in a tensor of the given shape (product of
    /// all dimensions). Returns 0 for an empty shape. Dimensions are assumed
    /// positive — pin dynamic (-1) dims before calling.
    size_t GetTensorElementsCount(const std::vector<int64_t>& shape);
}
