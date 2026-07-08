# aholo Block + 树遍历 — 实现流程

## 概览

把场景切成 Block，每个 Block 独立建 cycling_lod 树 + level-morton 排序 + RAD 输出。运行时按视锥加载各 Block 的 .rad，在 TS 里做树遍历展开 LOD，全局堆合并排序后渲染。

**Spark 参考代码位置**：所有 `spark-lib/`、`spark-worker-rs/`、`spark/` 引用均指向 `D:\Project\3dgs\spark-2.1.0\`。

### 数据流总览

```
[C++]
splat → gaussian_block → vector<Block>
  → 每 Block: build_lod_tree(vector<Gaussian>) → LodTreeResult
  → rad_encode(LodTreeResult) → .rad file

[TS]
.rad → rad_decode → BlkData
  → BlockTree(BlkData) + pool.upload(BlkData)
  → per frame: block.traverse(object camera) → TraverseOutput
  → render(TraverseOutput)
```

### 关键数据结构

#### Block

```typescript
interface Block {
    id: number;
    bounds: { min: Vec3; max: Vec3 };
    gaussians: Gaussian[];
}
```

C++ 侧类型，来自 gaussian_block.cpp 的 split_gaussians 产出。 每个 Block 包含该块的所有 GS 原始数据和空间范围。

#### LodTreeResult（C++ 产出 → .rad 文件）

```typescript
interface LodTreeResult {
    totalNodes: number; // 原始 GS + merge 节点
    shDegree: number;
    // GS 属性数组（已按 level-morton 排列）
    center: Uint16Array; // f16[3] × totalNodes
    rgb: Uint8Array; // u8[3] × totalNodes
    alpha: Uint8Array; // u8 × totalNodes
    scale: Uint8Array; // u8[3] × totalNodes
    quat: Uint8Array; // u8[3] × totalNodes
    sh: Uint8Array | null; // 取决于 shDegree
    // 树结构
    childStart: Uint32Array; // u32 × totalNodes
    childCount: Uint16Array; // u16 × totalNodes
}
```

这是建树的最终产出。GS 属性 + 树结构全部在这个结构中。RAD 编码器逐属性取出编码为 chunk。rad_decode 反向操作，解码出同结构。

#### BlkData（TS 运行时的 Block 数据）

```typescript
interface BlkData {
    id: number;
    count: number;
    totalNodes: number;
    nodeData: NodeData[]; // 树节点数组，给遍历器用
    textureData: {
        center: Uint16Array; // f16[3] × count，直接用于纹理上传
        rgba: Uint8Array; // u8[3] × count
        scale: Uint8Array; // u8[3] × count
        quat: Uint8Array; // u8[3] × count
        sh: Uint8Array | null;
    };
}
```

RAD 解码后的内存表示。拆分成了两部分：树节点数据（供 traversal 用）和 GS 纹理数据（供 GPU 上传）。

#### NodeData（单节点）

```typescript
interface NodeData {
    center: [number, number, number]; // f16 → f32
    size: number; // f16 → f32
    childStart: number; // 子节点起始索引
    childCount: number; // 子节点数
}
```

遍历器的基本操作单元。

#### TraverseOutput（每帧）

```typescript
interface TraverseOutput {
    indices: Uint32Array; // paged_index，指向纹理页 + 页内偏移
    numSplats: number; // 实际输出 GS 数
}
```

每帧遍历器输出的结果。直接用于渲染。

#### 纹理页映射

```typescript
type PageKey = `${blockId}:${chunk}`;
// 查找: pool.lookup(blockId, chunk) → texture layer index
// 分配: pool.alloc(pageKey) → layer index
// 上传: pool.upload(layer, pageKey, BlkData.textureData)
```

### N-API 函数签名

C++ 侧暴露给 TS 的函数，遵循现有 `api_splat.cpp` 模式（Napi::Buffer 传数组、Napi::Object 返回结构）：

#### build_lod_tree

```typescript
// TS 调用:
import native from 'splat-transform-native';
const result = native.buildLodTree(
  buffers: Float32Array[],    // [x, y, z, sx, sy, sz, qx, qy, qz, qw, r, g, b, a, sh...]
  shSize: number,              // SH 系数数量
  multipliers: Float32Array,   // [1.0, 1.4, 1.7]
  // 返回:
) => {
  childStart: Uint32Array;     // 全树节点 × u32
  childCount: Uint16Array;     // 全树节点 × u16
  center: Uint16Array;         // f16[3] × count
  rgba: Uint8Array;            // u8[4] × count
  scale: Uint8Array;           // u8[3] × count
  quat: Uint8Array;            // u8[3] × count
  sh: Uint8Array | null;       // 可选
  treeNodeCount: number;       // 树节点总数
  gsCount: number;             // 原始 GS 数
};
```

C++ 实现：接收 GS 属性 buffer + 参数 → 调用 cycling_lod 建树 → 返回树结构 + level-morton 排列后的 GS 属性。

**注册名**：`exports.Set("buildLodTree", ...)` 在 `api_lod_tree.cpp`

#### decodeRad

```typescript
// TS 调用:
const result = native.decodeRad(
  fileData: Uint8Array,          // .rad 文件的完整二进制内容
) => {
  count: number;                 // 该 .rad 的 GS 数
  totalNodes: number;            // 树节点总数
  childStart: Uint32Array;       // 树结构
  childCount: Uint16Array;       // 树结构
  // GS 属性（已解码为可直接上传纹理的格式）
  center: Uint16Array;           // f16[3] × count
  rgba: Uint8Array;              // u8[4] × count
  scale: Uint8Array;             // u8[3] × count
  quat: Uint8Array;              // u8[3] × count
  sh: Uint8Array | null;         // 可选
};
```

C++ 实现：解析 header → 遍历 chunk → 解码各属性 → 组装为紧凑数组。

**注册名**：`exports.Set("decodeRad", ...)` 在 `api_rad_decode.cpp`

## 一、构建：分块

复用 aholo 现成的 `gaussian_block.cpp`。从场景 AABB 开始递归 8 等分，每块 GS 数不超过 20 万。GS 按中心坐标准入，边界只分一侧，不复置。叶子 Block 就是最终加载单元。

**参考文件**：`source/src/gaussian/gaussian_block.cpp`
**新增文件**：无
**修改文件**：无（完全复用）

---

## 二、构建：块内建树

每个 Block 内的所有 GS 按 feature_size 从小到大排序。从最小步长开始逐层增长，每一层用当前步长算出每个 GS 的网格坐标（center/step），对网格坐标做 Morton 编码，按 Morton 码排序。同 grid cell 的 GS 合并成一个父节点（加权平均 center/scale/quat/opacity 等），记录 child_start/child_count。单个节点原样传到下一层。所有层级合并到只剩 root 后，按 level-morton 序 permute，产出完整树结构。

**参考文件**：

- `spark-lib/src/cycling_lod.rs`（Rust → C++ 移植）：`compute_step`、Morton 排序、grid merge、levels_output、remap_children 逻辑
- `spark-lib/src/gsplat.rs`：`new_merged` 加权平均合并公式
- `spark-lib/src/ordering.rs`：`morton_coord64_to_index`、`expand3_21` 位操作

**新增文件**：

- `source/src/gaussian/gaussian_lod_tree.cpp` — cycling_lod 建树核心
- `source/include/gaussian/gaussian_lod_tree.h` — 头文件

**修改文件**：

- `source/CMakeLists.txt` — 新增 .cpp 到 `BINDING_SOURCE_FILES`

---

## 三、RAD 编码

建树完成后按 CHUNK_SIZE=16384 切 chunk，RAD 编码为 .rad 文件。每个 Block 输出一个独立的 .rad 文件，格式与 Spark 标准 RAD 完全一致。chunk 中的 child_start/child_count 直接从树结构中读取，不需要修改 RAD 格式。

**参考文件**：

- `spark-lib/src/rad.rs`（Rust → C++ 移植，仅编码部分）：`encode_with_chunks`、`encode_chunk_child_count`、`encode_chunk_child_start`、各属性编码函数（encode_f16、encode_r8delta 等）

**新增文件**：

- `source/src/gaussian/rad_encoder.cpp` — RAD 编码
- `source/include/gaussian/rad_encoder.h` — 头文件

**修改文件**：

- （同上 `CMakeLists.txt`）

---

## 四、构建：lod-meta.json

AutoChunkLodTask.ts 的流程改为：

```
输入 splat → gaussian_block.cpp 分块
  → 对每个 Block：cycling_lod 建树 + RAD 编码
  → 收集各 Block 的 AABB 和 .rad 文件路径
  → 输出 lod-meta.json
