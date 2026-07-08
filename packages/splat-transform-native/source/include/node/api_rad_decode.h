#pragma once
#include <napi.h>

namespace node_api::rad {

Napi::Value decodeRad(const Napi::CallbackInfo& info);

} // namespace node_api::rad
