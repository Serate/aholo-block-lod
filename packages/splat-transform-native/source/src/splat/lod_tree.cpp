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

namespace splat {

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

// Simplified weighted merge of multiple GS into one parent node.
// Uses opacity-weighted averaging for most attributes.
static void merge_nodes(
    TreeNode& parent,
    float* parentSh,
    const std::vector<uint32_t>& childIndices,
    const std::vector<TreeNode>& nodes,
    const ShData& shData,
    float /*step*/  // unused in simplified version
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

    // Compute weights from opacity × max_scale (simple area proxy)
    std::vector<float> weights(n);
    float totalWeight = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        float area = nodes[c].featureSize;         // proxy for area
        float op = nodes[c].rgba[3];                // opacity
        weights[i] = std::max(area * op, 1e-30f);
        totalWeight += weights[i];
    }
    float invTotal = 1.0f / totalWeight;
    for (size_t i = 0; i < n; i++) weights[i] *= invTotal;

    // Weighted average center
    parent.center[0] = 0; parent.center[1] = 0; parent.center[2] = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        for (int d = 0; d < 3; d++)
            parent.center[d] += nodes[c].center[d] * weights[i];
    }

    // Max scale (conservative)
    parent.scale[0] = 0; parent.scale[1] = 0; parent.scale[2] = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        for (int d = 0; d < 3; d++)
            parent.scale[d] = std::max(parent.scale[d], nodes[c].scale[d]);
    }
    parent.featureSize = std::max({parent.scale[0], parent.scale[1], parent.scale[2]});

    // Weighted average quaternion (linear then normalize)
    parent.quat[0] = 0; parent.quat[1] = 0; parent.quat[2] = 0; parent.quat[3] = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        for (int d = 0; d < 4; d++)
            parent.quat[d] += nodes[c].quat[d] * weights[i];
    }
    // Normalize
    float qlen = std::sqrt(parent.quat[0]*parent.quat[0] + parent.quat[1]*parent.quat[1] +
                           parent.quat[2]*parent.quat[2] + parent.quat[3]*parent.quat[3]);
    if (qlen > 1e-10f) {
        float inv = 1.0f / qlen;
        for (int d = 0; d < 4; d++) parent.quat[d] *= inv;
    } else {
        parent.quat[0] = 0; parent.quat[1] = 0; parent.quat[2] = 0; parent.quat[3] = 1;
    }

    // Weighted average RGBA
    parent.rgba[0] = 0; parent.rgba[1] = 0; parent.rgba[2] = 0; parent.rgba[3] = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = childIndices[i];
        for (int d = 0; d < 4; d++)
            parent.rgba[d] += nodes[c].rgba[d] * weights[i];
    }

    // Weighted average SH
    if (parentSh && shData.shStride > 0) {
        std::memset(parentSh, 0, shData.shStride * sizeof(float));
        for (size_t i = 0; i < n; i++) {
            uint32_t c = childIndices[i];
            const float* childSh = shData.get(c);
            if (childSh) {
                for (size_t j = 0; j < shData.shStride; j++)
                    parentSh[j] += childSh[j] * weights[i];
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
