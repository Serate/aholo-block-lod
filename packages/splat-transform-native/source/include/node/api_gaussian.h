#pragma once
#include <napi.h>

namespace node_api::gaussian {
Napi::Value generate_lod(const Napi::CallbackInfo& info);
} // namespace node_api::gaussian
