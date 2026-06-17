#pragma once
#include <napi.h>

namespace node_api::spatial {
Napi::Value cluster_average(const Napi::CallbackInfo& info);
}
