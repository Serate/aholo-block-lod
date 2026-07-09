#pragma once
#include <napi.h>

namespace node_api::lod_tree {

Napi::Value buildLodTree(const Napi::CallbackInfo& info);

} // namespace node_api::lod_tree
