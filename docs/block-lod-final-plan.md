# Block LOD 渲染方案

## 架构总览

```
初始化:
  decodeRad × ALL blocks（所有 .rad 都解码，TreeData 都构建，轻量快速）
  ├── 视锥内 block → 创建 Splat（GPU 纹理），sc.add(splat)
  └── 视锥外 block → 不创建 Splat（不占 GPU 内存）

每帧:
  BlockManager.update(camera)  → 参考 aholo LodSplat.tick()
    ├── 检查视锥变化 → 新进入的 block → 异步创建 Splat，加载完成后 sc.add
    └── 离开视锥的 block → sc.remove(splat)，释放 GPU 资源

  所有已有 Splat 的 block 的 root 同时入堆  → 参考 Spark traverse_lod_trees()
  单次 BFS 遍历（非 22 次串行）
    └── per-block 均分预算
    └── 堆按 pixel_scale 全局竞争
    └── 预算耗尽 → 剩余节点 flush 为 LOD

  depthSort → setExternalOrder
```

---

## 阶段 1：初始化

**参考代码：**

- `website/src/pages/[lang]/bv-lod-test.astro` — 当前的加载逻辑
- `packages/renderer/src/rad-decoder-browser.ts` — `decodeRad()`
- `packages/renderer/src/block-tree.ts` — `addBlock()` / `prepareTreeData()`
- `packages/renderer/src/block-manager.ts` — `update()` 视锥计算

```
1. fetch lod-meta.json
2. decodeRad × ALL blocks（所有 block 的 TreeData 都构建）
     TreeData 轻量（~几 MB），用于遍历
3. 计算初始视锥
4. 对视锥内 block: createSplat + sc.add(splat)
     视锥外 block: 跳过（不创建 Splat）
```

### 为什么 decodeRad × ALL blocks

- `.rad` 解码 ≈ 22 × 200ms = ~4.4s，可以接受（一次性）
- TreeData 仅存 `childStart`/`childCount`/`centers`/`featureSizes`，约几 MB
- 解码后如果 block 不在视锥内，不创建 Splat 即可（Splat 是 GPU 纹理，125MB 的大头）

### 为什么 Splat 创建是瓶颈

当前用 `decodedBlockToSplatData()` + `setCenter/setScale...` 逐节点循环，每个 block ~200ms。可以用**直接缓冲复制**优化（见下方）。

---

## 阶段 1.5：Splat 创建优化（直接缓冲复制）

**参考代码：**

- `external/egs-core/packages/loaders/splat-loader/splat/SuperCompressedSplatData.ts` — `init()`, `setCenter()`, `setScale()`, `setColor()`, `setAlpha()`, `serialize()` 了解内部缓冲布局
- `website/src/pages/[lang]/bv-lod-test.astro:97-109` — 当前的 `scd.setCenter(globalIdx, ...)` 逐节点调用

当前：

```typescript
for each node: scd.setCenter(...); scd.setScale(...); ...
```

每个节点走 f32 → f16 / Ln0R8 / Oct88R8 的编码往返，慢。

优化后：

```typescript
// .rad 解码后的数据已经是 f16/Ln0R8/Oct88R8/u8 格式
// SuperCompressedSplatData 内部格式与之完全相同
const buf16 = (scd as any).splatUint16Buffer;
const buf8 = (scd as any).splatUint8Buffer;
for each node:
  buf16[gi*8]   = db.center[i*3];     // f16 → f16（直写, 无需转换）
  buf8[gi*16+6] = db.scale[i*3];       // Ln0R8 → Ln0R8
  buf8[gi*16+9] = db.quat[i*3];        // Oct88R8 → Oct88R8
  buf8[gi*16+12] = db.rgba[i*4];       // u8 → u8
```

从 ~200ms/block 降到 ~5ms/block，动态加载时可接受。

---

## 阶段 2：动态加载

**参考代码：**

- Spark: `rust/spark-worker-rs/src/lod_tree.rs:515-524` — chunk 未加载时输出父节点
- Spark: `src/SplatPager.ts` — `driveFetchers()`, `fetchDecodeChunk()`, LRU 页面管理
- Spark: `src/SparkRenderer.ts:1291-1316` — `consumeLodTreeUpdates()` 处理新加载的块
- aholo: `external/egs-core/packages/utils/splat-utils/lod/index.ts:216-527` — `flush()` 异步加载/卸载逻辑
- `D:/Project/3dgs/spark-main/examples/editor/index.html:415-424` — .rad 文件加载入口

### Spark 的做法

Spark 的 `traverse_lod_trees()` 返回 `chunks: [chunkId, ...]` 表示遍历过程中遇到了哪些未加载的块。
未加载的块中，父节点替代子节点渲染（不展开），同时启动异步加载。
`SplatPager.driveFetchers()` 每次帧取优先级最高的 chunk 发起 `fetchDecodeChunk()`。
加载完成后，块数据进入 GPU 纹理，下一帧遍历时该块的子节点可展开。

### 我们的做法

```
每帧:
  blockManager.update(camera):
    1. 计算视锥
    2. 检查哪些 block 从视锥外 → 视锥内（新进入）
    3. 检查哪些 block 从视锥内 → 视锥外（离开）

  新进入视锥:
    → 标记为 Pending
    → 异步创建 Splat（用直接缓冲复制）
    → 完成后 sc.add(splat) + 标记为 Loaded

  离开视锥:
    → sc.remove(splat) + 释放 GPU 资源

  遍历:
    → 只有 Loaded 状态的 block 参与
    → Pending 或 Unloaded 的 block 不参与
```

