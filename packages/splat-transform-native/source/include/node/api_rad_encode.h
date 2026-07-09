#pragma once
#include <napi.h>

namespace node_api::rad_encode {

Napi::Value encodeRad(const Napi::CallbackInfo& info);

} // namespace node_api::rad_encode
