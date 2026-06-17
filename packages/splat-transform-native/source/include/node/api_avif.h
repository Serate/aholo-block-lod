#pragma once
#include <napi.h>

namespace node_api::imaging {
Napi::Value avif_encode_rgba(const Napi::CallbackInfo& info);
Napi::Value avif_encode_rgba_batched(const Napi::CallbackInfo& info);
Napi::Value avif_decode_rgba(const Napi::CallbackInfo& info);
Napi::Value avif_decode_rgba_batched(const Napi::CallbackInfo& info);
} // namespace node_api::imaging
