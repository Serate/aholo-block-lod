#define _USE_MATH_DEFINES
#include "splat/lod_tree.h"
#include "splat/morton_code.h"
#include <cmath>
#include <algorithm>
#include <queue>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cfloat>
#include <unordered_map>
#include <array>

namespace splat {

// ============================================================================
// 3x3 Symmetric matrix helpers (for covariance-based merging)
// ============================================================================

struct SymMat3 {
    double xx, yy, zz;  // diagonal
    double xy, xz, yz;  // off-diagonal

    SymMat3() : xx(0), yy(0), zz(0), xy(0), xz(0), yz(0) {}

    static SymMat3 from_scale_quat(const float scale[3], const float quat[4]) {
        // Rotation matrix from quaternion
        double qx = quat[0], qy = quat[1], qz = quat[2], qw = quat[3];
        double sx = scale[0], sy = scale[1], sz = scale[2];

        // R * diag(s^2) * R^T = sum of outer products of scaled axes
        // Axis x: rot * (sx, 0, 0)
        double axx = (1 - 2*(qy*qy + qz*qz)) * sx;
        double axy = 2*(qx*qy + qw*qz) * sx;
        double axz = 2*(qx*qz - qw*qy) * sx;
        // Axis y: rot * (0, sy, 0)
        double ayx = 2*(qx*qy - qw*qz) * sy;
        double ayy = (1 - 2*(qx*qx + qz*qz)) * sy;
        double ayz = 2*(qy*qz + qw*qx) * sy;
        // Axis z: rot * (0, 0, sz)
        double azx = 2*(qx*qz + qw*qy) * sz;
        double azy = 2*(qy*qz - qw*qx) * sz;
        double azz = (1 - 2*(qx*qx + qy*qy)) * sz;

        SymMat3 m;
        m.xx = axx*axx + ayx*ayx + azx*azx;
        m.yy = axy*axy + ayy*ayy + azy*azy;
        m.zz = axz*axz + ayz*ayz + azz*azz;
        m.xy = axx*axy + ayx*ayy + azx*azy;
        m.xz = axx*axz + ayx*ayz + azx*azz;
        m.yz = axy*axz + ayy*ayz + azy*azz;
        return m;
    }

    void add_weighted(const SymMat3& other, double weight) {
        xx += other.xx * weight;
        yy += other.yy * weight;
        zz += other.zz * weight;
        xy += other.xy * weight;
        xz += other.xz * weight;
        yz += other.yz * weight;
    }

    // Jacobi eigendecomposition for 3x3 symmetric matrix.
    // Returns eigenvalues and eigenvectors (as column vectors).
    void eigens(double out_vals[3], double out_vecs[3][3]) const {
        // Copy to local working matrix
        double a[3][3] = {
            {xx, xy, xz},
            {xy, yy, yz},
            {xz, yz, zz}
        };
        double v[3][3] = {{1,0,0},{0,1,0},{0,0,1}};

        // Jacobi iterations
        const int MAX_ITER = 50;
        for (int iter = 0; iter < MAX_ITER; iter++) {
            // Find largest off-diagonal element
            int p = 0, q = 1;
            double max_off = std::fabs(a[0][1]);
            for (int i = 0; i < 3; i++) {
                for (int j = i+1; j < 3; j++) {
                    double val = std::fabs(a[i][j]);
                    if (val > max_off) {
                        max_off = val;
                        p = i; q = j;
                    }
                }
            }

            if (max_off < 1e-30) break;

            // Compute rotation
            double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
            double t = std::copysign(1.0, theta) / (std::fabs(theta) + std::sqrt(theta*theta + 1.0));
            double c = 1.0 / std::sqrt(1.0 + t*t);
            double s = t * c;

            // Apply rotation to a
            double ap = a[p][p], aq = a[q][q];
            a[p][p] = c*c*ap + s*s*aq - 2*c*s*a[p][q];
            a[q][q] = s*s*ap + c*c*aq + 2*c*s*a[p][q];
            a[p][q] = a[q][p] = 0;

            for (int r = 0; r < 3; r++) {
                if (r != p && r != q) {
                    double arp = a[r][p], arq = a[r][q];
                    a[r][p] = a[p][r] = c*arp - s*arq;
                    a[r][q] = a[q][r] = s*arp + c*arq;
                }
                // Accumulate eigenvectors
                double vrp = v[r][p], vrq = v[r][q];
                v[r][p] = c*vrp - s*vrq;
                v[r][q] = s*vrp + c*vrq;
            }
        }

        // Sort by eigenvalue descending
        int order[3] = {0, 1, 2};
        if (a[0][0] < a[1][0]) std::swap(order[0], order[1]);
        if (order[0] < 2 && a[order[0]][order[0]] < a[2][2]) {
            std::swap(order[0], order[2]);
        }
        if (a[order[1]][order[1]] < a[2][2]) {
            std::swap(order[1], order[2]);
        }

        for (int i = 0; i < 3; i++) {
            int o = order[i];
            out_vals[i] = a[o][o];
            for (int r = 0; r < 3; r++) {
                out_vecs[r][i] = v[r][o];
            }
        }

        // Ensure right-handed basis (determinant > 0)
        double det = out_vecs[0][0]*(out_vecs[1][1]*out_vecs[2][2] - out_vecs[1][2]*out_vecs[2][1])
                   - out_vecs[0][1]*(out_vecs[1][0]*out_vecs[2][2] - out_vecs[1][2]*out_vecs[2][0])
                   + out_vecs[0][2]*(out_vecs[1][0]*out_vecs[2][1] - out_vecs[1][1]*out_vecs[2][0]);
        if (det < 0) {
            out_vecs[0][2] = -out_vecs[0][2];
            out_vecs[1][2] = -out_vecs[1][2];
            out_vecs[2][2] = -out_vecs[2][2];
        }
    }

