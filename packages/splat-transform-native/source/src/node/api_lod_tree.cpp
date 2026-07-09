#include <napi.h>
#include <cstring>
#include <vector>
#include "splat/lod_tree.h"

namespace node_api::lod_tree {

Napi::Value buildLodTree(const Napi::CallbackInfo& info) {
    auto env = info.Env();

    // Args: center, scale, quat, rgba, sh, count, shDegree, [multipliers]
    if (info.Length() < 7) {
        Napi::TypeError::New(env, "Expected at least 7 arguments: center, scale, quat, rgba, sh, count, shDegree, [multipliers]")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    if (!info[0].IsTypedArray() || !info[1].IsTypedArray() ||
        !info[2].IsTypedArray() || !info[3].IsTypedArray()) {
        Napi::TypeError::New(env, "center, scale, quat, rgba must be Float32Array")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    auto centerBuf = info[0].As<Napi::Float32Array>();
    auto scaleBuf  = info[1].As<Napi::Float32Array>();
    auto quatBuf   = info[2].As<Napi::Float32Array>();
    auto rgbaBuf   = info[3].As<Napi::Float32Array>();

    size_t count = info[5].As<Napi::Number>().Uint32Value();
    int shDegree = info[6].As<Napi::Number>().Int32Value();

    const float* shData = nullptr;
    if (!info[4].IsNull() && !info[4].IsUndefined()) {
        auto shBuf = info[4].As<Napi::Float32Array>();
        shData = shBuf.Data();
    }

    std::vector<float> multipliers = {1.0f, 1.4f, 1.7f};
    if (info.Length() >= 8 && info[7].IsTypedArray()) {
        auto multBuf = info[7].As<Napi::Float32Array>();
        multipliers.assign(multBuf.Data(), multBuf.Data() + multBuf.ElementLength());
    }

    auto result = splat::build_lod_tree(
        centerBuf.Data(),
        scaleBuf.Data(),
        quatBuf.Data(),
        rgbaBuf.Data(),
        shData,
        count,
        shDegree,
        multipliers
    );

    auto obj = Napi::Object::New(env);
    obj.Set("treeNodeCount", (uint32_t)result.totalNodes);
    obj.Set("gsCount",       (uint32_t)result.count);

    if (!result.center.empty()) {
        auto arr = Napi::Float32Array::New(env, result.center.size());
        std::memcpy(arr.Data(), result.center.data(), result.center.size() * sizeof(float));
        obj.Set("center", arr);
    }
    if (!result.scale.empty()) {
        auto arr = Napi::Float32Array::New(env, result.scale.size());
        std::memcpy(arr.Data(), result.scale.data(), result.scale.size() * sizeof(float));
        obj.Set("scale", arr);
    }
    if (!result.quat.empty()) {
        auto arr = Napi::Float32Array::New(env, result.quat.size());
        std::memcpy(arr.Data(), result.quat.data(), result.quat.size() * sizeof(float));
        obj.Set("quat", arr);
    }
    if (!result.rgba.empty()) {
        auto arr = Napi::Float32Array::New(env, result.rgba.size());
        std::memcpy(arr.Data(), result.rgba.data(), result.rgba.size() * sizeof(float));
        obj.Set("rgba", arr);
    }
    if (!result.sh.empty()) {
        auto arr = Napi::Float32Array::New(env, result.sh.size());
        std::memcpy(arr.Data(), result.sh.data(), result.sh.size() * sizeof(float));
        obj.Set("sh", arr);
    }
    if (!result.childStart.empty()) {
        auto arr = Napi::Uint32Array::New(env, result.childStart.size());
        std::memcpy(arr.Data(), result.childStart.data(), result.childStart.size() * sizeof(uint32_t));
        obj.Set("childStart", arr);
    }
    if (!result.childCount.empty()) {
        auto arr = Napi::Uint16Array::New(env, result.childCount.size());
        std::memcpy(arr.Data(), result.childCount.data(), result.childCount.size() * sizeof(uint16_t));
        obj.Set("childCount", arr);
    }

    return obj;
}

} // namespace node_api::lod_tree
