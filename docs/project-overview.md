# 3DGS Block + 树遍历 LOD 项目总览

## 总目标

将大场景 3DGS 文件（如 4.38M GS）分割为空间 Block，每块独立构建 cycling_lod + level-morton 排序 + RAD 编码。运行时在浏览器端做树遍历展开 LOD（pixel_scale 驱动），通过 orderTex 机制控制 GPU 渲染顺序，实现 **VRAM 可控、视角自适应 LOD 的 3DGS 渲染**。

## 架构

```
输入 .splat
  → CLI 处理（Node.js + C++）:
      空间分块 → 每块 cycling_lod 建树 → RAD 编码 + .splat 输出
  → Web viewer（浏览器 + aholo 引擎）:
      加载块列表（lod-meta.json）
      合并块数据为一个 Splat（当前演示阶段）
      后续: 每帧 blockManager 视锥筛选 → TS traverseBlock → orderTex → GPU 渲染
```

## 关键参数

| 参数               | 值              | 说明                |
| ------------------ | --------------- | ------------------- |
| MAX_BLOCK_GS       | 200,000         | 单块 GS 上限        |
| MAX_ACTIVE_BLOCKS  | 4               | 同时活跃块数        |
| CHUNK_SIZE         | 16384           | 纹理页大小          |
| PRELOAD_DIST       | 2.0             | 预加载距离(×对角线) |
| EVICT_DIST         | 2.5             | 淘汰距离            |
| FADE_FRAMES        | 8               | 淡出帧数            |
| TEXTURE_POOL_PAGES | 64              | 纹理池页数          |
| BASE               | 2.0             | cycling_lod base    |
| MULTIPLIERS        | [1.0, 1.4, 1.7] | cycling_lod 乘数    |

## 完成状态

### 第一梯队：C++ 核心 ✅

- [x] `morton_code.h` — Morton 编码函数
- [x] `rad_encoder.cpp` — RAD 编码（zlib deflate，tag-byte framing）
- [x] `rad_decoder.cpp` — RAD 解码（zlib inflate，U16 TypedArray 修复）
- [x] `lod_tree.cpp` — cycling_lod 建树 + level-morton permute + traverse_block BFS 遍历

### 第二梯队：N-API 绑定 ✅

- [x] `api_lod_tree.cpp` — buildLodTree
- [x] `api_rad_encode.cpp` — encodeRad
- [x] `api_rad_decode.cpp` — decodeRad（修复 Buffer→Uint16Array）
- [x] `binding.cpp` — 注册所有导出
- [x] CMakeLists.txt — 添加新源文件
- [x] cmake-js 本地构建（local preset，绕过 vcpkg）

### 第三梯队：TS 运行时 ✅

- [x] `SharedTexturePool` — 纹理页池 LRU
- [x] `BlockManager` — 状态机，距离驱动
- [x] `BlockTree` — traverse wrapper
- [x] `BlockLodRenderer` — 编排 update→traverse→setExternalOrder

### 第四梯队：SplattingPlugin 集成 ✅

- [x] `SplattingPlugin.setExternalOrder()` — orderTex + instancedCount
- [x] `getSplattingPlugin()` — 全局引用
- [x] `setViewerConfig` — 黑色背景 + SplattingPlugin 启用

### 第五梯队：CLI 处理 ✅

- [x] `process-block-splat.mjs` — .splat→分块→建树→.rad+.splat→lod-meta.json
- [x] garden-7k.splat 已处理为 22 块

### 第六梯队：Web 展示 ✅

- [x] `rad-decoder-browser.ts` — 浏览器 RAD 解码（DecompressionStream API）
- [x] `block-viewer.astro` — 分块加载并显示
    - [x] 修复 SplatFileType.SPLAT 枚举名
    - [x] 修复 createSplat 导入路径（SplatUtils）
    - [x] 修复 SplatPackType（Raw→Compressed）
    - [x] 添加渲染循环（rAF + requestRenderHandler）
    - [x] 添加 CameraControl（鼠标/WASD 控制）
    - [x] 添加 setViewerConfig（黑色背景 + SplattingPlugin）
    - [x] 添加 FPS 显示
    - [x] 合并 22 块数据为单个 Splat（性能提升）
    - [x] OpenCV Y-down 坐标系
