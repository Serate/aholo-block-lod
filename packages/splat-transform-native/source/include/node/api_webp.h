#pragma once
#include <napi.h>

namespace node_api::imaging {
Napi::Value webp_encode_rgba_lossless(const Napi::CallbackInfo& info);
Napi::Value webp_encode_rgba(const Napi::CallbackInfo& info);
Napi::Value webp_decode_rgba(const Napi::CallbackInfo& info);
} // namespace node_api::imaging
