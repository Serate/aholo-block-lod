#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <napi.h>
#include <thread>
#include <thread_pool.h>

namespace node_api::threading {
class ThreadPool : public Napi::ObjectWrap<ThreadPool> {
public:
    static Napi::FunctionReference Init(Napi::Env env, Napi::Object exports);

    ThreadPool(const Napi::CallbackInfo& info);
    ::threading::ThreadPool& impl() noexcept;

    ThreadPool() = delete;

private:
    static size_t calc_thread_count(const Napi::CallbackInfo& info);
    Napi::Value thread_count(const Napi::CallbackInfo& info);
    Napi::Value task_count(const Napi::CallbackInfo& info);

    ::threading::ThreadPool impl_;
};
} // namespace node_api::threading