    // Positive eigens: ensure all eigenvalues >= 0
    void positive_eigens(double out_vals[3], double out_vecs[3][3]) const {
        eigens(out_vals, out_vecs);
        for (int i = 0; i < 3; i++) {
            if (out_vals[i] < 0) out_vals[i] = 0;
        }
    }
};

static double ellipsoid_area(double s0, double s1, double s2) {
    const double P = 1.6075;
    double num = std::pow(s0*s1, P) + std::pow(s0*s2, P) + std::pow(s1*s2, P);
    return 4.0 * M_PI * std::pow(num / 3.0, 1.0 / P);
}

// Convert eigenvector matrix (column vectors) to quaternion
static void mat3_to_quat(const double vecs[3][3], double quat[4]) {
    double trace = vecs[0][0] + vecs[1][1] + vecs[2][2];
    if (trace > 0) {
        double s = 0.5 / std::sqrt(trace + 1.0);
        quat[3] = 0.25 / s;
        quat[0] = (vecs[2][1] - vecs[1][2]) * s;
        quat[1] = (vecs[0][2] - vecs[2][0]) * s;
        quat[2] = (vecs[1][0] - vecs[0][1]) * s;
    } else if (vecs[0][0] > vecs[1][1] && vecs[0][0] > vecs[2][2]) {
        double s = 2.0 * std::sqrt(1.0 + vecs[0][0] - vecs[1][1] - vecs[2][2]);
        quat[3] = (vecs[2][1] - vecs[1][2]) / s;
        quat[0] = 0.25 * s;
        quat[1] = (vecs[0][1] + vecs[1][0]) / s;
        quat[2] = (vecs[0][2] + vecs[2][0]) / s;
    } else if (vecs[1][1] > vecs[2][2]) {
        double s = 2.0 * std::sqrt(1.0 + vecs[1][1] - vecs[0][0] - vecs[2][2]);
        quat[3] = (vecs[0][2] - vecs[2][0]) / s;
        quat[0] = (vecs[0][1] + vecs[1][0]) / s;
        quat[1] = 0.25 * s;
        quat[2] = (vecs[1][2] + vecs[2][1]) / s;
    } else {
        double s = 2.0 * std::sqrt(1.0 + vecs[2][2] - vecs[0][0] - vecs[1][1]);
        quat[3] = (vecs[1][0] - vecs[0][1]) / s;
        quat[0] = (vecs[0][2] + vecs[2][0]) / s;
        quat[1] = (vecs[1][2] + vecs[2][1]) / s;
        quat[2] = 0.25 * s;
    }
    // Normalize
    double len = std::sqrt(quat[0]*quat[0] + quat[1]*quat[1] + quat[2]*quat[2] + quat[3]*quat[3]);
    if (len > 1e-10) {
        double inv = 1.0 / len;
        quat[0] *= inv; quat[1] *= inv; quat[2] *= inv; quat[3] *= inv;
    } else {
        quat[0] = 0; quat[1] = 0; quat[2] = 0; quat[3] = 1;
    }
}

// ============================================================================
// traverse_block (per-frame traversal)
// ============================================================================

// BinaryHeap entry sorted by pixel_scale (descending)
struct HeapEntry {
    float pixelScale;
    uint32_t nodeIndex;