### 状态机

```
Unloaded → [进入视锥] → Pending → [Splat 创建完成] → Loaded → [离开视锥] → Unloaded
```

### 参考 Spark 的容错设计

Spark 在遍历时检测 chunk 未加载 → 不展开子节点，直接输出父节点作为 LOD 表示。

我们的等价做法：Pending 状态的 block 不参与遍历，不输出任何 GS → 该区域暂时不渲染（无数据可用）。

**区别：** Spark 有"降级"（输出父节点），我们没有。因为我们的父节点和叶节点在同一个 Splat 中，而 Splat 还没创建时，什么节点都渲染不了。

---

## 阶段 3：运行时遍历（单堆合并）

**参考代码：**

- `packages/renderer/src/traverse-block.ts:207-323` — `_standardTraverse()` BFS 遍历算法（复用到单堆）
- `packages/renderer/src/traverse-block.ts:136-189` — `computePixelScale()` 像素尺度计算
- `packages/renderer/src/block-tree.ts:70-129` — 当前 `traverse()` 方法（将重写）
- Spark: `rust/spark-worker-rs/src/lod_tree.rs:412-599` — `traverse_lod_trees()` 多实例同时遍历
- Spark: `rust/spark-worker-rs/src/lod_tree.rs:601-629` — `compute_pixel_scale()` 含 foveation

```
traverse(loadedBlockIds, config):
  perBlockBudget = floor(maxSplats / loadedBlockIds.length)

  所有 loaded block 的 root 同时入堆
  while 预算未耗尽:
    弹最大 pixelScale 节点
    若该 block 已花完预算 → 跳过
    若 pixelScale ≤ limit → 输出（全局索引）
    否则 → 展开子节点

  depthSort → setExternalOrder
```

## 改动范围

| 文件                | 改动                                                       |
| ------------------- | ---------------------------------------------------------- |
| `block-tree.ts`     | 重写 `traverse()` 为单堆合并遍历                           |
| `block-renderer.ts` | 加入动态加载逻辑（新进/离开视锥检测 + Splat 异步创建）     |
| `bv-lod-test.astro` | 分离 decodeRad 和 createSplat，只在视锥内 block 创建 Splat |

### 不动的文件

- `block-manager.ts`
- `traverse-block.ts`
- `lod_tree.cpp`
- `rad_encoder.cpp`

---

## LRU 缓存管理

**参考代码：**

- Spark: `src/SplatPager.ts` — 页面 LRU 管理（`markUsed()`, `lastUsedFrame`, `freePage()`）
- Spark: `src/SplatPager.ts:1025-1109` — `driveFetchers()` 优先级排序 + LRU 驱逐
- aholo: `external/egs-core/packages/utils/splat-utils/lod/index.ts:459-527` — LodSplat 的 proxy 切换（add/remove）

### BlockManager 视锥计算

**参考代码：**

- `packages/renderer/src/block-manager.ts:113-146` — 当前 `update()` 方法（已重写）
- aholo: `external/egs-core/packages/utils/splat-utils/lod/index.ts:530-563` — `tick()` 的视锥 + 距离 + 后方权重计算

### 设计

所有 block 的 TreeData 始终在内存中（轻量），Splat（GPU 纹理）只在 LRU 缓存中保留。

```
配置:
  MAX_RESIDENT_BLOCKS = 22   // GPU 最大驻留 block 数（不限制）
  MAX_IDLE_FRAMES = 60      // 闲置 N 帧后自动驱逐

状态:
  Loaded + lastUsedFrame    // 在 LRU 缓存中
  Unloaded                  // 不在 GPU 中（TreeData 仍保留）

访问:
  遍历命中某 block → lastUsedFrame = currentFrame

加载（block 进入视锥）:
  if Loaded 数 < MAX_RESIDENT_BLOCKS:
    直接创建 Splat（直接缓冲复制 ~5ms/block）
  else:
    驱逐 lastUsedFrame 最小的 block（LRU）
    释放其 GPU 资源
    创建新 block 的 Splat

卸载（block 离开视锥）:
  每帧检查 Loaded block:
    if currentFrame - lastUsedFrame > MAX_IDLE_FRAMES:
      自动驱逐，释放 GPU 资源
```

### 示例

```
8 个 block 驻留 GPU，视锥内有 5 个，视锥外 3 个（因 LRU 暂未驱逐）

帧 1: 视锥内 5 个被遍历命中 → lastUsedFrame 更新
      视锥外 3 个未被命中 → lastUsedFrame 保持旧值

帧 60: 视锥外 3 个的 idle 帧数超过 60 → 自动驱逐 → 释放 GPU

帧 61: 用户旋转相机，视锥内出现新 block
       当前 Loaded 数 = 5（有空位）→ 直接加载
       若 Loaded 数 = 8（满）→ 驱逐 LRU block 后加载
```

### 与 Spark 的对比

| 维度     | Spark（SplatPager）  | 本方案                 |
| -------- | -------------------- | ---------------------- |
| 缓存粒度 | 16384 GS / page      | 整个 block（~350K GS） |
| TreeData | 在 WASM 中           | 始终在 JS 内存中       |
| 重载成本 | 网络请求 + WASM 解码 | 直接缓冲复制 ~5ms      |
| 驱逐策略 | LRU（使用计数）      | LRU（idle 帧数）       |
