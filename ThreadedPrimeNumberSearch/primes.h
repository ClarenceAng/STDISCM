#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Inclusive range of integers [first, last]. Empty when first > last.
struct Range {
    std::uint64_t first = 1;
    std::uint64_t last = 0;

    bool empty() const { return first > last; }
    std::uint64_t size() const { return empty() ? 0 : last - first + 1; }
};

// Cuts [first, last] into `parts` contiguous chunks whose sizes differ by at
// most one and returns chunk number `index` (0-based). The leftover numbers go
// to the first chunks, e.g. [1, 10] in 4 parts -> 1-3, 4-6, 7-8, 9-10.
inline Range splitRange(std::uint64_t first, std::uint64_t last, std::uint64_t parts, std::uint64_t index) {
    if (first > last || parts == 0 || index >= parts) return {};

    const std::uint64_t total = last - first + 1;
    const std::uint64_t base = total / parts;
    const std::uint64_t extra = total % parts;
    const std::uint64_t size = base + (index < extra ? 1 : 0);
    if (size == 0) return {};

    const std::uint64_t start = first + index * base + std::min(index, extra);
    return {start, start + size - 1};
}

// floor(sqrt(n)), exact for all 64-bit n.
inline std::uint64_t isqrt(std::uint64_t n) {
    auto root = static_cast<std::uint64_t>(std::sqrt(static_cast<long double>(n)));
    while (root > 0 && root > n / root) --root;
    while (root + 1 <= n / (root + 1)) ++root;
    return root;
}

// Trial division by every d in [2, isqrt(n)]. The divisibility-testing variants
// split exactly this set of divisors across threads, so all four variants do
// the same arithmetic and their timings are comparable.
inline bool isPrime(std::uint64_t n) {
    if (n < 2) return false;
    const std::uint64_t root = isqrt(n);
    for (std::uint64_t d = 2; d <= root; ++d) {
        if (n % d == 0) return false;
    }
    return true;
}
