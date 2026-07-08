#include "splat/lod_tree.h"
#include <cmath>
#include <algorithm>
#include <queue>
#include <vector>
#include <cstdint>

namespace splat {

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

} // namespace splat
