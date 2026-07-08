#pragma once
#include <cstdint>
#include <vector>

namespace splat {

/// Result of building a LOD tree for one block.
struct LodTreeResult {
    /// GS attributes (level-morton sorted, flattened for N-API transfer)
    std::vector<float> center;     // f32[3] × count
    std::vector<float> scale;      // f32[3] × count
    std::vector<float> quat;       // f32[4] × count
    std::vector<float> rgba;       // f32[4] × count
    std::vector<float> sh;         // f32[N] × count

    /// Tree structure (one entry per GS, even for leaf = count=0)
    std::vector<uint32_t> childStart;
    std::vector<uint16_t> childCount;

    size_t count = 0;        ///< Number of input GS (leaf nodes)
    size_t totalNodes = 0;   ///< totalNodes = count + internal nodes
    int shDegree = 0;
    bool lodTree = false;
};

/// Build a cycling_lod tree from raw GS arrays.
///
/// Input arrays are in AoS layout:
///   center[f32x3]  → {x0,y0,z0, x1,y1,z1, ...}
///   scale[f32x3]   → {sx0,sy0,sz0, ...}
///   quat[f32x4]    → {qx0,qy0,qz0,qw0, ...}
///   rgba[f32x4]    → {r0,g0,b0,a0, ...}
///   sh[f32xN]      → SH coefficients per GS
///
/// Returns tree structure + attributes in level-morton order.
LodTreeResult build_lod_tree(
    const float* center,
    const float* scale,
    const float* quat,
    const float* rgba,
    const float* sh,
    size_t count,
    int shDegree,
    const std::vector<float>& multipliers = {1.0f, 1.4f, 1.7f}
);

} // namespace splat
