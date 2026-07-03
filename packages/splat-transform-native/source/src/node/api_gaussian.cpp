#include <algorithm>
#include <atomic>
#include <cassert>
#include <container_helpers.h>
#include <gaussian/gaussian.h>
#include <gaussian/gaussian_block.h>
#include <gaussian/gaussian_lod.h>
#include <node/api_buffer.h>
#include <node/api_gaussian.h>
#include <node/api_thread_pool.h>
#include <numeric>
#include <ranges>
#include <span>
#include <thread_pool.h>
namespace {
enum SplatTable {
    SPLAT_TABLE_MEAN_X = 0,
    SPLAT_TABLE_MEAN_Y = 1,
    SPLAT_TABLE_MEAN_Z = 2,
    SPLAT_TABLE_SCALE_X = 3,
    SPLAT_TABLE_SCALE_Y = 4,
    SPLAT_TABLE_SCALE_Z = 5,
    SPLAT_TABLE_QUAT_X = 6,
    SPLAT_TABLE_QUAT_Y = 7,
    SPLAT_TABLE_QUAT_Z = 8,
    SPLAT_TABLE_QUAT_W = 9,
    SPLAT_TABLE_COLOR_R = 10,
    SPLAT_TABLE_COLOR_G = 11,
    SPLAT_TABLE_COLOR_B = 12,
    SPLAT_TABLE_OPACITY = 13,
    SPLAT_TABLE_SH_OFFSET = 14
};

inline gaussian::Splat read_gaussian(const std::vector<Napi::Buffer<float>>& buffers, size_t sh_size, threading::ThreadPool& pool) {
    auto thread_count = pool.thread_count();
    auto read_count = std::atomic<size_t>(0);
    auto gaussian_count = buffers[0].Length();
    auto gaussians = std::vector<gaussian::Gaussian>(gaussian_count);

    auto gaussian_per_thread = std::max(gaussian_count / thread_count, static_cast<size_t>(1));
    auto futures = std::vector<std::future<void>>();
    auto used_threads = std::min(gaussian_count, thread_count);

    futures.reserve(used_threads);
    read_count.store(0, std::memory_order_release);

    auto process_gaussian = [&, buffers](size_t start, size_t count) {
        for (auto i = 0; i < count; i++) {
            auto index = start + i;

            auto& gaussian = gaussians[index];
            gaussian.mean = Eigen::Vector3f(
                buffers[SPLAT_TABLE_MEAN_X].Data()[index],
                buffers[SPLAT_TABLE_MEAN_Y].Data()[index],
                buffers[SPLAT_TABLE_MEAN_Z].Data()[index]);
            gaussian.scale = Eigen::Vector3f(
                buffers[SPLAT_TABLE_SCALE_X].Data()[index],
                buffers[SPLAT_TABLE_SCALE_Y].Data()[index],
                buffers[SPLAT_TABLE_SCALE_Z].Data()[index]);
            gaussian.rotation = Eigen::Vector4f(
                buffers[SPLAT_TABLE_QUAT_X].Data()[index],
                buffers[SPLAT_TABLE_QUAT_Y].Data()[index],
                buffers[SPLAT_TABLE_QUAT_Z].Data()[index],
                buffers[SPLAT_TABLE_QUAT_W].Data()[index]);
            gaussian.opacity = buffers[SPLAT_TABLE_OPACITY].Data()[index];

            // SH
            gaussian.sh = ::gaussian::SH(sh_size + 3);
            gaussian.sh[0] = buffers[SPLAT_TABLE_COLOR_R].Data()[index];
            gaussian.sh[1] = buffers[SPLAT_TABLE_COLOR_G].Data()[index];
            gaussian.sh[2] = buffers[SPLAT_TABLE_COLOR_B].Data()[index];
            for (auto j = 3; j < gaussian.sh.size(); j++) {
                gaussian.sh[j] = buffers[SPLAT_TABLE_SH_OFFSET + j - 3].Data()[index];
            }

            gaussian.rotation.normalize();
            gaussian.compute_covariance();
            gaussian.compute_bounding_box();
        }
        read_count.fetch_add(count, std::memory_order_acq_rel);
    };

    for (auto i = 0; i < used_threads; i++) {
        futures.push_back(pool.submit_task(process_gaussian, gaussian_per_thread * i, gaussian_per_thread));
    }

    process_gaussian(gaussian_per_thread * used_threads, gaussian_count - gaussian_per_thread * used_threads);

    for (auto& future : futures) {
        future.wait();
    }

    assert(read_count.load(std::memory_order_acquire) == gaussian_count);

    auto splat = gaussian::Splat {
        .gaussians = std::move(gaussians),
        .bounding_box = Eigen::AlignedBox3f {},
    };
    splat.compute_bounding_box();
    return splat;
}

inline void write_gaussian(
    const gaussian::Splat& splat,
    threading::ThreadPool& pool,
    /* out */ std::vector<std::unique_ptr<std::vector<float>>>& buffers) {
    auto thread_count = pool.thread_count();
    auto written_count = std::atomic<size_t>(0);
    auto write_offset = buffers[0]->size();

    auto gaussian_per_thread = std::max(splat.gaussians.size() / thread_count, static_cast<size_t>(1));
    auto futures = std::vector<std::future<void>>();
    auto used_threads = std::min(splat.gaussians.size(), thread_count);

    // prepare writable range.
    for (auto& buffer : buffers) {
        buffer->resize(buffer->size() + splat.gaussians.size());
    }
    futures.reserve(used_threads);
    written_count.store(0, std::memory_order_release);

    // transform gaussian to js readable struct
    auto process_gaussian = [&, write_offset](size_t start, size_t count) {
        for (auto i = 0; i < count; i++) {
            auto index = start + i;
            auto write_index = write_offset + index;
            auto& gaussian = splat.gaussians[index];

            (*buffers[SPLAT_TABLE_MEAN_X])[write_offset + index] = gaussian.mean.x();
            (*buffers[SPLAT_TABLE_MEAN_Y])[write_offset + index] = gaussian.mean.y();
            (*buffers[SPLAT_TABLE_MEAN_Z])[write_offset + index] = gaussian.mean.z();
            (*buffers[SPLAT_TABLE_SCALE_X])[write_offset + index] = gaussian.scale.x();
            (*buffers[SPLAT_TABLE_SCALE_Y])[write_offset + index] = gaussian.scale.y();
            (*buffers[SPLAT_TABLE_SCALE_Z])[write_offset + index] = gaussian.scale.z();
            (*buffers[SPLAT_TABLE_QUAT_X])[write_offset + index] = gaussian.rotation.x();
            (*buffers[SPLAT_TABLE_QUAT_Y])[write_offset + index] = gaussian.rotation.y();
            (*buffers[SPLAT_TABLE_QUAT_Z])[write_offset + index] = gaussian.rotation.z();
            (*buffers[SPLAT_TABLE_QUAT_W])[write_offset + index] = gaussian.rotation.w();
            (*buffers[SPLAT_TABLE_COLOR_R])[write_offset + index] = gaussian.sh[0];
            (*buffers[SPLAT_TABLE_COLOR_G])[write_offset + index] = gaussian.sh[1];
            (*buffers[SPLAT_TABLE_COLOR_B])[write_offset + index] = gaussian.sh[2];
            (*buffers[SPLAT_TABLE_OPACITY])[write_offset + index] = gaussian.opacity;

            for (auto j = 3; j < gaussian.sh.size(); j++) {
                (*buffers[SPLAT_TABLE_SH_OFFSET + j - 3])[write_offset + index] = gaussian.sh[j];
            }
        }

        written_count.fetch_add(count, std::memory_order_acq_rel);
    };

    for (auto i = 0; i < used_threads; i++) {
        futures.push_back(pool.submit_task(process_gaussian, gaussian_per_thread * i, gaussian_per_thread));
    }

    process_gaussian(gaussian_per_thread * used_threads, splat.gaussians.size() - gaussian_per_thread * used_threads);

    for (auto& future : futures) {
        future.wait();
    }

    assert(written_count.load(std::memory_order_acquire) == splat.gaussians.size());
}
} // namespace
namespace node_api::gaussian {
Napi::Value generate_lod(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 7 || !info[0].IsArray() || !info[1].IsNumber() || !info[2].IsBuffer() || !info[3].IsNumber() || !info[4].IsNumber() || !info[5].IsNumber() || !info[6].IsObject()) {
        Napi::TypeError::New(env, "Wrong Arguments").ThrowAsJavaScriptException();
        return env.Null();
    }

