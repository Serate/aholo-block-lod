#pragma once
#include <napi.h>

namespace node_api::splat {
Napi::Value generate_lod(const Napi::CallbackInfo& info);
Napi::Value split(const Napi::CallbackInfo& info);
} // namespace node_api::splat