    bool operator<(const HeapEntry& other) const {
        return pixelScale < other.pixelScale; // max-heap
    }
};

// Compute pixel_scale for a node
static float compute_pixel_scale(
    const float* center,  // f32[3] for this node
    float size,           // feature_size
    const float cameraPos[3],
    const float cameraForward[3],
    float lodScale
) {
    float dx = center[0] - cameraPos[0];
    float dy = center[1] - cameraPos[1];
    float dz = center[2] - cameraPos[2];
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-6f) dist = 1e-6f;
    return (size / dist) * lodScale;
}

size_t traverse_block(
    const uint32_t* childStart,
    const uint16_t* childCount,
    const float* center,
    const float* size,
    size_t totalNodes,
    const float cameraPos[3],
    const float cameraForward[3],
    float lodScale,
    float pixelScaleLimit,
    size_t maxSplats,
    uint32_t* out_indices
) {
    if (totalNodes == 0 || maxSplats == 0) return 0;

    // BinaryHeap for traversal
    std::priority_queue<HeapEntry> heap;
    size_t numSplats = 0;
    size_t outputCount = 0;

    // Push root
    float rootPs = compute_pixel_scale(center, size[0], cameraPos, cameraForward, lodScale);
    heap.push({rootPs, 0});
    numSplats = 1;

    while (!heap.empty()) {
        auto entry = heap.top();
        if (entry.pixelScale <= pixelScaleLimit) break;

        uint32_t idx = entry.nodeIndex;
        uint16_t cnt = childCount[idx];

        if (cnt == 0) {
            // Leaf → output
            heap.pop();
            out_indices[outputCount++] = idx;
            if (outputCount >= maxSplats) break;
            continue;
        }

        uint32_t start = childStart[idx];
        size_t newTotal = numSplats - 1 + cnt;
        if (newTotal > maxSplats) break;

        heap.pop();

        for (uint32_t c = 0; c < cnt; c++) {
            uint32_t childIdx = start + c;
            float ps = compute_pixel_scale(
                &center[childIdx * 3], size[childIdx],
                cameraPos, cameraForward, lodScale);

            if (ps <= pixelScaleLimit) {
                out_indices[outputCount++] = childIdx;
                if (outputCount >= maxSplats) {
                    numSplats = newTotal;
                    goto done;
                }
            } else {
                heap.push({ps, childIdx});
            }
        }
        numSplats = newTotal;
    }

done:
    // Flush remaining heap entries as output
    while (!heap.empty() && outputCount < maxSplats) {
        auto entry = heap.top();
        heap.pop();
        out_indices[outputCount++] = entry.nodeIndex;
    }

    return outputCount;
}

// ============================================================================
// build_lod_tree (tree construction)
// ============================================================================

static const float BASE = 2.0f;
static const size_t CL_CHUNK_SIZE = 16384;  // chunk size for tree building

// Compute step size for a given level and multipliers
static float compute_step(int level, const std::vector<float>& multipliers) {
    int cycle = (int)multipliers.size();
    int m = level % cycle;
    if (m < 0) m += cycle;
    int base_pow = level / cycle;
    if (level < 0 && (level % cycle) != 0) base_pow--;
    return std::pow(BASE, (float)base_pow) * multipliers[(size_t)m];
}

static size_t sh_stride_for_degree(int degree) {
    switch (degree) {
        case 0: return 0;
        case 1: return 9;   // 3×3
        case 2: return 24;  // 9 + 15
        case 3: return 45;  // 9 + 15 + 21
        default: return 0;
    }
}

// A single node in the LOD tree during construction
struct TreeNode {
    // Attributes (f32)
    float center[3];
    float scale[3];
    float quat[4];
    float rgba[4];

