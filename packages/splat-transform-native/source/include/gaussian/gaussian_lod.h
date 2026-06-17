#pragma once
#include <container_helpers.h>
#include <cstdint>
#include <gaussian/gaussian.h>
#include <vector>

namespace gaussian::lod {
struct GaussianLodParameters;

namespace detail {
Splat reduce_gaussians(size_t id, const Splat& splat, size_t target_count, float scale_boost, size_t max_step);
}

struct GaussianLod {
    std::vector<Splat> splats;
    std::vector<uint32_t> levels;
};

struct GaussianLevelParameters {
    float precision;
    float scale_boost;
};

template<typename T>
    requires requires(T) {
        requires helpers::container::range_of<T, GaussianLevelParameters>;
    }
inline void generate_lod(size_t id, GaussianLod& lod, T&& parameters, size_t min_size, size_t max_step) {
    auto base_count = lod.splats.back().gaussians.size();
    for (auto& parameter : parameters) {
        auto target_count = std::max<size_t>(static_cast<size_t>(std::floor(static_cast<double>(base_count) * parameter.precision)), 1);
        auto& current = lod.splats.back();
        if (current.gaussians.size() > min_size && target_count < current.gaussians.size()) {
            auto generated = detail::reduce_gaussians(
                id,
                current,
                target_count,
                parameter.scale_boost,
                max_step);
            lod.splats.push_back(std::move(generated));
        }
        lod.levels.push_back(lod.splats.size() - 1);
    }
}
} // namespace gaussian::lod