    auto& pool = node_api::threading::ThreadPool::Unwrap(info[6].As<Napi::Object>())->impl();
    size_t thread_count = pool.thread_count();

    auto sh_size = info[1].As<Napi::Number>().Uint32Value();
    std::vector<::gaussian::Splat> blocks;
    // read & create blocks.
    {
        auto buffers = std::vector<Napi::Buffer<float>>();
        auto array = info[0].As<Napi::Array>();
        buffers.reserve(sh_size + SPLAT_TABLE_SH_OFFSET);
        for (auto i = 0; i < sh_size + SPLAT_TABLE_SH_OFFSET; i++) {
            buffers.push_back(array[i].AsValue().As<Napi::Buffer<float>>());
        }
        blocks = ::gaussian::block::split_gaussians(
            read_gaussian(buffers, sh_size, pool),
            info[3].As<Napi::Number>().DoubleValue());
    }

    auto level_parameters = info[2].As<Napi::Buffer<::gaussian::lod::GaussianLevelParameters>>();
    auto level_parameters_span = std::span(level_parameters.Data() + 1, level_parameters.Length() - 1);

    auto results = std::vector<::gaussian::Splat>();
    auto gaussian_count = std::make_unique<std::vector<uint32_t>>();
    auto block_boxes = std::make_unique<std::vector<float>>();
    auto block_refs = std::make_unique<std::vector<uint32_t>>();

