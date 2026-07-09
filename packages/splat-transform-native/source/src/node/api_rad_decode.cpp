#include <napi.h>
#include "splat/rad_decoder.h"
#include "splat/lod_tree.h"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

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
        auto arr = Napi::Uint32Array::New(env, result.childStart.size());
        std::memcpy(arr.Data(), result.childStart.data(), result.childStart.size() * sizeof(uint32_t));
        obj.Set("childStart", arr);
    }
    if (!result.childCount.empty()) {
        auto arr = Napi::Uint16Array::New(env, result.childCount.size());
        std::memcpy(arr.Data(), result.childCount.data(), result.childCount.size() * sizeof(uint16_t));
        obj.Set("childCount", arr);
    }
    if (!result.center.empty()) {
        auto arr = Napi::Uint16Array::New(env, result.center.size());
        std::memcpy(arr.Data(), result.center.data(), result.center.size() * sizeof(uint16_t));
        obj.Set("center", arr);
    }
    if (!result.rgba.empty()) {
        auto arr = Napi::Uint8Array::New(env, result.rgba.size());
        std::memcpy(arr.Data(), result.rgba.data(), result.rgba.size() * sizeof(uint8_t));
        obj.Set("rgba", arr);
    }
    if (!result.scale.empty()) {
        auto arr = Napi::Uint8Array::New(env, result.scale.size());
        std::memcpy(arr.Data(), result.scale.data(), result.scale.size() * sizeof(uint8_t));
        obj.Set("scale", arr);
    }
    if (!result.quat.empty()) {
        auto arr = Napi::Uint8Array::New(env, result.quat.size());
        std::memcpy(arr.Data(), result.quat.data(), result.quat.size() * sizeof(uint8_t));
        obj.Set("quat", arr);
    }
    if (!result.sh.empty()) {
        auto arr = Napi::Uint8Array::New(env, result.sh.size());
        std::memcpy(arr.Data(), result.sh.data(), result.sh.size() * sizeof(uint8_t));
        obj.Set("sh", arr);
    }

    return obj;
}

Napi::Value traverseBlock(const Napi::CallbackInfo& info) {
    auto env = info.Env();

    // Args: childStart, childCount, center (f16), size (f16), cameraPos, maxSplats, lodScale, pixelScaleLimit
    if (info.Length() < 8) {
        Napi::TypeError::New(env, "Expected 8 arguments").ThrowAsJavaScriptException();
        return env.Null();
    }

    auto childStartBuf = info[0].As<Napi::Buffer<uint32_t>>();
    auto childCountBuf = info[1].As<Napi::Buffer<uint16_t>>();
    auto centerBuf = info[2].As<Napi::Buffer<uint16_t>>();  // f16[3] per node
    auto sizeBuf = info[3].As<Napi::Buffer<uint16_t>>();     // f16 per node
    auto cameraPosBuf = info[4].As<Napi::Buffer<float>>();   // [x, y, z]
    size_t maxSplats = info[5].As<Napi::Number>().Uint32Value();
    float lodScale = info[6].As<Napi::Number>().FloatValue();
    float pixelScaleLimit = info[7].As<Napi::Number>().FloatValue();

    size_t totalNodes = childStartBuf.Length();
    const uint32_t* childStart = childStartBuf.Data();
    const uint16_t* childCount = childCountBuf.Data();
    const uint16_t* centerF16 = centerBuf.Data();
    const uint16_t* sizeF16 = sizeBuf.Data();
    const float* cameraPos = cameraPosBuf.Data();

    // Decode f16 → f32 for center and size, compute pixel scales, traverse
    auto outBuf = Napi::Buffer<uint32_t>::New(env, maxSplats);
    uint32_t* outIndices = outBuf.Data();

    // Simple f16→f32 conversion (IEEE 754-2008 half float)
    auto f16_to_f32 = [](uint16_t h) -> float {
        uint32_t sign = (h & 0x8000) << 16;
        uint32_t exp = (h >> 10) & 0x1f;
        uint32_t mant = h & 0x3ff;
        if (exp == 0) {
            // subnormal
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            exp += 112; // exp - 127 + 15
        } else if (exp == 31) {
            exp = 255; // inf/nan
        } else {
            exp += 112;
        }
        uint32_t f = sign | (exp << 23) | (mant << 13);
        float result;
        memcpy(&result, &f, 4);
        return result;
    };

    // Allocate f32 arrays for traversal
    std::vector<float> centerF32(totalNodes * 3);
    std::vector<float> sizeF32(totalNodes);
    for (size_t i = 0; i < totalNodes; i++) {
        centerF32[i * 3] = f16_to_f32(centerF16[i * 3]);
        centerF32[i * 3 + 1] = f16_to_f32(centerF16[i * 3 + 1]);
        centerF32[i * 3 + 2] = f16_to_f32(centerF16[i * 3 + 2]);
        sizeF32[i] = f16_to_f32(sizeF16[i]);
    }

    // Camera forward not used in simple version
    float dummyForward[3] = {0, 0, 1};

    size_t count = splat::traverse_block(
        childStart, childCount,
        centerF32.data(), sizeF32.data(),
        totalNodes,
        cameraPos, dummyForward,
        lodScale, pixelScaleLimit,
        maxSplats, outIndices
    );

    auto result = Napi::Object::New(env);
    result.Set("indices", outBuf);
    result.Set("numSplats", (uint32_t)count);
    return result;
}

} // namespace node_api::rad