```

lod-meta.json 结构：

```json
{
  "magicCode": 0x262834,
  "type": "lod-splat",
  "version": "1.0",
  "counts": 总数,
  "shDegree": ...,
  "levels": 5,
  "files": ["block_0.rad", "block_1.rad", ...],
  "tree": [
    {"bound": {"min": [...], "max": [...]}, "file": 0, "count": 200000},
    ...
  ]
}
```

**参考文件**：

- `splat-transform/src/tasks/AutoChunkLodTask.ts`（修改）：参考原 `generateLod` 编排方式

**新增文件**：

- `source/src/node/api_lod_tree.cpp` — N-API 绑定，暴露 `build_lod_tree` 给 TS

**修改文件**：

- `splat-transform/src/tasks/AutoChunkLodTask.ts` — 替换 `generateLod` 调用
- `source/CMakeLists.txt` — 添加 `api_lod_tree.cpp`

---

## 五、运行时：RAD 解码

每个 .rad 文件独立加载。解析 RAD header → chunk 数据 → 上传 GS 纹理到共享纹理池。树结构（child_start/child_count）保持为 Uint32Array + Uint16Array 在内存中，给遍历器用。

RAD 解码器用 C++ 重写，编译为 aholo 的 Node.js 原生插件（N-API），与现有的 `api_splat.cpp` 相同方式暴露给 TS。不依赖 spark-rs WASM。

**参考文件**：

- `spark-lib/src/rad.rs`（Rust → C++ 移植，解码部分）：`decode_rad_header`、chunk 属性解码
- `source/src/node/api_splat.cpp`（参考 N-API 模式）

**新增文件**：

- `source/src/node/api_rad_decode.cpp` — RAD 解码 N-API 绑定

**修改文件**：

- （同上 `CMakeLists.txt`）

---

## 六、运行时：BlockTree

每个 Block 加载后创建 BlockTree 实例，持有该 Block 的节点数据（center、size、child_start、child_count）。每帧遍历时从 root 开始算 pixel_scale，推入全局堆。所有 Block 的节点共享同一个堆。

遍历展开逻辑：pop 堆顶 → 如果 pixel_scale ≤ 阈值则停止；如果是叶子（child_count=0）输出 paged_index；否则展开所有子节点，算 pixel_scale，≥ 阈值的入堆，≤ 的直接输出。

pixel_scale = size / distanceToCamera × lodScale × foveation。

**参考文件**：

- `spark-worker-rs/src/lod_tree.rs`（翻译为 TS）：`traverse_lod_trees` 堆遍历逻辑、`compute_pixel_scale` 含 foveation
- `spark/src/SparkRenderer.ts`：`compute_pixel_scale` 的 JS 实现（1450 行附近）

**新增文件**：

- `packages/renderer/src/block-tree.ts` — TS 树遍历器

**修改文件**：

- 无

---

## 七、运行时：SharedTexturePool

全局纹理页池，所有 Block 共用。用 `(blockId, chunk)` 做复合键查询页面 → texture layer 映射。分配时优先用空闲 layer，不够时淘汰 LRU 中最旧的页。每页 256×256 RGBA32UI，存 16384 GS。

**参考文件**：

- `spark/src/SplatPager.ts`：`newUint32ArrayTexture`（DataArrayTexture 创建，637-658 行）、`uploadPage`（写入纹理 layer，1085-1120 行）、`allocatePage`/`freePage`（页分配，1009-1015 行）、LRU 淘汰逻辑（1350-1375 行）

**新增文件**：

- `packages/renderer/src/shared-texture-pool.ts`

**修改文件**：

- 无

---

## 八、运行时：BlockManager

每帧视锥测试 + 距离排序 → 决定活跃 Block 列表。状态机：free → loading → active → fading → free。

Block 在视锥内且距相机不超过 PRELOAD_DIST × AABB 对角线的，发起加载或标记 active。距相机超过 EVICT_DIST × AABB 对角线的进入 fading，8 帧衰减后释放。fading 期间仍然渲染，opacity 逐帧衰减。

**参考文件**：

- 无（新设计，无现有参考）

**新增文件**：

- `packages/renderer/src/block-manager.ts`

**修改文件**：

- 无

---

## 九、渲染循环

aholo 的 Splatting plugin 已有 `orderTex` 机制：每帧读取 GS 深度排序后写入 orderTex（R32Uint 纹理），顶点着色器读 `orderTex[gl_InstanceID]` 拿到要渲染的 GS 索引。这与 Spark 的 `lodIndices` 一致。

遍历输出直接写入 orderTex：

```
每帧：
  1. blockManager.update(camera) → active Block 列表
  2. 创建全局堆
  3. 对每个 active Block：BlockTree.traverse(camera, 全局堆)
  4. 从全局堆 pop 前 N 个输出 → traverseOutput: Uint32Array
  5. 直接写入 orderTex 纹理（替代深度排序）：
     reorderMaterial.orderTex.setLevelData(traverseOutput)
     splattingMaterial.count = N
     splattingGeometry.instancedCount = ceil(N / 128)
  6. GPU 按 orderTex 顺序绘制 N 个 GS
