#include <napi.h>
#include <node/api_thread_pool.h>
#include <thread_pool.h>

namespace node_api::threading {
Napi::FunctionReference ThreadPool::Init(Napi::Env env, Napi::Object exports) {
    auto ctor = DefineClass(env, "ThreadPool", {
                                                   InstanceAccessor("threadCount", &ThreadPool::thread_count, nullptr, static_cast<napi_property_attributes>(napi_enumerable | napi_configurable)),
                                                   InstanceAccessor("taskCount", &ThreadPool::task_count, nullptr, static_cast<napi_property_attributes>(napi_enumerable | napi_configurable)),
                                               });
    exports.Set("ThreadPool", ctor);
    return Napi::Persistent(ctor);
}

ThreadPool::ThreadPool(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<ThreadPool>(info), impl_(calc_thread_count(info)) {
}

Napi::Value ThreadPool::thread_count(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), static_cast<double>(this->impl_.thread_count()));
}

Napi::Value ThreadPool::task_count(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), static_cast<double>(this->impl_.task_count()));
}

::threading::ThreadPool& ThreadPool::impl() noexcept {
    return this->impl_;
}

size_t ThreadPool::calc_thread_count(const Napi::CallbackInfo& info) {
    size_t thread_count = 0u;
    if (info.Length() > 0 && info[0].IsNumber()) {
        thread_count = info[0].As<Napi::Number>().Uint32Value();
    }
    if (thread_count == 0) {
        thread_count = std::min(std::max(std::bit_ceil(std::thread::hardware_concurrency()) / 2u, 1u), 16u);
    }

    return thread_count;
}
} // namespace node_api::threading
