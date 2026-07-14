# 完整 Issue 清单（待修复）

## 问题 1：setExternalOrder 的 order 数组越界 → WebGL 错误

**文件：** `packages/renderer/src/block-renderer.ts:123`

```typescript
plugin.setExternalOrder(order, count);
```

`setExternalOrder` 内部用 `order.subarray(0, w*h)` 创建重排纹理，其中 `w*h >= count`。但 `order.length = count`（来自 BlockTree.traverse 的 depthSort 结果），导致 `w*h > order.length` → `texSubImage2D: ArrayBufferView not big enough`。

**修复：** 确保 order 数组长度至少为 `w*h`。

---

## 问题 2：`_standardTraverse` 中 `numSplats` 在 budget 不足时未正确更新

**文件：** `packages/renderer/src/traverse-block.ts:272-279`

当 `newTotal > maxSplats` 时，节点被 pop 并输出，但 `numSplats` 只减了 1（已修）。但更根本的：这个节点的 children count（cnt）可能远大于 budget，说明该节点的子节点数超大，展开会超出预算。

**影响：** 大多数 block 的 root 有 8 个子节点，第一层的子节点就有数万 grandchildren，budget（22728）连第一个子节点都展不开。

**修复方向：** 已改为每 block 用满 500K budget（不再均分），待验证。

---

## 问题 3：flush 输出内部节点导致同心椭球

**文件：** `packages/renderer/src/traverse-block.ts:314-318`

原 flush 输出所有剩余节点（含内部节点）。内部节点和已展开的子节点同时在渲染输出中 → 同心椭球。

**修复：** flush 只输出叶子节点（`childCount === 0`），跳过内部节点。这是正确的 LOD 行为——预算不足时丢弃未展开的分支（场景出现空洞但无重叠）。

---

## 问题 4：`BlockLodRenderer.update()` 在 plugin 不可用时静默返回

**文件：** `packages/renderer/src/block-renderer.ts:65-68`

当 `getSplattingPlugin()` 返回 undefined，update 直接 return，`lastSelectedCount` 保持 0。主页面用 `lastSelectedCount || totalTreeNodes` 作 fallback 显示，但实际渲染是默认全部节点。

**影响：** 页面加载初期、首次 render 完成后 plugin 可能还未初始化，导致前几帧没有 LOD。

---

## 问题 5：`_standardTraverse` 的 heap 全局单例

**文件：** `packages/renderer/src/traverse-block.ts:120`

```typescript
const heap = new MaxHeap();
```

是一个模块级单例。`heap.clear()` 在每次 `_standardTraverse` 开始时调用。但 `_standardTraverse` 在 dynamic mode 的循环中被多次调用，每次调用前都 clear。当前没有并发问题。

---

## 问题 6：`BlockTree.traverse()` 日志频率过高

**文件：** `packages/renderer/src/block-tree.ts:95`

每帧对每个 active block 输出日志。22 blocks × 60fps = 1320 行/秒。

**修复：** 加 throttle。

---

## 问题 7：`traverseBlock` 日志频率过高

**文件：** `packages/renderer/src/traverse-block.ts`

dynamic mode 的每次迭代都输出日志。22 blocks × 5 iters × 60fps = 6600 行/秒。

**修复：** 加 throttle。

---

## 问题 8：opacity clamp 与 RAD 编码不匹配

**文件：** `packages/splat-transform-native/source/src/splat/lod_tree.cpp:509`

```cpp
newOpacity = std::min(newOpacity, 1.0);
```

RAD 编码器用 `max_alpha=2.0`，但 opacity 被 clamp 到 1.0。解码后 alpha_f32 = u8/255 = clamped_opacity/2.0 → 内部节点透明度异常。

**修复：** 去掉 clamp。但这不是同心椭球的原因（透明度不影响位置/大小）。

---

## 问题 9：空 SH 采样器导致 WebGL 纹理上载错误

**文件：** `external/egs-core/packages/loaders/splat-loader/splat/SuperCompressedSplatData.ts:117-132`

当 `shDegree=0` 时，SH 采样器创建为 width=4096, height=1903 但 source.length=0。`createSourceTextureFromSampler` 对空缓冲调用 `texSubImage2D` → WebGL 错误。

**修复：** 已改为 width=1, height=1, source=16 字节（当 shDegree=0 时）。

---

## 问题 10：`bv-lod-test.astro` 的 alpha 补偿可能过度

**文件：** `website/src/pages/[lang]/bv-lod-test.astro:106`

```typescript
scd.setAlpha(gIdx, table[13][i] * 2);
```

`table[13][i]` 是 `decodedBlockToSplatData` 解码后的 alpha（`decoded.rgba[i*4+3] / 255`）。RAD 编码器用 `max_alpha=2.0`，所以解码后的 alpha 范围是 [0, 0.5]（对应原始 [0, 1]）。`*2` 还原到 [0, 1]。这是正确的。

但如果 merge_nodes 不再 clamp opacity（问题 8），内部节点的 opacity 可能 > 1.0。编码时用 max_alpha=2.0，解码后 alpha = real_alpha/2。`*2` 还原到 real_alpha。对于 opacity=1.5 的内部节点，解码后 0.75，`*2` 后 1.5。SuperCompressed 格式的 alpha 是 u8[0,255] 映射到 [0, 1]，所以 1.5 会被 clamp 到 1.0，丢失信息。

**潜在问题：** SuperCompressed 的 shader 是否支持 >1.0 的 alpha？

---

## 汇总

| #   | 严重度 | 文件                     | 问题                                        |
| --- | :----: | ------------------------ | ------------------------------------------- |
| 1   | **高** | block-renderer.ts        | order 数组越界 → WebGL 错误                 |
| 2   |   中   | block-tree.ts            | budget 均分过小（已改待验证）               |
| 3   | **高** | traverse-block.ts        | flush 输出内部节点 → 同心椭球（已修待验证） |
| 4   |   低   | block-renderer.ts        | plugin 不可用时静默返回                     |
| 5   |   无   | traverse-block.ts        | heap 全局单例（无害）                       |
| 6   |   低   | block-tree.ts            | 日志太多                                    |
| 7   |   低   | traverse-block.ts        | 日志太多                                    |
| 8   |   中   | lod_tree.cpp             | opacity clamp 不匹配                        |
| 9   |   中   | SuperCompressedSplatData | 空 SH 采样器（已修）                        |
| 10  |   低   | bv-lod-test.astro        | alpha >1.0 精度损失                         |