```

不需要修改 aholo 引擎的 shader。每个 Block 的 GS 纹理作为 CompressedSplat 上传，按 Block 分配独立纹理页即可。

**参考文件**：

- `external/egs-core/packages/egs/src/fx/plugins/Splatting.ts`：`orderTex` 创建（220-245 行）、`instancedCount` 设置（224-228 行）
- `external/egs-core/packages/egs/src/scene/splat/CompressedSplat.ts`：GS 属性纹理

**新增文件**：无
**修改文件**：

- `packages/renderer/src/index.ts` — 导出新增模块
- `website/src/client/viewer.ts` — 新增 Block 模式加载路径，替代 LodSplat

---

## 实现顺序

### 第一梯队（无依赖，可并行）

```
A: morton_code.h ← 纯位运算，从 Spark ordering.rs 复制
B: rad_encoder.cpp ← 数据编码，不依赖建树
C: rad_decoder.cpp ← 数据解码，可从已知 .rad 文件验证
```

### 第二梯队（逐个依赖第一梯队）

```
D: gaussian_lod_tree.cpp ← 依赖 A
   cycling_lod 建树 + level-morton permute
E: api_lod_tree.cpp ← N-API 绑定，暴露 buildLodTree
   TS 端可调用后验证单 Block 建树
```

### 第三梯队（TS 运行时，可并行）

```
F: block-tree.ts ← 独立的 TS 堆遍历，可用写死的树结构模拟测试
G: block-manager.ts ← 视锥测试 + 状态机，可独立测试
H: shared-texture-pool.ts ← 纹理页分配 + LRU
```

### 第四梯队（依赖第二、三梯队）

```
I: AutoChunkLodTask.ts ← 编排 buildLodTree → 输出 .rad
J: 渲染循环 ← orderTex 写入 + instancedCount 控制
K: viewer.ts ← 串联 blockManager → traverse → orderTex
```

**可并行**：第一梯队 A/B/C 三人可同时开工。第三梯队 F/G/H 三人可同时开工。
**不可并行**：D 依赖 A，E 依赖 D，I 依赖 B+E，J/K 依赖 F/G/H+E。

## 关键参数

| 参数               | 值              | 说明                        |
| ------------------ | --------------- | --------------------------- |
| MAX_BLOCK_GS       | 200,000         | 单 Block GS 上限            |
| MAX_ACTIVE_BLOCKS  | 4               | 同时活跃 Block 上限         |
| CHUNK_SIZE         | 16384           | 每 chunk GS 数              |
| PRELOAD_DIST       | 2.0             | 预加载距离（× AABB 对角线） |
| EVICT_DIST         | 2.5             | 淘汰距离                    |
| FADE_FRAMES        | 8               | fading 过渡帧数             |
| TEXTURE_POOL_PAGES | 64              | 纹理池总页数                |
| BASE               | 2.0             | cycling_lod base            |
| MULTIPLIERS        | [1.0, 1.4, 1.7] | cycling_lod 乘数组          |