- [x] Vite optimizeDeps exclude 修复（@qunhe 包）

### 第七梯队：渲染循环（浏览器 LOD）— ✅

- [x] TS traverseBlock — 将 C++ traverse_block（~75行 BFS）移植为 TypeScript
    - 零外部依赖，使用自定义 binary max-heap
    - 输入: TreeData（childStart/childCount/centers/featureSizes）+ 相机参数
    - 输出: 按 pixel_scale 排序的 GS 索引数组
    - BlockTree/BlockLodRenderer 已更新使用 TS traverse
    - 单元测试验证（6 个场景：低阈值、高阈值、远距离、maxSplats限制、空树、featureSize计算）
- [ ] BlockViewer 完整集成 — 加载 .rad 文件 → decodeRad → 创建 TreeData → 每帧 traverse → setExternalOrder

### 第八梯队：构建 / 基础设施

- [ ] cmake-js vcpkg 完整构建（vcpkg 网络恢复后）

## 当前架构决策

### 为什么不用 WASM 移植 traverseBlock

C++ `traverse_block` 只有 75 行，本质是 BFS 优先队列 + pixel_scale 计算（纯数学运算）。没有任何平台依赖或 C++ 特有的功能。直接翻译为 TypeScript：

- **零额外依赖**，无需 Emscripten 工具链
- **类型安全**，数据直接在 JS heap 上操作
- **易于调试**，浏览器 sourcemap 直接定位
- 工作量：~4 小时 vs WASM 方案的 3-5 天

### 为什么合并块数据为单个 Splat

22 个独立 Splat 对象 vs 1 个大 Splat 的性能差距巨大——每个 Splat 有独立的 GPU 纹理、状态切换和 draw call 开销。block-viewer 演示阶段使用原始 .splat 拼接方式合并为单个 Splat，性能和主 viewer 一致。

后续 LOD 实现后，每个块独立管理，但会通过 BlockManager 做预算控制和动态加载/卸载，不会同时保持 22 个 Splat 在 GPU 上。

### 参考实现：主 viewer LodSplat

主 viewer 的 LodSplat 实现了完整的 block+LOD 系统：

- 预算管理（maxBudget=300万 GS）
- 距离驱动 LOD 级别选择
- 相邻 block 自动合并（同资源文件内的连续数据）
- 动态加载/卸载

我们的方案不同点在于 LOD 精度控制方式——LodSplat 用预设 level，我们用 traversal 的 pixel_scale 阈值。但预算管理、视锥裁剪、动态加载等策略可借鉴。

## 文件索引

| 路径                                                               | 说明                               |
| ------------------------------------------------------------------ | ---------------------------------- |
| `scripts/process-block-splat.mjs`                                  | CLI 处理工具                       |
| `packages/renderer/src/block-manager.ts`                           | 块状态机                           |
| `packages/renderer/src/block-renderer.ts`                          | 编排器                             |
| `packages/renderer/src/block-tree.ts`                              | 遍历封装                           |
| `packages/renderer/src/shared-texture-pool.ts`                     | 纹理池                             |
| `packages/renderer/src/rad-decoder-browser.ts`                     | 浏览器 RAD 解码                    |
| `packages/splat-transform-native/source/src/splat/lod_tree.cpp`    | C++ 建树+遍历                      |
| `packages/splat-transform-native/source/src/splat/rad_encoder.cpp` | C++ RAD 编码                       |
| `packages/splat-transform-native/source/src/splat/rad_decoder.cpp` | C++ RAD 解码                       |
| `external/egs-core/packages/egs/src/fx/plugins/Splatting.ts`       | SplattingPlugin + setExternalOrder |
| `website/src/pages/[lang]/block-viewer.astro`                      | Block viewer 页面（已完成）        |
| `website/public/block-data/`                                       | garden-7k 处理后的块数据           |
| `docs/project-overview.md`                                         | 本文件                             |
