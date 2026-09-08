#pragma once

#include <cstdint>
#include <optional>

namespace platform {

constexpr std::optional<uint64_t> CheckedAdd(uint64_t a, uint64_t b) noexcept
{
    uint64_t sum = a + b;
    if (sum < a) {
        return std::nullopt; // wrapped
    }
    return sum;
}

constexpr std::optional<uint64_t> CheckedMultiply(uint64_t a, uint64_t b) noexcept
{
    if (a != 0 && b > UINT64_MAX / a) {
        return std::nullopt; // would overflow
    }
    return a * b;
}

} // namespace platform
