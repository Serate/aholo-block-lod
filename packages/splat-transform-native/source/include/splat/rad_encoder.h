#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace splat {

/// Encodes a LodTreeResult (tree nodes + GS attributes sorted by level-morton)
/// into a complete .rad file buffer, byte-compatible with Spark's RAD format.
///
/// All attribute arrays are in AoS (interleaved) layout:
///   center[i*3+0..2] = x,y,z for GS i
///   scale[i*3+0..2]  = sx,sy,sz for GS i
///   quat[i*4+0..3]   = qx,qy,qz,qw for GS i  (xyzw order)
///   rgba[i*4+0..3]   = r,g,b,a for GS i
///
/// SH coefficients exclude the DC term (degree 0, which is stored in rgba).
/// The combined `sh` array layout depends on sh_degree:
///   sh_degree 0: sh must be nullptr (no higher-order SH)
///   sh_degree 1: per GS stride = 9  (3 colors x 3 degree-1 directions)
///   sh_degree 2: per GS stride = 24 (sh1 9 + sh2 15)
///   sh_degree 3: per GS stride = 45 (sh1 9 + sh2 15 + sh3 21)
///
/// child_start and child_count must be non-null for LOD-tree mode
/// (indicates the file carries tree-child metadata). Pass nullptr for
/// flat (non-LOD) splat files.
std::vector<uint8_t> encode_rad(
    const float* center,
    const float* scale,
    const float* quat,
    const float* rgba,
    const float* sh,
    const uint32_t* child_start,
    const uint16_t* child_count,
    size_t count,
    int sh_degree
);

} // namespace splat
