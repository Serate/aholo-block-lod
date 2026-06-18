#include <array>
#include <container_helpers.h>
#include <eigen3/Eigen/Dense>
#include <gaussian/gaussian_block.h>
#include <queue>
#include <ranges>
#include <utility>
#include <vector>

namespace {
struct RefSplat {
    const gaussian::Splat& source;
    std::vector<size_t> gaussians;
    Eigen::AlignedBox3f box;
    bool need_split;

    RefSplat(const gaussian::Splat& splat) : source(splat), box(splat.bounding_box), need_split(true) {
        this->gaussians.reserve(splat.gaussians.size());
        for (auto i = 0; i < splat.gaussians.size(); i++) {
            this->gaussians.push_back(i);
        }
    }
    RefSplat(RefSplat& splat) : source(splat.source), need_split(true) {}
    RefSplat(RefSplat&& splat) = default;

    void compute_bounding_box() {
        this->box = Eigen::AlignedBox3f();
        for (auto index : this->gaussians) {
            this->box.extend(this->source.gaussians[index].bounding_box);
        }
    }

    void compute_need_split(const Eigen::AlignedBox3f& parent_box, size_t max_block_size) {
        this->need_split = this->gaussians.size() > max_block_size &&
                           ((this->box.min() - parent_box.min()).norm() > 0.001 ||
                               (this->box.max() - parent_box.max()).norm() > 0.001);
    }

    gaussian::Splat to_owned() {
        std::vector<gaussian::Gaussian> gaussians;
        std::vector<float> sh;
        gaussians.reserve(this->gaussians.size());
        for (auto index : this->gaussians) {
            gaussians.push_back(this->source.gaussians[index]);
        }
        {
            auto _ = std::move(this->gaussians);
        }
        auto result = gaussian::Splat {
            .gaussians = std::move(gaussians),
            .bounding_box = this->box,
        };
        result.compute_compact_bounding_box();
        return result;
    }
};

std::array<Eigen::AlignedBox3f, 8> split_box(const Eigen::AlignedBox3f& box) {
    auto center = box.center();
    return {
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::BottomLeftFloor)),
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::BottomRightFloor)),
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::TopLeftFloor)),
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::TopRightFloor)),
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::BottomLeftCeil)),
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::BottomRightCeil)),
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::TopLeftCeil)),
        Eigen::AlignedBox3f().extend(center).extend(box.corner(Eigen::AlignedBox3f::CornerType::TopRightCeil)),
    };
}

std::array<RefSplat, 8> split_block(RefSplat& splat, size_t max_block_size) {
    auto boxes = split_box(splat.box);
    std::array<RefSplat, 8> result { RefSplat(splat),
        RefSplat(splat),
        RefSplat(splat),
        RefSplat(splat),
        RefSplat(splat),
        RefSplat(splat),
        RefSplat(splat),
        RefSplat(splat) };

    for (auto& r : result) {
        r.gaussians.reserve(splat.gaussians.size() / 4);
    }

    for (auto index : splat.gaussians) {
        auto& box = splat.source.gaussians[index].bounding_box;
        auto max_interaction = 0.0;
        auto max_interaction_index = 0;
        for (auto i = 0; i < 8; i++) {
            auto intersection = boxes[i].intersection(box);
            if (!intersection.isEmpty()) {
                auto current = intersection.volume();
                if (current > max_interaction) {
                    max_interaction = current;
                    max_interaction_index = i;
                }
            }
        }
        result[max_interaction_index].gaussians.push_back(index);
    }

    for (auto i = 0; i < result.size(); i++) {
        result[i].box = boxes[i];
        result[i].compute_need_split(splat.box, max_block_size);
    }

    return result;
}
} // namespace

namespace gaussian::block::detail {
std::vector<Splat> split_gaussians(const Splat& splat, size_t max_block_size) {
    if (splat.gaussians.size() <= max_block_size) {
        return std::vector({ splat });
    }

    std::queue<RefSplat> queue;
    std::vector<Splat> results;
    queue.emplace(splat);

    while (!queue.empty()) {
        auto r = std::move(queue.front());
        queue.pop();
        if (r.need_split) {
            for (auto& splitted : split_block(r, max_block_size)) {
                queue.emplace(std::move(splitted));
            }
        } else if (r.gaussians.size() > 0) {
            results.emplace_back(r.to_owned());
        }
    }

    return results;
}
} // namespace gaussian::block::detail
