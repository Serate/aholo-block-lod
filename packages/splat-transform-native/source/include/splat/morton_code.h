#pragma once
#include <cstdint>

namespace splat::morton {

/// Expands the low 21 bits of `x` so that bit `k` becomes bit `3k`.
inline uint64_t expand3_21(uint32_t x) {
    uint64_t v = static_cast<uint64_t>(x & 0x1FFFFF);
    v = (v | v << 32) & UINT64_C(0x1F00000000FFFF);
    v = (v | v << 16) & UINT64_C(0x1F0000FF0000FF);
    v = (v | v << 8)  & UINT64_C(0x100F00F00F00F00F);
    v = (v | v << 4)  & UINT64_C(0x10C30C30C30C30C3);
    v = (v | v << 2)  & UINT64_C(0x1249249249249249);
    return v;
}

/// Computes a 64-bit Morton code from three signed 64-bit grid coordinates.
inline uint64_t morton_encode(int64_t x, int64_t y, int64_t z) {
    auto ux = static_cast<uint64_t>(x);
    auto uy = static_cast<uint64_t>(y);
    auto uz = static_cast<uint64_t>(z);
    return expand3_21(static_cast<uint32_t>(ux)) |
          (expand3_21(static_cast<uint32_t>(uy)) << 1) |
          (expand3_21(static_cast<uint32_t>(uz)) << 2);
}

} // namespace splat::morton
