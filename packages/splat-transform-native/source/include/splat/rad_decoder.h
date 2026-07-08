#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace splat {

/// Decoded result from a .rad file, matching the decodeRad N-API signature.
struct RadDecodeResult {
    // --- GS attributes (interleaved, "count" entries) ---

    /// f16[3] x count — raw half-float bits for GPU texture upload.
    std::vector<uint16_t> center;

    /// u8[4] x count — R, G, B, A packed.
    std::vector<uint8_t> rgba;

    /// u8[3] x count — ln0r8-encoded scale.
    std::vector<uint8_t> scale;

    /// u8[3] x count — oct88r8-encoded quaternion (xyz only; w is reconstructed).
    std::vector<uint8_t> quat;

    /// u8[N] x count — s8-encoded SH coefficients, empty if shDegree == 0.
    std::vector<uint8_t> sh;

    // --- Tree structure (one entry per tree-node) ---

    std::vector<uint32_t> childStart;
    std::vector<uint16_t> childCount;

    // --- Dimensions ---

    size_t count = 0;        // Gaussian splat count
    size_t totalNodes = 0;   // Tree node count (for LOD trees may be > count, but in practice equals count)
    size_t shDegree = 0;     // 0, 1, 2, or 3
};

/// Decodes a complete .rad file from a memory buffer.
///
/// Accepts the full file contents (header + all chunks concatenated).
RadDecodeResult decode_rad(const uint8_t* data, size_t size);

} // namespace splat