    // Feature size for level determination
    float featureSize;

    // Child info (filled after finalization)
    uint32_t childStart;
    uint16_t childCount;

    // Whether this node was output in the permutation
    bool outputted;
};

// The SH data is stored separately to keep TreeNode small
struct ShData {
    std::vector<float> values;  // f32 × shStride per node
    size_t shStride;

    ShData(size_t count, size_t stride) : shStride(stride) {
        if (stride > 0) values.resize(count * stride, 0.0f);
    }

    void resize(size_t count) {
        if (shStride > 0) values.resize(count * shStride, 0.0f);
    }

    const float* get(size_t node) const {
        return shStride > 0 ? &values[node * shStride] : nullptr;
    }

    float* get_mut(size_t node) {
        return shStride > 0 ? &values[node * shStride] : nullptr;
    }

    void copy_to(size_t dst, const float* src) {
        if (shStride > 0 && src) {
            std::memcpy(&values[dst * shStride], src, shStride * sizeof(float));
        }
    }

    void weighted_add_to(size_t dst, const float* src, float weight) {
        if (shStride > 0 && src) {
            for (size_t j = 0; j < shStride; j++) {
                values[dst * shStride + j] += src[j] * weight;
            }
        }
    }
};

// Description of one graph edge in the LOD tree: parent → child
struct TreeEdge {
    uint32_t parent;
    uint32_t child;
};

// Description of one level's output: list of (parent, [children...])
struct LevelMergeGroup {
    uint32_t parent;            // UINT32_MAX for pass-through
    std::vector<uint32_t> children;
};

// Covariance-based merge matching Spark's approach.
// Uses ellipsoid-area-weighted averaging for center/color/SH,
// and covariance summation + eigendecomposition for scale/quat.
static void merge_nodes(
    TreeNode& parent,
    float* parentSh,
    const std::vector<uint32_t>& childIndices,
    const std::vector<TreeNode>& nodes,
    const ShData& shData,
    float step
) {
    size_t n = childIndices.size();
    if (n == 0) return;
    if (n == 1) {
        // Copy single child
        uint32_t c = childIndices[0];
        std::memcpy(parent.center, nodes[c].center, 3 * sizeof(float));
        std::memcpy(parent.scale, nodes[c].scale, 3 * sizeof(float));
        std::memcpy(parent.quat, nodes[c].quat, 4 * sizeof(float));
        std::memcpy(parent.rgba, nodes[c].rgba, 4 * sizeof(float));
        if (parentSh && shData.shStride > 0) {
            std::memcpy(parentSh, shData.get(c), shData.shStride * sizeof(float));
        }
        parent.featureSize = nodes[c].featureSize;
        return;
    }

    // Compute weights from ellipsoid_area(scales) * opacity (matching Spark)
    std::vector<double> weights(n);
    double totalWeight = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        double s0 = nodes[c].scale[0];
        double s1 = nodes[c].scale[1];
        double s2 = nodes[c].scale[2];
        double area = ellipsoid_area(s0, s1, s2);
        double op = (double)nodes[c].rgba[3];
        weights[i] = std::max(area * op, 1e-30);
        totalWeight += weights[i];
    }
    double invTotal = 1.0 / totalWeight;
    for (size_t i = 0; i < n; i++) weights[i] *= invTotal;

    // Weighted average center
    double center[3] = {0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        for (int d = 0; d < 3; d++)
            center[d] += nodes[c].center[d] * weights[i];
    }
    parent.center[0] = (float)center[0];
    parent.center[1] = (float)center[1];
    parent.center[2] = (float)center[2];

    // Weighted average RGB
    double rgb[3] = {0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        for (int d = 0; d < 3; d++)
            rgb[d] += nodes[c].rgba[d] * weights[i];
    }

    // Covariance-based merge for scale and quat (matching Spark)
    double filter2 = (0.5 * (double)step);
    filter2 = filter2 * filter2;

    SymMat3 totalCov;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        double w = weights[i];

        // Child covariance from its scale+quat
        SymMat3 childCov = SymMat3::from_scale_quat(nodes[c].scale, nodes[c].quat);

        // Delta from merged center
        double dx = nodes[c].center[0] - center[0];
        double dy = nodes[c].center[1] - center[1];
        double dz = nodes[c].center[2] - center[2];

        // Add delta^2 + covariance + filter2, weighted
        SymMat3 contrib;
        contrib.xx = (dx*dx + childCov.xx + filter2) * w;
        contrib.yy = (dy*dy + childCov.yy + filter2) * w;
        contrib.zz = (dz*dz + childCov.zz + filter2) * w;
        contrib.xy = (dx*dy + childCov.xy) * w;
        contrib.xz = (dx*dz + childCov.xz) * w;
        contrib.yz = (dy*dz + childCov.yz) * w;

        totalCov.add_weighted(contrib, 1.0);
    }

    // Eigendecomposition
    double vals[3], vecs[3][3];
    totalCov.positive_eigens(vals, vecs);

    // New scales = sqrt(eigenvalues)
    double newScale[3];
    for (int i = 0; i < 3; i++) {
        newScale[i] = std::sqrt(std::max(vals[i], 0.0));
        if (newScale[i] < 1e-30) newScale[i] = 1e-30;
    }

    // New quaternion from eigenvector matrix
    double newQuat[4];
    mat3_to_quat(vecs, newQuat);

    // New opacity = totalWeight / ellipsoid_area(newScales)
    double newArea = ellipsoid_area(newScale[0], newScale[1], newScale[2]);
    double newOpacity = totalWeight / std::max(newArea, 1e-30);
    newOpacity = std::min(newOpacity, 1.0);

    parent.scale[0] = (float)newScale[0];
    parent.scale[1] = (float)newScale[1];
    parent.scale[2] = (float)newScale[2];
    parent.quat[0] = (float)newQuat[0];
    parent.quat[1] = (float)newQuat[1];
    parent.quat[2] = (float)newQuat[2];
    parent.quat[3] = (float)newQuat[3];
    parent.rgba[0] = (float)rgb[0];
    parent.rgba[1] = (float)rgb[1];
    parent.rgba[2] = (float)rgb[2];
    parent.rgba[3] = (float)newOpacity;

    parent.featureSize = (float)std::max({newScale[0], newScale[1], newScale[2]}) * 2.0f;
    if (parent.featureSize < 1e-10f) parent.featureSize = 1e-10f;

    // Weighted average SH
    if (parentSh && shData.shStride > 0) {
        std::memset(parentSh, 0, shData.shStride * sizeof(float));
        for (size_t i = 0; i < n; i++) {
            uint32_t c = childIndices[i];
            const float* childSh = shData.get(c);
            if (childSh) {
                for (size_t j = 0; j < shData.shStride; j++)
                    parentSh[j] += childSh[j] * (float)weights[i];
            }
        }
    }
}

