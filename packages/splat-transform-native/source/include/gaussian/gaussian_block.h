#pragma once
#include <gaussian/gaussian.h>
#include <type_traits>
#include <vector>

namespace gaussian::block {
namespace detail {
std::vector<Splat> split_gaussians(const Splat& splat, size_t max_block_size);
} // namespace detail

template<typename T>
    requires std::is_same_v<std::remove_reference_t<T>, Splat>
std::vector<Splat> split_gaussians(T&& input, double precision) {
    auto max_block_size = static_cast<size_t>(static_cast<double>(input.gaussians.size()) * precision);
    return detail::split_gaussians(input, max_block_size);
}
} // namespace gaussian::block
