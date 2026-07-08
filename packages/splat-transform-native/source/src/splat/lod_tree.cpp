#include "splat/lod_tree.h"
#include "splat/morton_code.h"
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <cstring>

namespace splat {

static const float BASE = 2.0f;

// ---------------------------------------------------------------------------
// Internal node structure for tree building
// ---------------------------------------------------------------------------
struct LodNode {
    float center[3];
    float size;              // feature_size
    std::vector<size_t> children;
    size_t merged_index;     // index after merging (in the global splat array)
};

struct SortEntry {
    size_t index;
    uint64_t morton;
};

// ---------------------------------------------------------------------------
// compute_step (from cycling_lod.rs:24-29)
// ---------------------------------------------------------------------------
static float compute_step(int16_t level, const std::vector<float>& multipliers) {
    int16_t cycle = (int16_t)multipliers.size();
    float multiplier = level >= 0
        ? multipliers[(size_t)level % cycle]
        : multipliers[(((level % cycle) + cycle) % cycle)];
    int16_t base_exp = level >= 0
        ? level / cycle
        : (level - (cycle - 1)) / cycle;
    return std::pow(BASE, (float)base_exp) * multiplier;
}

// ---------------------------------------------------------------------------
// feature_size = 2.0 * max_scale * opacity
// ---------------------------------------------------------------------------
static float feature_size(const float* scale, float opacity) {
    float ms = std::max({std::abs(scale[0]), std::abs(scale[1]), std::abs(scale[2])});
    float lod_op = opacity > 1.0f ? 1.0f : 1.0f; // simplified
    return 2.0f * ms * lod_op;
}

// ---------------------------------------------------------------------------
// grid(step): (center / step).floor()
// ---------------------------------------------------------------------------
static int64_t grid_component(float center, float step) {
    return (int64_t)std::floor(center / step);
}

// ---------------------------------------------------------------------------
// new_merged: compute weighted average of children
// Ported from gsplat.rs:291-340
// ---------------------------------------------------------------------------
static void merge_gaussians(
    const float* center_src,
    const float* scale_src,
    const float* quat_src,
    const float* rgba_src,
    const float* sh_src,
    const std::vector<size_t>& children,
    size_t sh_stride,
    float* out_center,
    float* out_scale,
    float* out_quat,
    float* out_rgba,
    float* out_sh
) {
    size_t n = children.size();
    if (n == 0) return;

    // Compute weights: area * opacity
    std::vector<float> weights(n);
    float total_weight = 0;
    for (size_t i = 0; i < n; i++) {
        size_t idx = children[i];
        float a = rgba_src[idx * 4 + 3];
        // area ≈ scale.x * scale.y * scale.z (simplified)
        float area = std::abs(scale_src[idx * 3] * scale_src[idx * 3 + 1] * scale_src[idx * 3 + 2]);
        weights[i] = area * a;
        total_weight += weights[i];
    }
    if (total_weight < 1e-30f) total_weight = 1e-30f;
    for (size_t i = 0; i < n; i++) weights[i] /= total_weight;

    // Weighted average of center and rgb
    for (int d = 0; d < 3; d++) {
        float sum = 0;
        for (size_t i = 0; i < n; i++) sum += center_src[children[i] * 3 + d] * weights[i];
        out_center[d] = sum;
    }
    for (int d = 0; d < 4; d++) {
        float sum = 0;
        for (size_t i = 0; i < n; i++) sum += rgba_src[children[i] * 4 + d] * weights[i];
        out_rgba[d] = sum;
    }

    // Scale: weighted log-average
    for (int d = 0; d < 3; d++) {
        float sum = 0;
        for (size_t i = 0; i < n; i++) {
            float s = std::abs(scale_src[children[i] * 3 + d]);
            sum += std::log(s + 1e-10f) * weights[i];
        }
        out_scale[d] = std::exp(sum);
    }

    // Quat: weighted average (simplified - no spherical interpolation)
    for (int d = 0; d < 4; d++) {
        float sum = 0;
        for (size_t i = 0; i < n; i++) sum += quat_src[children[i] * 4 + d] * weights[i];
        out_quat[d] = sum;
    }
    float qlen = std::sqrt(out_quat[0]*out_quat[0] + out_quat[1]*out_quat[1] +
                           out_quat[2]*out_quat[2] + out_quat[3]*out_quat[3]);
    if (qlen > 1e-8f) {
        for (int d = 0; d < 4; d++) out_quat[d] /= qlen;
    } else {
        out_quat[0] = 1; out_quat[1] = 0; out_quat[2] = 0; out_quat[3] = 0;
    }

    // SH: weighted average
    if (out_sh && sh_stride > 0) {
        for (size_t j = 0; j < sh_stride; j++) {
            float sum = 0;
            for (size_t i = 0; i < n; i++) sum += sh_src[children[i] * sh_stride + j] * weights[i];
            out_sh[j] = sum;
        }
    }
}

// ---------------------------------------------------------------------------
// main entry point
// ---------------------------------------------------------------------------
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
    size_t sh_stride = 0;
    if (shDegree >= 1) sh_stride = 9;
    if (shDegree >= 2) sh_stride = 24;
    if (shDegree >= 3) sh_stride = 45;

