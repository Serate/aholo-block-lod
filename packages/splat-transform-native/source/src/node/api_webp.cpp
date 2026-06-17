#include <node/api_buffer.h>
#include <node/api_webp.h>
#include <span>
#include <webp/decode.h>
#include <webp/encode.h>

namespace {
size_t webp_encode_rgba(const std::span<uint8_t>& data, int32_t width, int32_t height, bool lossless, float quality, uint8_t** output) {
    WebPPicture pic;
    WebPConfig config;
    WebPMemoryWriter wrt;
    int ok;

    if (output == nullptr) {
        return 0;
    }

    if (!WebPConfigPreset(&config, WEBP_PRESET_DEFAULT, quality) ||
        !WebPPictureInit(&pic)) {
        return 0; // shouldn't happen, except if system installation is broken
    }

    config.lossless = static_cast<int32_t>(lossless);
    config.exact = 1;
    pic.use_argb = static_cast<int32_t>(lossless);
    pic.width = width;
    pic.height = height;
    pic.writer = WebPMemoryWrite;
    pic.custom_ptr = &wrt;
    WebPMemoryWriterInit(&wrt);

    ok = WebPPictureImportRGBA(&pic, data.data(), 4 * width) && WebPEncode(&config, &pic);
    WebPPictureFree(&pic);
    if (!ok) {
        WebPMemoryWriterClear(&wrt);
        return 0;
    }
    *output = wrt.mem;
    return wrt.size;
}
} // namespace

namespace node_api::imaging {
Napi::Value webp_encode_rgba(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 4 || !info[0].IsBuffer() || !info[1].IsNumber() || !info[2].IsNumber() || !info[3].IsNumber()) {
        Napi::TypeError::New(env, "Wrong Arguments").ThrowAsJavaScriptException();
        return env.Null();
    }
    auto buffer = info[0].As<Napi::Buffer<uint8_t>>();
    auto width = info[1].As<Napi::Number>().Int32Value();
    auto height = info[2].As<Napi::Number>().Int32Value();
    auto quality = info[3].As<Napi::Number>().FloatValue();
    auto span = std::span<uint8_t>(buffer.Data(), buffer.Length());

    uint8_t* output = nullptr;
    auto compressed = ::webp_encode_rgba(span, width, height, false, quality, &output);
    if (compressed == 0) {
        Napi::TypeError::New(env, "WebP Encode Failed").ThrowAsJavaScriptException();
        return env.Null();
    }
    return node_api::buffer::UniqueBufferFinalizer<uint8_t, decltype(&WebPFree)>::make_buffer(env, std::unique_ptr<uint8_t, decltype(&WebPFree)>(output, WebPFree), compressed);
}

Napi::Value webp_encode_rgba_lossless(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 3 || !info[0].IsBuffer() || !info[1].IsNumber() || !info[2].IsNumber()) {
        Napi::TypeError::New(env, "Wrong Arguments").ThrowAsJavaScriptException();
        return env.Null();
    }

    auto buffer = info[0].As<Napi::Buffer<uint8_t>>();
    auto width = info[1].As<Napi::Number>().Int32Value();
    auto height = info[2].As<Napi::Number>().Int32Value();
    auto span = std::span<uint8_t>(buffer.Data(), buffer.Length());

    uint8_t* output = nullptr;
    auto compressed = ::webp_encode_rgba(span, width, height, true, 70, &output);
    if (compressed == 0) {
        Napi::TypeError::New(env, "WebP Encode Failed").ThrowAsJavaScriptException();
        return env.Null();
    }
    return node_api::buffer::UniqueBufferFinalizer<uint8_t, decltype(&WebPFree)>::make_buffer(env, std::unique_ptr<uint8_t, decltype(&WebPFree)>(output, WebPFree), compressed);
}

Napi::Value webp_decode_rgba(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info.Length() < 1 || !info[0].IsBuffer()) {
        Napi::TypeError::New(env, "Wrong Arguments").ThrowAsJavaScriptException();
        return env.Null();
    }
    auto buffer = info[0].As<Napi::Buffer<uint8_t>>();
    int32_t width = 0;
    int32_t height = 0;
    auto rgba = WebPDecodeRGBA(buffer.Data(), buffer.Length(), &width, &height);
    if (rgba == nullptr) {
        Napi::TypeError::New(env, "WebP Decode Failed").ThrowAsJavaScriptException();
        return env.Null();
    }
    auto object = Napi::Object::New(env);
    {
        object.Set("data", node_api::buffer::UniqueBufferFinalizer<uint8_t, decltype(&WebPFree)>::make_buffer(env, std::unique_ptr<uint8_t, decltype(&WebPFree)>(rgba, WebPFree), width * height * 4));
        object.Set("width", Napi::Number::New(env, width));
        object.Set("height", Napi::Number::New(env, height));
    }
    return object;
}
} // namespace node_api::imaging
