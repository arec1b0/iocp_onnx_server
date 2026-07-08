#include "TensorUtils.h"
#include <numeric>
#include <functional>

size_t TensorUtils::GetTensorElementsCount(const std::vector<int64_t>& shape) {
    if (shape.empty()) return 0;
    return static_cast<size_t>(std::accumulate(shape.begin(), shape.end(), 1LL, std::multiplies<int64_t>()));
}