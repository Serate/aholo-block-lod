#include <napi.h>
#include "splat/rad_decoder.h"
#include <fstream>

namespace node_api::rad {

Napi::Value decodeRad(const Napi::CallbackInfo& info) {
    auto env = info.Env();

    if (info.Length() < 1 || !info[0].IsBuffer()) {
        Napi::TypeError::New(env, "Expected Buffer argument").ThrowAsJavaScriptException();
        return env.Null();
    }

    auto buf = info[0].As<Napi::Buffer<uint8_t>>();
    auto result = splat::decode_rad(buf.Data(), buf.Length());

    auto obj = Napi::Object::New(env);
    obj.Set("count", (uint32_t)result.count);
    obj.Set("totalNodes", (uint32_t)result.totalNodes);
    obj.Set("shDegree", (uint32_t)result.shDegree);

    if (!result.childStart.empty()) {
        obj.Set("childStart", Napi::Buffer<uint32_t>::Copy(env, result.childStart.data(), result.childStart.size()));
    }
    if (!result.childCount.empty()) {
        obj.Set("childCount", Napi::Buffer<uint16_t>::Copy(env, result.childCount.data(), result.childCount.size()));
    }
    if (!result.center.empty()) {
        obj.Set("center", Napi::Buffer<uint16_t>::Copy(env, result.center.data(), result.center.size()));
    }
    if (!result.rgba.empty()) {
        obj.Set("rgba", Napi::Buffer<uint8_t>::Copy(env, result.rgba.data(), result.rgba.size()));
    }
    if (!result.scale.empty()) {
        obj.Set("scale", Napi::Buffer<uint8_t>::Copy(env, result.scale.data(), result.scale.size()));
    }
    if (!result.quat.empty()) {
        obj.Set("quat", Napi::Buffer<uint8_t>::Copy(env, result.quat.data(), result.quat.size()));
    }
    if (!result.sh.empty()) {
        obj.Set("sh", Napi::Buffer<uint8_t>::Copy(env, result.sh.data(), result.sh.size()));
    }

    return obj;
}

} // namespace node_api::rad