// Build the full LOD tree from input GS data
LodTreeResult build_lod_tree(
    const float* center,
    const float* scale,
    const float* quat,
    const float* rgba,
    const float* sh,
    size_t count,
    int shDegree,
    const std::vector<float>& multipliers
) {
    LodTreeResult result;
    if (count == 0) return result;

    size_t shStride = sh_stride_for_degree(shDegree);

    // ============================
    // 1. Initialize leaf nodes
    // ============================
    std::vector<TreeNode> nodes(count);
    ShData nodeSh(count, shStride);

    for (size_t i = 0; i < count; i++) {
        std::memcpy(nodes[i].center, &center[i * 3], 3 * sizeof(float));
        std::memcpy(nodes[i].scale,  &scale[i * 3],  3 * sizeof(float));
        std::memcpy(nodes[i].quat,   &quat[i * 4],   4 * sizeof(float));
        std::memcpy(nodes[i].rgba,   &rgba[i * 4],   4 * sizeof(float));

        // feature_size = max scale × 2 (diameter)
        nodes[i].featureSize = std::max({scale[i * 3], scale[i * 3 + 1], scale[i * 3 + 2]}) * 2.0f;
        if (nodes[i].featureSize < 1e-10f) nodes[i].featureSize = 1e-10f;

        nodes[i].childStart = 0;
        nodes[i].childCount = 0;
        nodes[i].outputted = false;

        if (sh && shStride > 0) {
            std::memcpy(nodeSh.get_mut(i), &sh[i * shStride], shStride * sizeof(float));
        }
    }

    // ============================
    // 2. Sort by feature_size (ascending)
    // ============================
    std::vector<size_t> sortOrder(count);
    for (size_t i = 0; i < count; i++) sortOrder[i] = i;
    std::sort(sortOrder.begin(), sortOrder.end(), [&](size_t a, size_t b) {
        return nodes[a].featureSize < nodes[b].featureSize;
    });

    // Apply sort order: permute nodes array
    {
        std::vector<TreeNode> sortedNodes(count);
        ShData sortedSh(count, shStride);
        for (size_t i = 0; i < count; i++) {
            sortedNodes[i] = nodes[sortOrder[i]];
            if (shStride > 0) {
                std::memcpy(sortedSh.get_mut(i), nodeSh.get(sortOrder[i]), shStride * sizeof(float));
            }
        }
        nodes = std::move(sortedNodes);
        nodeSh = std::move(sortedSh);
    }

    // ============================
    // 3. Build levels
    // ============================
    float minFeatureSize = std::max(nodes[0].featureSize, 0.000001f);
    float logMin = std::log(minFeatureSize) / std::log(BASE);
    int baseLevel = (int)std::ceil(logMin) * 3;

    // Adjust base level
    int level = baseLevel;
    for (int offset = 0; offset < (int)multipliers.size(); offset++) {
        int candidate = baseLevel - offset;
        float step = compute_step(candidate, multipliers);
        if (step >= minFeatureSize) {
            level = candidate;
            break;
        }
    }

    size_t frontier = 0;
    std::vector<uint32_t> active;  // indices of active nodes (in the current level)

    // Levels output: each level is a vector of merge groups
    std::vector<std::vector<LevelMergeGroup>> levelsOutput;

    while (true) {
        float step = compute_step(level, multipliers);

        // Add nodes whose feature_size ≤ step to the active set
        while (frontier < count) {
            if (nodes[frontier].featureSize > step) break;
            active.push_back((uint32_t)frontier);
            frontier++;
        }

        // Morton code each active node
        struct ActiveEntry {
            uint32_t index;      // node index
            int64_t grid[3];     // grid coordinates
            uint64_t morton;     // Morton code
        };

        std::vector<ActiveEntry> entries(active.size());
        for (size_t i = 0; i < active.size(); i++) {
            uint32_t idx = active[i];
            entries[i].index = idx;
            for (int d = 0; d < 3; d++) {
                entries[i].grid[d] = (int64_t)std::floor(nodes[idx].center[d] / step);
            }
            entries[i].morton = morton::morton_encode(
                entries[i].grid[0], entries[i].grid[1], entries[i].grid[2]
            );
        }

        // Sort by Morton code
        std::sort(entries.begin(), entries.end(), [](const ActiveEntry& a, const ActiveEntry& b) {
            return a.morton < b.morton;
        });

        // Group by grid cell
        std::vector<LevelMergeGroup> levelGroups;
        std::vector<uint32_t> nextActive;

        size_t pos = 0;
        while (pos < entries.size()) {
            auto& first = entries[pos];
            size_t end = pos + 1;
            while (end < entries.size() &&
                   entries[end].grid[0] == first.grid[0] &&
                   entries[end].grid[1] == first.grid[1] &&
                   entries[end].grid[2] == first.grid[2]) {
                end++;
            }

            size_t cellCount = end - pos;
            LevelMergeGroup group;

            if (cellCount == 1) {
                // Pass-through: single node, no merge
                group.parent = UINT32_MAX;
                group.children = {first.index};
                nextActive.push_back(first.index);
            } else {
                // Merge: create parent node
                std::vector<uint32_t> childIds;
                childIds.reserve(cellCount);
                for (size_t k = pos; k < end; k++) {
                    childIds.push_back(entries[k].index);
                }

                // Create parent node (append to end of arrays)
                uint32_t parentIdx = (uint32_t)nodes.size();
                nodes.push_back(TreeNode{});
                std::memset(&nodes.back(), 0, sizeof(TreeNode));
                if (shStride > 0) {
                    nodeSh.values.resize((parentIdx + 1) * shStride, 0);
                } else {
                    nodeSh.values.resize((parentIdx + 1) * shStride);
                }

                // Merge children into parent
                merge_nodes(
                    nodes[parentIdx],
                    shStride > 0 ? nodeSh.get_mut(parentIdx) : nullptr,
                    childIds, nodes, nodeSh, step
                );

                group.parent = parentIdx;
                group.children = childIds;
                nextActive.push_back(parentIdx);
            }

            levelGroups.push_back(std::move(group));
            pos = end;
        }

        levelsOutput.push_back(std::move(levelGroups));

        // Update active set for next level
        active = std::move(nextActive);
        level++;

        // Check termination: all GS added & only 1 cell
        if (frontier >= count && active.size() <= 1) {
            break;
        }
    }

    // ============================
    // 4. Level-Morton permute (coarse → fine)
    // ============================
    // Walk levels in reverse (coarse first). For each merge group:
    // - If parent is a real node (not pass-through): remap children to output positions
    // - Append unique children to permuteOrder
    // - Mark children as outputted
    //
    // At the end, the root is appended.

    std::vector<uint32_t> permuteOrder;
    permuteOrder.reserve(nodes.size());

    // Walk levels coarse→fine
    for (int li = (int)levelsOutput.size() - 1; li >= 0; li--) {
        for (auto& group : levelsOutput[li]) {
            // Filter already-outputted children
            std::vector<uint32_t> uniqueChildren;
            for (uint32_t c : group.children) {
                if (!nodes[c].outputted) {
                    uniqueChildren.push_back(c);
                    nodes[c].outputted = true;
                }
            }
            if (uniqueChildren.empty()) continue;

            // If parent is a real merge node (not pass-through), set its children
            if (group.parent != UINT32_MAX) {
                nodes[group.parent].childStart = (uint32_t)permuteOrder.size();
                nodes[group.parent].childCount = (uint16_t)uniqueChildren.size();
            }

            permuteOrder.insert(permuteOrder.end(), uniqueChildren.begin(), uniqueChildren.end());
        }
    }

    // Append root (last remaining active node)
    if (!active.empty() && !nodes[active[0]].outputted) {
        permuteOrder.push_back(active[0]);
        nodes[active[0]].outputted = true;
    }

    // Handle any un-outputted nodes (shouldn't happen, but safety)
    for (size_t i = 0; i < nodes.size(); i++) {
        if (!nodes[i].outputted) {
            permuteOrder.push_back((uint32_t)i);
            nodes[i].outputted = true;
        }
    }

    // ============================
    // 5. Build final output arrays
    // ============================
    size_t ntotal = permuteOrder.size();
    result.count = count;
    result.totalNodes = ntotal;
    result.shDegree = shDegree;
    result.lodTree = true;

    result.center.resize(ntotal * 3);
    result.scale.resize(ntotal * 3);
    result.quat.resize(ntotal * 4);
    result.rgba.resize(ntotal * 4);
    result.childStart.resize(ntotal);
    result.childCount.resize(ntotal);
    if (shStride > 0) result.sh.resize(ntotal * shStride);

    // Remap childStart/childCount from old indices to permuted indices
    std::vector<uint32_t> oldToNew(nodes.size(), UINT32_MAX);
    for (size_t i = 0; i < ntotal; i++) {
        oldToNew[permuteOrder[i]] = (uint32_t)i;
    }

    for (size_t i = 0; i < ntotal; i++) {
        uint32_t oldIdx = permuteOrder[i];

        std::memcpy(&result.center[i * 3], nodes[oldIdx].center, 3 * sizeof(float));
        std::memcpy(&result.scale[i * 3],  nodes[oldIdx].scale,  3 * sizeof(float));
        std::memcpy(&result.quat[i * 4],   nodes[oldIdx].quat,   4 * sizeof(float));
        std::memcpy(&result.rgba[i * 4],   nodes[oldIdx].rgba,   4 * sizeof(float));

        if (shStride > 0 && nodeSh.shStride > 0) {
            std::memcpy(&result.sh[i * shStride], nodeSh.get(oldIdx), shStride * sizeof(float));
        }

        // Remap children
        if (nodes[oldIdx].childCount > 0) {
            uint32_t oldStart = nodes[oldIdx].childStart;
            result.childStart[i] = oldToNew[oldStart];
            result.childCount[i] = nodes[oldIdx].childCount;
        } else {
            result.childStart[i] = 0;
            result.childCount[i] = 0;
        }
    }

    return result;
}

} // namespace splat
