#pragma once
#include <memory>
#include <napi.h>
#include <span>
#include <vector>

namespace node_api::buffer {
template<typename T, typename D = std::default_delete<T>>
class UniqueBufferFinalizer {
public:
    UniqueBufferFinalizer() noexcept = default;
    UniqueBufferFinalizer(const UniqueBufferFinalizer<T, D>& other) = delete;
    UniqueBufferFinalizer(UniqueBufferFinalizer<T, D>&& other) noexcept = default;
    UniqueBufferFinalizer(std::unique_ptr<T, D>&& ptr) noexcept : ptr(std::move(ptr)) {}
    UniqueBufferFinalizer(T* ptr) noexcept : ptr(ptr) {}
    void operator()(Napi::Env env, T* data) noexcept {}

    inline static Napi::Buffer<T> make_buffer(Napi::Env& env, std::unique_ptr<T, D>&& ptr, size_t size) noexcept {
        auto finalizer = UniqueBufferFinalizer(std::move(ptr));
        auto data_ptr = finalizer.ptr.get();
        return Napi::Buffer<T>::NewOrCopy(env, data_ptr, size, std::move(finalizer));
    }

protected:
    std::unique_ptr<T, D> ptr;
};

template<typename T, typename D = std::default_delete<std::vector<T>>>
class UniqueVecBufferFinalizer : public UniqueBufferFinalizer<std::vector<T>, D> {
public:
    void operator()(Napi::Env env, T* data) noexcept {}

    template<typename U>
    inline static Napi::Buffer<T> make_buffer(Napi::Env& env, U&& data) noexcept {
        auto finalizer = UniqueVecBufferFinalizer(std::forward<U>(data));
        auto data_ptr = finalizer.ptr->data();
        auto data_size = finalizer.ptr->size();
        return Napi::Buffer<T>::NewOrCopy(env, data_ptr, data_size, std::move(finalizer));
    }
};

template<typename T>
class SharedBufferFinalizer {
public:
    SharedBufferFinalizer() noexcept = default;
    SharedBufferFinalizer(const SharedBufferFinalizer<T>& other) noexcept = default;
    SharedBufferFinalizer(SharedBufferFinalizer<T>&& other) noexcept = default;
    SharedBufferFinalizer(const std::shared_ptr<T>& ptr) noexcept : ptr(ptr) {}
    SharedBufferFinalizer(std::shared_ptr<T>&& ptr) noexcept : ptr(std::move(ptr)) {}
    void operator()(Napi::Env env, T* data) noexcept {}

    template<typename U>
        requires std::is_same_v<std::remove_reference_t<U>, std::shared_ptr<T>>
    inline static Napi::Buffer<T> make_buffer(Napi::Env& env, U&& ptr, size_t size) noexcept {
        auto finalizer = SharedBufferFinalizer(std::forward<U>(ptr));
        auto data_ptr = finalizer.ptr.get();
        return Napi::Buffer<T>::NewOrCopy(env, data_ptr, size, std::move(finalizer));
    }

    template<typename U>
        requires std::is_same_v<std::remove_reference_t<U>, std::shared_ptr<T>>
    inline static Napi::Buffer<T> make_buffer(Napi::Env& env, U&& ptr, size_t offset, size_t size) noexcept {
        auto finalizer = SharedBufferFinalizer(std::forward<U>(ptr));
        auto data_ptr = finalizer.ptr.get();
        return Napi::Buffer<T>::NewOrCopy(env, data_ptr + offset, size, std::move(finalizer));
    }

protected:
    std::shared_ptr<T> ptr;
};

template<typename T>
class SharedVecBufferFinalizer : public SharedBufferFinalizer<std::vector<T>> {
public:
    void operator()(Napi::Env env, T* data) noexcept {}

    template<typename U>
    inline static Napi::Buffer<T> make_buffer(Napi::Env& env, U&& data) noexcept {
        auto finalizer = SharedVecBufferFinalizer(std::forward<U>(data));
        auto data_ptr = finalizer.ptr->data();
        auto data_size = finalizer.ptr->size();
        return Napi::Buffer<T>::NewOrCopy(env, data_ptr, data_size, std::move(finalizer));
    }

    template<typename U>
    inline static Napi::Buffer<T> make_buffer(Napi::Env& env, U&& data, size_t offset, size_t size) noexcept {
        auto finalizer = SharedVecBufferFinalizer(std::forward<U>(data));
        auto data_ptr = finalizer.ptr->data();
        return Napi::Buffer<T>::NewOrCopy(env, data_ptr + offset, size, std::move(finalizer));
    }
};
} // namespace node_api::buffer
