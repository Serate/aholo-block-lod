#include <napi.h>
#include <cstring>
#include "splat/rad_encoder.h"

namespace node_api::rad_encode {

Napi::Value encodeRad(const Napi::CallbackInfo& info) {
    auto env = info.Env();

    // Args: center, scale, quat, rgba, sh, childStart, childCount, count, shDegree
    if (info.Length() < 9) {
        Napi::TypeError::New(env, "Expected 9 arguments: center, scale, quat, rgba, sh, childStart, childCount, count, shDegree")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    if (!info[0].IsFloat32Array() || !info[1].IsFloat32Array() ||
        !info[2].IsFloat32Array() || !info[3].IsFloat32Array()) {
        Napi::TypeError::New(env, "center, scale, quat, rgba must be Float32Array")
            .ThrowAsJavaScriptException();
        return env.Null();
    }

    auto centerBuf = info[0].As<Napi::Float32Array>();
    auto scaleBuf  = info[1].As<Napi::Float32Array>();
    auto quatBuf   = info[2].As<Napi::Float32Array>();
    auto rgbaBuf   = info[3].As<Napi::Float32Array>();

    const float* shData = nullptr;
    if (!info[4].IsNull() && !info[4].IsUndefined()) {
        auto shBuf = info[4].As<Napi::Float32Array>();
        shData = shBuf.Data();
    }

    const uint32_t* childStart = nullptr;
    const uint16_t* childCount = nullptr;
    if (!info[5].IsNull() && !info[5].IsUndefined()) {
        childStart = info[5].As<Napi::Uint32Array>().Data();
    }
    if (!info[6].IsNull() && !info[6].IsUndefined()) {
        childCount = info[6].As<Napi::Uint16Array>().Data();
    }

    size_t count    = info[7].As<Napi::Number>().Uint32Value();
    int shDegree    = info[8].As<Napi::Number>().Int32Value();

    auto result = splat::encode_rad(
        centerBuf.Data(),
        scaleBuf.Data(),
        quatBuf.Data(),
        rgbaBuf.Data(),
        shData,
        childStart,
        childCount,
        count,
        shDegree
    );

    return Napi::Buffer<uint8_t>::Copy(env, result.data(), result.size());
}

} // namespace node_api::rad_encode