    // reverse data, small.
    results.reserve(blocks.size() * level_parameters.Length());
    gaussian_count->reserve(blocks.size() * level_parameters.Length());
    block_boxes->reserve(blocks.size() * 6);
    block_refs->reserve(blocks.size() * level_parameters.Length());

    {
        auto levels = level_parameters.Length();
        auto futures = std::vector<std::future<::gaussian::lod::GaussianLod>>();
        auto used_threads = std::min(blocks.size(), thread_count);
        auto min_size = info[4].As<Napi::Number>().Uint32Value();
        auto max_step = info[5].As<Napi::Number>().Uint32Value();

        futures.reserve(blocks.size());

        auto process_block = [&, levels, max_step](size_t index) -> ::gaussian::lod::GaussianLod {
            auto& block = blocks[index];
            auto lod = ::gaussian::lod::GaussianLod();
            lod.levels.reserve(level_parameters.Length());
            lod.splats.reserve(level_parameters.Length());
            lod.levels.push_back(0);
            lod.splats.push_back(std::move(block));
            if (level_parameters_span.size() > 0) {
                ::gaussian::lod::generate_lod(index, lod, level_parameters_span, min_size, max_step);
            }
            return lod;
        };

        for (auto i = 0; i < blocks.size(); i++) {
            futures.push_back(pool.submit_task(process_block, i));
        }

        for (auto& f : futures) {
            auto block = f.get();
            auto base_offset = static_cast<uint32_t>(results.size());
            {
                auto& bbx = block.splats.front().bounding_box;
                helpers::container::append_range(*block_boxes, bbx.min());
                helpers::container::append_range(*block_boxes, bbx.max());
            }
            helpers::container::append_range(*gaussian_count, block.splats | std::views::transform([](::gaussian::Splat& splat) -> uint32_t {
                return static_cast<uint32_t>(splat.gaussians.size());
            }));
            helpers::container::append_range(*block_refs, block.levels | std::views::transform([base_offset](uint32_t ref) -> uint32_t {
                return ref + base_offset;
            }));
            for (auto& splat : block.splats) {
                results.push_back(std::move(splat));
            }
        }

        // release blocks
        {
            auto _ = std::move(blocks);
        }
    }

    auto total = std::reduce(gaussian_count->begin(), gaussian_count->end());
    auto buffers = std::vector<std::unique_ptr<std::vector<float>>>();

    // prealloc buffer before insertion.
    buffers.reserve(sh_size + SPLAT_TABLE_SH_OFFSET);
    {
        for (auto i = 0; i < sh_size + SPLAT_TABLE_SH_OFFSET; i++) {
            buffers.push_back(std::make_unique<std::vector<float>>());
            buffers.back()->reserve(total);
        }
    }

    for (auto& splat : results) {
        write_gaussian(splat, pool, buffers);

        // free data already transformed.
        {
            auto _ = std::move(splat);
        }
    }

    auto object = Napi::Object::New(env);
    {
        auto data = Napi::Array::New(env, buffers.size());
        auto offset = 0;
        object.Set("data", data);
        for (auto i = 0; i < buffers.size(); i++) {
            data[i] = node_api::buffer::UniqueVecBufferFinalizer<float>::make_buffer(env, std::move(buffers[i]));
        }
        auto _ = std::move(buffers);
    }
    object.Set("blockBoxes", node_api::buffer::UniqueVecBufferFinalizer<float>::make_buffer(env, std::move(block_boxes)));
    object.Set("blockRefs", node_api::buffer::UniqueVecBufferFinalizer<uint32_t>::make_buffer(env, std::move(block_refs)));
    object.Set("gaussianCount", node_api::buffer::UniqueVecBufferFinalizer<uint32_t>::make_buffer(env, std::move(gaussian_count)));
    return object;
}
} // namespace node_api::gaussian