    result.shDegree = shDegree;
    result.count = count;
    result.lodTree = true;

    // ----------------------------------------------------------
    // 1. sort by feature_size (ascending)
    // ----------------------------------------------------------
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return feature_size(scale + a * 3, rgba[a * 4 + 3]) <
               feature_size(scale + b * 3, rgba[b * 4 + 3]);
    });

    // Permute arrays to sorted order
    std::vector<float> sorted_center(count * 3);
    std::vector<float> sorted_scale(count * 3);
    std::vector<float> sorted_quat(count * 4);
    std::vector<float> sorted_rgba(count * 4);
    std::vector<float> sorted_sh(count * sh_stride);
    for (size_t i = 0; i < count; i++) {
        size_t src = order[i];
        memcpy(&sorted_center[i * 3], &center[src * 3], 3 * sizeof(float));
        memcpy(&sorted_scale[i * 3], &scale[src * 3], 3 * sizeof(float));
        memcpy(&sorted_quat[i * 4], &quat[src * 4], 4 * sizeof(float));
        memcpy(&sorted_rgba[i * 4], &rgba[src * 4], 4 * sizeof(float));
        if (sh_stride > 0) memcpy(&sorted_sh[i * sh_stride], &sh[src * sh_stride], sh_stride * sizeof(float));
    }
    // Add space for merged nodes (worst-case: ~count additional)
    sorted_center.reserve(count * 6);
    sorted_scale.reserve(count * 6);
    sorted_quat.reserve(count * 6);
    sorted_rgba.reserve(count * 6);
    if (sh_stride > 0) sorted_sh.reserve(count * 6 * sh_stride);

    // ----------------------------------------------------------
    // 2. Build tree structure
    // ----------------------------------------------------------
    size_t initial_splats = count;
    size_t frontier = 0;
    std::vector<SortEntry> active;
    std::vector<std::vector<std::pair<size_t, std::vector<size_t>>>> levels_output;
    bool make_root = false;

    // Find starting level
    float min_fs = feature_size(sorted_scale.data(), sorted_rgba[3]);
    float level_f = std::log(std::max(min_fs, 1e-6f)) / std::log(BASE);
    int16_t level = (int16_t)std::ceil(level_f) * 3;

    // Adjust level to ensure step >= min_feature_size
    size_t cycle = multipliers.size();
    for (size_t off = 0; off < cycle; off++) {
        float step = compute_step(level - (int16_t)off, multipliers);
        if (step >= min_fs) { level = level - (int16_t)off; break; }
    }

    // Main loop (port of cycling_lod.rs:75-156)
    while (true) {
        float step = compute_step(level, multipliers);

        // Gather new GS from frontier
        while (frontier < initial_splats) {
            float fs = feature_size(&sorted_scale[frontier * 3], sorted_rgba[frontier * 4 + 3]);
            if (fs > step) break;
            active.push_back({frontier, 0});
            frontier++;
        }

        // Morton sort
        for (auto& entry : active) {
            int64_t gx = grid_component(sorted_center[entry.index * 3], step);
            int64_t gy = grid_component(sorted_center[entry.index * 3 + 1], step);
            int64_t gz = grid_component(sorted_center[entry.index * 3 + 2], step);
            entry.morton = morton::morton_encode(gx, gy, gz);
        }
        std::sort(active.begin(), active.end(), [](const SortEntry& a, const SortEntry& b) {
            return a.morton < b.morton;
        });

        // Group by grid cell and merge
        std::vector<SortEntry> next_active;
        std::vector<std::pair<size_t, std::vector<size_t>>> output;
        size_t cell_count = 0;

        size_t start = 0;
        while (start < active.size()) {
            int64_t gx = grid_component(sorted_center[active[start].index * 3], step);
            int64_t gy = grid_component(sorted_center[active[start].index * 3 + 1], step);
            int64_t gz = grid_component(sorted_center[active[start].index * 3 + 2], step);

            size_t end = start + 1;
            while (end < active.size()) {
                if (!make_root) {
                    int64_t ex = grid_component(sorted_center[active[end].index * 3], step);
                    int64_t ey = grid_component(sorted_center[active[end].index * 3 + 1], step);
                    int64_t ez = grid_component(sorted_center[active[end].index * 3 + 2], step);
                    if (ex != gx || ey != gy || ez != gz) break;
                }
                end++;
            }

            cell_count++;
            size_t cell_size = end - start;

            if (cell_size > 1) {
                // Merge this cell's GS into a new parent node
                std::vector<size_t> children;
                for (size_t i = start; i < end; i++) children.push_back(active[i].index);

                size_t parent_idx = sorted_center.size() / 3; // GS count so far
                // Extend arrays for the new node
                sorted_center.resize((parent_idx + 1) * 3);
                sorted_scale.resize((parent_idx + 1) * 3);
                sorted_quat.resize((parent_idx + 1) * 4);
                sorted_rgba.resize((parent_idx + 1) * 4);
                if (sh_stride > 0) sorted_sh.resize((parent_idx + 1) * sh_stride);

                merge_gaussians(
                    sorted_center.data(), sorted_scale.data(), sorted_quat.data(),
                    sorted_rgba.data(), sorted_sh.data(),
                    children, sh_stride,
                    &sorted_center[parent_idx * 3],
                    &sorted_scale[parent_idx * 3],
                    &sorted_quat[parent_idx * 4],
                    &sorted_rgba[parent_idx * 4],
                    sh_stride > 0 ? &sorted_sh[parent_idx * sh_stride] : nullptr
                );

                next_active.push_back({parent_idx, 0});
                output.push_back({parent_idx, children});
            } else {
                // Pass-through
                size_t idx = active[start].index;
                next_active.push_back({idx, 0});
                output.push_back({(size_t)-1, {idx}});
            }

            start = end;
        }

        levels_output.push_back(output);
        active = std::move(next_active);

        level++;
        if (frontier >= initial_splats) {
            if (cell_count <= 1) break;
        }
    }

    // Create root sentinel
    size_t root_idx = active[0].index;
    levels_output.push_back({{(size_t)-1, {root_idx}}});

    // ----------------------------------------------------------
    // 3. Permute from coarse to fine (level-morton order)
    // ----------------------------------------------------------
    std::vector<size_t> perm_indices;
    perm_indices.reserve(sorted_center.size() / 3);
    std::vector<bool> outputted(sorted_center.size() / 3, false);

    // First pass: determine permute order from coarse to fine
    for (auto it = levels_output.rbegin(); it != levels_output.rend(); ++it) {
        for (auto& entry : *it) {
            for (auto c : entry.second) {
                if (!outputted[c]) {
                    outputted[c] = true;
                    perm_indices.push_back(c);
                }
            }
        }
    }

    // Build reverse mapping
    std::vector<size_t> new_index(sorted_center.size() / 3, (size_t)-1);
    for (size_t i = 0; i < perm_indices.size(); i++) {
        new_index[perm_indices[i]] = i;
    }

    // Second pass: rebuild child pointers using new indices
    std::vector<uint32_t> perm_child_start(perm_indices.size(), 0);
    std::vector<uint16_t> perm_child_count(perm_indices.size(), 0);

    for (auto it = levels_output.rbegin(); it != levels_output.rend(); ++it) {
        for (auto& entry : *it) {
            if (entry.first == (size_t)-1) continue; // pass-through or sentinel
            size_t parent_old = entry.first;
            size_t parent_new = new_index[parent_old];
            if (parent_new == (size_t)-1) continue;

            // Filter children that are still in the permuted set
            std::vector<size_t> child_new;
            for (auto c : entry.second) {
                size_t cn = new_index[c];
                if (cn != (size_t)-1) child_new.push_back(cn);
            }
            if (child_new.empty()) continue;

            // Children must be consecutive in the permuted array
            // Sort by new index to ensure they are
            std::sort(child_new.begin(), child_new.end());
            perm_child_start[parent_new] = (uint32_t)child_new[0];
            perm_child_count[parent_new] = (uint16_t)child_new.size();
        }
    }

    // ----------------------------------------------------------
    // 4. Apply permutation to GS arrays
    // ----------------------------------------------------------
    result.totalNodes = perm_indices.size();
    result.center.resize(result.totalNodes * 3);
    result.scale.resize(result.totalNodes * 3);
    result.quat.resize(result.totalNodes * 4);
    result.rgba.resize(result.totalNodes * 4);
    if (sh_stride > 0) result.sh.resize(result.totalNodes * sh_stride);

    for (size_t i = 0; i < result.totalNodes; i++) {
        size_t src = perm_indices[i];
        memcpy(&result.center[i * 3], &sorted_center[src * 3], 3 * sizeof(float));
        memcpy(&result.scale[i * 3], &sorted_scale[src * 3], 3 * sizeof(float));
        memcpy(&result.quat[i * 4], &sorted_quat[src * 4], 4 * sizeof(float));
        memcpy(&result.rgba[i * 4], &sorted_rgba[src * 4], 4 * sizeof(float));
        if (sh_stride > 0) memcpy(&result.sh[i * sh_stride], &sorted_sh[src * sh_stride], sh_stride * sizeof(float));
    }

    // 5. Write tree structure
    result.childStart = std::move(perm_child_start);
    result.childCount = std::move(perm_child_count);
    result.count = count;
    return result;
}

} // namespace splat
