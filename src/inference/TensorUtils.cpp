#include "TensorUtils.h"
#include <algorithm>

std::optional<std::size_t> TensorUtils::GetTensorElementsCount(
    const std::vector<std::int64_t>& shape, std::size_t maxElements) {
    if (shape.empty() || std::find(shape.begin(), shape.end(), 0) != shape.end()) {
        return 0;
    }

    std::size_t count = 1;
    for (const auto dimension : shape) {
        if (dimension < 0 || static_cast<std::uint64_t>(dimension) > maxElements / count) {
            return std::nullopt;
        }
        count *= static_cast<std::size_t>(dimension);
    }
    return count;
}
