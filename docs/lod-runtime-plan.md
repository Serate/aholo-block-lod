# RAD Block Viewer: Runtime LOD Enhancement Plan

## Goals

1. 改造 `traverse-block.ts`，匹配 Spark `traverse_lod_trees` 的 foveated LOD 遍历逻辑
2. 更新 `block-viewer.astro` 使用 RAD + BlockLodRenderer 管线
3. 支持 standard / dynamic 两种遍历模式
4. 支持注视点衰减（foveated rendering）
5. 正确实现 LOD 遍历 + 深度排序两阶段管线

---

## 核心管线设计

```
每帧流程:

camera + budget
      │
      ▼
┌─────────────────┐
│  BlockManager    │  ← 按距离筛选 active blocks（使用现有 BlockManager，不做额外 prefetch）
│  .update(cam)    │
└────────┬────────┘
         │ activeBlockIds
         ▼
┌─────────────────┐
│  BlockTree       │  ← 对每个 active block 做 LOD 遍历
│  .traverse()     │     standard: 硬阈值
│                  │     dynamic: 自动收紧直到命中预算
└────────┬────────┘
         │ { indices, count }
         ▼
┌─────────────────┐
│  depthSort()     │  ← CPU 深度排序（必须！setExternalOrder 绕过内部排序）
│                  │     用 dot(center, cameraForward) 计算 Z
│                  │     先用 TypedArray.sort，后续如需优化可改用 radix sort
└────────┬────────┘
         │ { sortedOrder, count }
         ▼
┌─────────────────────────┐
│  SplattingPlugin        │
│  .setExternalOrder()    │  ← 喂给渲染管线
└─────────────────────────┘
```

### 为什么需要 depthSort 步骤

`setExternalOrder` **完全绕过 SplattingPlugin 的深度排序管线**（源码注释原文：`bypassing the sort pipeline`），传入的 order 直接决定渲染顺序。而 LOD 遍历输出的是 BFS 粗细级顺序（根 → 子节点），不是由远及近的深度顺序。

不排序直接喂给 `setExternalOrder` → 半透明混合错乱。

### 与 Spark 的对比

| 维度       | Spark                  | 本方案（第一版）                    |
| ---------- | ---------------------- | ----------------------------------- |
| depth 来源 | GPU 光栅化深度回读     | CPU 算 `dot(center, cameraForward)` |
| 排序算法   | WASM radix sort (O(N)) | JS TypedArray.sort (O(N log N))     |
| 异步？     | 是（1-2 帧回读延迟）   | 同步（当前帧）                      |
| 精度       | 精确到 sub-pixel       | 中心 Z ≈ 足够（GS 是 billboard）    |

GS 是始终面向相机的 billboard，中心 Z 排序和 GPU 光栅化深度差异极小，CPU sort 够用。

---

## 数据合并方案

**结论：方案 C — 合并所有 .rad 数据为一个 SplatData + 一次性全加载**

```
.rad × 22 → decodeRad → SuperCompressedSplatData
                                ↓
                         createSplat(combined) → sc.add(splat)  // 所有节点常驻 GPU
                                ↓
                         BlockTree.addBlock(decoded, nodeOffset) × 22
```

**为什么不做 block LRU：**

- 438 万 GS / 7.8M tree nodes 约 172 MB 显存，现代 GPU 可承受
- 后续如需支持更大场景，再引入 per-block 独立 Splat + 动态加载

**为什么不做 traversal 驱动 prefetch（touchedBlocks）：**

- 全量加载，不需要预取
- 后续如果需要，可复用 Spark 的 `chunk_max: HashMap<chunkId, maxPixelScale>` 方案

---

## 调试原则

每个 Task 分三步验证：

1. **类型检查**：`pnpm check:renderer` 通过
2. **单元测试**：Node.js 运行独立测试函数，mock 输入验证输出
3. **集成验证**：block-viewer 页面加载后观察效果

辅助手段：

- `console.log` / `performance.now()` 计时
- Chrome DevTools → Performance 面板分析帧耗时
- 页面右下角 GS 计数实时反映 LOD 选中的数量

---

## Task A: 增强 computePixelScale — 加 Cone/Fovea 衰减

**文件：** `packages/renderer/src/traverse-block.ts`
**依赖：** 纯函数，可独立编写并运行

### 改动内容

重写 `computePixelScale`：

```
pixelScale = (featureSize / dist) * lodScale * coneFactor * behindFactor

coneFactor:
  - 角度 < coneFov0（中心凹）:              1.0
  - 角度 coneFov0 ~ coneFov（过渡区）:     1.0 → behindFoveate 线性衰减
  - 角度 > coneFov（外围）:                  behindFoveate

behindFactor:
  - 相机前方（dot > 0）:  1.0
  - 相机后方（dot < 0）:  behindFoveate（默认 0.1）
```

### 新增导出

```typescript
export interface FoveatedConfig {
    coneFov0: number; // 中心凹半角（度），默认 5
    coneFov: number; // 外围半角（度），默认 30
    behindFoveate: number; // 后方衰减系数，默认 0.1
}

export function computePixelScale(
    cx: number,
    cy: number,
    cz: number,
    featureSize: number,
    camX: number,
    camY: number,
    camZ: number,
    forwardX: number,
    forwardY: number,
    forwardZ: number,
    lodScale: number,
    foveated?: Partial<FoveatedConfig>,
): number;
```

### 调试方案

**Step 1 — 类型检查：**

```bash
pnpm --filter @manycore/aholo-viewer run .build
# 确保 tsc --emitDeclarationOnly 通过
```

**Step 2 — Node.js 直接运行测试函数：**
在 `traverse-block.ts` 末尾添加临时测试函数：

```bash
node -e "
// 复制 computePixelScale 函数到临时文件，或用 tsx 直接执行
// 测试用例：
const assert = require('assert');

// 同距离不同角度
const fwd = computePixelScale(0,0,1, 1, 0,0,0, 0,0,1, 1);  // 正前方
const side = computePixelScale(1,0,0, 1, 0,0,0, 0,0,1, 1); // 侧方
const behind = computePixelScale(0,0,-1, 1, 0,0,0, 0,0,1, 1); // 后方
console.log('fwd:', fwd, 'side:', side, 'behind:', behind);
assert(fwd > side, '前方应大于侧方');
assert(side > behind, '侧方应大于后方');

// cone 衰减
const fwdCenter = computePixelScale(0,0,10, 1, 0,0,0, 0,0,1, 1, {coneFov0:5, coneFov:30, behindFoveate:0.1});
const periph = computePixelScale(5,0,8.66, 1, 0,0,0, 0,0,1, 1, {coneFov0:5, coneFov:30, behindFoveate:0.1});
console.log('center:', fwdCenter, 'periphery:', periph);
assert(fwdCenter >= periph, '中心凹应 >= 外围');

console.log('All tests passed');
"
```

**Step 3 — 确认旧参数兼容：**
不传 `foveated` 参数时，行为与旧版本一致（coneFactor=1, behindFactor=1）。

---

## Task B: 抽出 prepareTreeData + 新增 depthSort

**文件：** `packages/renderer/src/traverse-block.ts`
**依赖：** 纯函数，可独立编写并运行

### 改动内容

#### 1. prepareTreeData（从 BlockTree 移入）

```typescript
export function prepareTreeData(decoded: DecodedBlock): TreeData;
```

将 f16→f32 center 转换 + featureSize 计算封装为独立函数。

#### 2. depthSort

```typescript
export function depthSort(
    indices: Uint32Array,
    count: number,
    centers: Float32Array, // f32[3] × totalNodes（全局合并后的）
    camX: number,
    camY: number,
    camZ: number,
    forwardX: number,
    forwardY: number,
    forwardZ: number,
): Uint32Array;
```

用 `camera-space Z = dot(center - cameraPos, cameraForward)` 排序。

实现方式：

```
1. const pairs = new Array(count);
2. for i in count: pairs[i] = { idx: indices[i], z: dot(centers[idx*3..], camForward) }
3. pairs.sort((a, b) => b.z - a.z)  // 降序（远→近）
4. 提取 sorted indices 到新 Uint32Array
```

### 调试方案

**Step 1 — 类型检查：** Task A 相同

**Step 2 — Mock BlockData 测试 prepareTreeData：**

```bash
node -e "
// 构造最小 DecodedBlock
const mockDecoded = {
  totalNodes: 3,
  center: new Uint16Array([ // f16 values
    0x0000, 0x0000, 0x0000,  // node0: (0,0,0)
    0x3C00, 0x0000, 0x0000,  // node1: (1,0,0)
    0x0000, 0x3C00, 0x0000,  // node2: (0,1,0)
  ]),
  scale: new Uint8Array([...]),   // Ln0R8
  childStart: new Uint32Array([2, 0, 0]),
  childCount: new Uint16Array([2, 0, 0]),
  ...
};

const treeData = prepareTreeData(mockDecoded);
// 验证 centers 被正确转换为 f32
// 验证 featureSizes 全部 > 0
// 验证 childStart/childCount 不变
console.log('prepareTreeData OK');
"
```

**Step 3 — 测试 depthSort：**

```bash
node -e "
const centers = new Float32Array([0,0,0, 1,0,0, 0,1,0]); // 3 nodes
const indices = new Uint32Array([0, 1, 2]); // 选中全部
const cam = [0, 0, 10];
const fwd = [0, 0, -1]; // 看向 -Z

const sorted = depthSort(indices, 3, centers, ...cam, ...fwd);
// node0 距离 10 → Z = dot(0-0,0-0,0-10, 0,0,-1) = 10
// node1 距离 sqrt(101) ≈ 10.05 → Z ≈ 10.05
// node2 距离 sqrt(101) ≈ 10.05 → Z ≈ 10.05
// 预期排序: node1, node2, node0 (远→近)
console.log('sorted:', Array.from(sorted));
// 验证返回的 count 正确
console.log('depthSort OK');
"
```

**Step 4 — 性能测试：**

```bash
node -e "
const N = 500000;
const centers = new Float32Array(N * 3);
const indices = new Uint32Array(N);
for (let i = 0; i < N; i++) {
  indices[i] = i;
  centers[i*3] = Math.random() * 100 - 50;
  centers[i*3+1] = Math.random() * 100 - 50;
  centers[i*3+2] = Math.random() * 100 - 50;
}
const t0 = performance.now();
const sorted = depthSort(indices, N, centers, 0,0,0, 0,0,-1);
console.log('500K sort:', (performance.now() - t0).toFixed(1), 'ms');
console.assert(sorted.length === N, 'count check');
"
```

---

## Task C: 重写 traverseBlock — 支持 Standard/Dynamic 双模式

**文件：** `packages/renderer/src/traverse-block.ts`
**依赖：** Task A, B

### 改动内容

```typescript
export interface TraverseConfig {
    cameraPos: [number, number, number];
    cameraForward: [number, number, number]; // normalized
    lodScale: number;
    pixelScaleLimit: number;
    maxSplats: number;
    foveated?: Partial<FoveatedConfig>;
    dynamicMode?: boolean;
}

export function traverseBlock(tree: TreeData, config: TraverseConfig, outputIndices: Uint32Array): number; // returns output count
```

**standard 模式：**

1. root 入 MaxHeap（**注意：root = totalNodes - 1**）
2. 弹出最大 pixelScale 节点
3. 若 pixelScale ≤ pixelScaleLimit → 直接输出到 outputIndices
4. 否则展开子节点（用增强 computePixelScale 评估每个子节点）
5. 超阈值子节点入堆，低于阈值直接输出
6. 输出数 ≥ maxSplats 时停止

**dynamic 模式：**

```
currentLimit = lastFrameLimit || pixelScaleLimit * 100

for iter in 0..5:
    traverse(currentLimit) → count
    if count == 0: break
    ratio = count / maxSplats
    if 0.95 <= ratio <= 1.0: break
    currentLimit *= (ratio ^ 0.5)
    clamp(currentLimit, pixelScaleLimit, INF)

lastFrameLimit = currentLimit
```

### 调试方案

**Step 1 — 构造最小 Mock TreeData 测试根节点修复：**

```bash
node -e "
// 构造极小树：3 层，7 节点（4 leaf + 2 internal + 1 root）
// Root → P1, P2 → A, B, C, D
// 布局：[P1, P2, A, B, C, D, R] 根在 index 6
const tree = {
  totalNodes: 7,
  childStart: new Uint32Array([2, 4, 0, 0, 0, 0, 0]),
  childCount: new Uint16Array([2, 2, 0, 0, 0, 0, 2]),
  centers: new Float32Array([
    0,0,0,  // P1
    1,0,0,  // P2
    -1,0,0, // A
    1,0,0,  // B
    0,1,0,  // C
    0,2,0,  // D
    0,0,0,  // R ← 根
  ]),
  featureSizes: new Float32Array([0.5, 0.5, 0.1, 0.1, 0.1, 0.1, 1.0]),
};

const output = new Uint32Array(100);
const count = traverseBlock(tree, {
  cameraPos: [0,0,10],
  cameraForward: [0,0,-1],
  lodScale: 1,
  pixelScaleLimit: 0.01,
  maxSplats: 10,
}, output);

console.log('count:', count, 'indices:', Array.from(output.subarray(0, count)));
// 应包含 A, B, C, D（叶子节点），不包含 P1, P2, R（内部节点 pixelScale 过大→展开）
// 验证所有 index < totalNodes
"
```

**Step 2 — 验证 Standard 模式 budget 约束：**

```bash
node -e "
// 用同样的树，maxSplats=2
const count = traverseBlock(tree, { ..., maxSplats: 2 }, output);
console.assert(count <= 2, 'budget exceeded');
"
```

**Step 3 — 验证 Dynamic 模式收敛：**

```bash
node -e "
// 使用较大树（通过 process-block-splat 的 .rad 解码得到）
// 或构造 1000+ 节点树
const config = {
  cameraPos: [0,0,10],
  cameraForward: [0,0,-1],
  lodScale: 1,
  pixelScaleLimit: 0.001,
  maxSplats: 500,
  dynamicMode: true,
};
const count = traverseBlock(largeTree, config, output);
// 输出数应在 [475, 500] 范围内 (95%~100%)
console.log('dynamic count:', count);
console.assert(count >= 475 && count <= 500, 'budget not met');
"
```

**Step 4 — 验证 foveated 效果：**

```bash
# 前方 vs 侧方，同一棵树，同一 maxSplats
# 前方：选中更多前方节点
# 侧方：选中更均衡
# 预期前方 count > 侧方 count（前方像素精度更高）
```

**Step 5 — 集成测试（block-viewer 加载后）：**

```typescript
// 在 DevTools Console 中:
const plugin = window.__INTERNAL__.getSplattingPlugin();
// 观察每帧 plugin.splattingMaterial.count 变化
// 旋转相机时 count 应动态变化
```

---

## Task D: 更新 BlockTree — 新 traverse + 节点级 LOD

**文件：** `packages/renderer/src/block-tree.ts`
**依赖：** Task C

### 改动内容

```typescript
export class BlockTree {
    private treeDataCache: (TreeData | null)[] = [];
    private nodeOffsets: number[] = [];
    readonly globalCenters: Float32Array;
    totalNodes: number = 0;

    addBlock(data: DecodedBlock, nodeOffset: number): void {
        // 用 prepareTreeData 转换 data → TreeData
        // 记录 nodeOffset
        // centers 追加到 globalCenters（后续被 depthSort 使用）
    }

    traverse(activeBlockIds: number[], config: TraverseConfig): { order: Uint32Array; count: number } {
        // 对每个 active block:
        //   temp = new Uint32Array(config.maxSplats)
        //   count += traverseBlock(treeData, config, temp)
        //   选中索引 + nodeOffset → 全局索引
        // depthSort(所有选中, globalCenters, cameraPos, cameraForward)
        // return { order, count }
    }
}
```

### 调试方案

**Step 1 — Mock 2 blocks 测试偏移正确性：**

```bash
node -e "
const bt = new BlockTree();

// Block 0: 3 nodes
bt.addBlock(mockDecoded0, 0);
console.assert(bt.totalNodes === 3, 'block0 nodeCount');

// Block 1: 4 nodes, offset=3
bt.addBlock(mockDecoded1, 3);
console.assert(bt.totalNodes === 7, 'total nodeCount');
console.assert(bt.nodeOffsets[1] === 3, 'offset');
console.assert(bt.globalCenters.length === 7*3, 'centers size');

// traverse: 激活 block 0
const result = bt.traverse([0], {
  cameraPos: [0,0,10],
  cameraForward: [0,0,-1],
  lodScale: 1, pixelScaleLimit: 0.01, maxSplats: 100
});
// 所有 index 应在 [0, 3) 范围内（全局索引）
for (const idx of result.order.subarray(0, result.count)) {
  console.assert(idx >= 0 && idx < 3, 'index out of block0 range');
}

// 激活 block 0+1
const result2 = bt.traverse([0, 1], { ... });
// index 应在 [0, 7) 范围内，且 block 1 的 index >= 3
"
```

**Step 2 — 测试 depthSort 顺序：**

```bash
node -e "
const bt = new BlockTree();
// 添加 2 个 block
bt.addBlock(...); bt.addBlock(...);

const result = bt.traverse([0, 1], {
  cameraPos: [0, 0, 10],
  cameraForward: [0, 0, -1],
  ...
});
// 验证 result.order 按 Z 降序排列
for (let i = 1; i < result.count; i++) {
  const zPrev = dot(...cameraForward, centers[prevIdx*3..] - cameraPos);
  const zCurr = dot(...);
  console.assert(zPrev >= zCurr, 'Z not descending at', i);
}
"
```

**Step 3 — 集成测试（block-viewer 加载后）：**

```typescript
// DevTools Console:
// 在帧循环中加 log
console.log('LOD selected:', blockTree.totalNodes, 'total nodes');
console.log('Active blocks:', blockManager.blocks.length);
```

---

## Task E: 更新 BlockLodRenderer — 传递相机 + 调度 depthSort

**文件：** `packages/renderer/src/block-renderer.ts`
**依赖：** Task D

### 改动内容

```typescript
update(
  camera: PerspectiveCamera,
  splattingPlugin?: SplattingPlugin,
  lodScale = 1.0,
  pixelScaleLimit = 0.001,
  maxSplats = 500000,
  foveated?: Partial<FoveatedConfig>,
  dynamicMode = true,
): void;
```

### 调试方案

**Step 1 — Mock 组件验证流程：**

```bash
node -e "
// 无法在 Node 中测试（依赖 DOM/WebGL），用类型检查 + 逻辑验证
// 关注点：
// 1. camera.position 是否正确提取
// 2. camera.getWorldDirection() 是否为 normalized
// 3. blockManager.update 返回值是否正确传入 traverse
// 4. setExternalOrder 的调用参数

// 检查 BlockLodRenderer.update 不再需要 lodScale 作为第 3 个参数
// 新签名: update(camera, plugin, lodScale?, pixelScaleLimit?, maxSplats?, foveated?, dynamicMode?)
"
```

**Step 2 — 类型检查：**

```bash
pnpm --filter @manycore/aholo-viewer run .build
# 确保 tsc 通过
```

**Step 3 — 在 block-viewer 中加临时 log 验证每帧流程：**

```typescript
// block-viewer.astro:
blockRenderer.update(camera, plugin, 1, 0.001, 500000, undefined, true);
console.log('Frame: active blocks', blockManager.blocks.length);
console.log('Frame: selected GS', plugin.splattingMaterial.count);
```

**Step 4 — 验证 setExternalOrder 调用次数：**

```typescript
// 在 SplattingPlugin 上 patch
const orig = plugin.setExternalOrder;
plugin.setExternalOrder = (order, count) => {
    console.assert(count <= 500000, 'budget');
    console.assert(count >= 0, 'non-negative');
    return orig.call(plugin, order, count);
};
```

---

## Task F: 同步更新 C++ traverse_block（可选）

**文件：** `packages/splat-transform-native/source/src/splat/lod_tree.cpp`
**依赖：** Task A（算法一致）

### 改动内容

更新 C++ `compute_pixel_scale` 加 cone/fovea 衰减，与 TS 端逻辑一致。

### 调试方案

**Step 1 — 编译：**

```bash
cd packages/splat-transform-native/source
node build.js build --preset x64-windows-static --target win32-x64-msvc
```

**Step 2 — 与 TS 版对比输出：**

```bash
# 用同一组输入分别调用 C++ 和 TS 的 computePixelScale
# 对比输出结果（允许 ~1e-6 浮点误差）
node -e "
// 从 Node 加载 .node
const native = require('./packages/splat-transform-native/.../splat-transform.node');
// ... 导出 compute_pixel_scale 测试函数
// 对比
"
```

**Step 3 — 集成验证：** 重建后运行 process-block-splat.mjs，确认输出与之前一致（精度允许范围内）

---

## Task G: 更新 lod-meta.json 生成

**文件：** `scripts/process-block-splat.mjs`
**依赖：** 无，可随时做

### 改动内容

在 `lod-meta.json` 增加字段：

```json
{
  "radFiles": ["block_0.rad", "block_1.rad", ...],
  "treeNodes": [327473, ...]
}
```

### 调试方案

```bash
# 重新生成数据
node scripts/process-block-splat.mjs "D:/download/garden-7k.splat" website/public/block-data

# 验证 lod-meta.json 内容
node -e "
const meta = require('./website/public/block-data/lod-meta.json');
console.assert(meta.radFiles, 'missing radFiles');
console.assert(meta.radFiles.length === meta.tree.length, 'count mismatch');
console.assert(meta.treeNodes, 'missing treeNodes');
console.assert(meta.treeNodes.every(n => n > 0), 'invalid treeNodeCount');
console.log('OK');
"
```

---

## Task H: 重写 block-viewer.astro — 接入 RAD + BlockLodRenderer

**文件：** `website/src/pages/[lang]/block-viewer.astro`
**依赖：** Task A~G

### 改动内容

完整替换加载和渲染管线：

```
1. fetch lod-meta.json
2. fetch + decodeRad() × N
3. 合并 N 个 decoded 的 node 数据为一个 SuperCompressedSplatData
   - 初始化 SuperCompressedSplatData.init(totalNodes, shDegree)
   - 对每个 block 的每个节点：setCenter/setScale/setQuat/setColor/setAlpha
   - 记录每个 block 的 nodeOffset
4. createSplat(combined) → sc.add(splat)   // 所有节点常驻 GPU
5. blockTree.addBlock(decoded, nodeOffset) × N
6. blockRenderer = new BlockLodRenderer()
7. 每帧:
   - 在 v.render() 之前调用 blockRenderer.update(camera, plugin, params)
   - 更新 GS 计数显示（从 plugin.splattingMaterial.count 读取）
```

### 调试方案

**Step 1 — 加载阶段验证（DevTools Console）：**

```javascript
// 确认 .rad 文件被正确请求和解析
// Network 面板: block_0.rad ~ block_21.rad 全部 200 OK
// Console: 检查 decodeRad 输出
const testRad = await fetch('/block-data/block_0.rad').then(r => r.arrayBuffer());
const decoded = await decodeRad(new Uint8Array(testRad));
console.log('Block 0:', decoded.totalNodes, 'nodes');
console.log('  childStart:', decoded.childStart.slice(0, 5));
console.log('  childCount:', decoded.childCount.slice(0, 5));
```

**Step 2 — SuperCompressedSplatData 构造验证：**

```javascript
// 合并数据后，检查第一个节点的值
const scd = new SuperCompressedSplatData();
scd.init(totalNodes, 0);
scd.getCenter(0, {}); // 检查 x, y, z 是否合理
scd.getScale(0, {});
scd.getQuat(0, {});
scd.getColor(0, {});
scd.getAlpha(0, {});
```

**Step 3 — BlockLodRenderer 集成验证：**

```javascript
// 在帧循环中加入调试输出
function frame(time) {
    // ... 现有代码 ...
    blockRenderer.update(camera, plugin);
    console.log('activeBlocks:', blockManager.blocks.length);
    console.log('selectedGS:', plugin.splattingMaterial.count);
    // ... 继续渲染 ...
}
```

**Step 4 — LOD 效果验证：**

```
测试场景                      预期结果
──────────────────────────────────────────────────
页面加载完成                  右下角 GS ≈ maxSplats (如 500K)
旋转相机到空旷方向             GS 数减少（后方/外围 GS 被剔除）
旋转相机到密集方向             GS 数增加回到 ≈ maxSplats
缩小（zoom out）              更多 GS 被 LOD 合并 → 显示数 ≈ maxSplats
放大（zoom in）               更少 GS 被选中 → 可能 < maxSplats
关闭 LOD（pixelScaleLimit=0） 所有节点被选中 → GS = totalNodes (7.8M)
设置 maxSplats=100000         GS 数 ≈ 100K
vs 原始全量渲染               FPS 明显提升
```

**Step 5 — 性能分析：**

```javascript
// 用 performance.mark 测量各阶段耗时
performance.mark('traverse-start');
// ... traverse ...
performance.mark('traverse-end');
performance.measure('traverse', 'traverse-start', 'traverse-end');

performance.mark('depthSort-start');
// ... sort ...
performance.mark('depthSort-end');
performance.measure('depthSort', 'depthSort-start', 'depthSort-end');

// Chrome DevTools → Performance: 查看各 measure 耗时
```

**Step 6 — 回归验证：**

```javascript
// GS 计数显示：右下角数字应为正数，≤ maxSplats
// FPS：不应低于原始全量渲染的 50%
// 渲染正确性：GS 位置不发生漂移（检查 center 编码/解码正确）
// 透明度：不因 Alpha max_alpha 问题导致 GS 过暗（如需 α×2 补偿）
```

---

## 实施顺序与依赖

```
Task G (lod-meta.json)    ← 独立，随时可做
     │
Task A (computePixelScale) ← 纯函数，单测
     │
Task B (prepare+depthSort) ← 纯函数，单测
     │
Task C (traverseBlock)     ← 依赖 A, B
     │
Task D (BlockTree 新接口)   ← 依赖 C
     │
Task E (BlockLodRenderer)  ← 依赖 D
     │
Task F (C++ 同步)          ← 优先级最低，可选
     │
Task H (block-viewer)      ← 依赖 A~E, G
```

---

## 性能预算

| 步骤                        | 估算耗时            | 说明                |
| --------------------------- | ------------------- | ------------------- |
| decodeRad × 22              | ~200ms（一次性）    | 加载阶段            |
| 合并 SplatData              | ~50-100ms（一次性） | 7.8M 次 setter 调用 |
| BlockTree.addBlock × 22     | ~30ms（一次性）     | f16→f32 转换        |
| traverseBlock (standard)    | ~2-5ms/帧           | 节点访问            |
| traverseBlock (dynamic × 5) | ~10-25ms/帧         | 最多 5 次迭代       |
| depthSort (500K)            | ~5-10ms/帧          | TypedArray.sort     |
| 合计每帧 (dynamic)          | ~15-35ms            | 目标 < 16ms (60fps) |

如果 dynamic 5 次迭代超时，可降到 3 次或改用 standard。

---

## 兼容性审查结果

审查范围：`build_lod_tree(C++) → encodeRad → decodeRad → BlockTree → traverseBlock → depthSort → setExternalOrder`

### Bug 1（严重）：traverseBlock 根节点索引错误

**`traverse-block.ts:153` 和 C++ `lod_tree.cpp:65` 都从 index 0 开始遍历：**

```typescript
// ❌ 错误：index 0 不是根节点
const rootPs = computePixelScale(centers[0], centers[1], centers[2], featureSizes[0], ...);
heap.push({ pixelScale: rootPs, nodeIndex: 0 });
```

但 `build_lod_tree` 的 level-morton permutation 把根节点放在**数组末尾**：

```
permuteOrder = [child_of_root_1, child_of_root_2, ..., child_of_root_n, grandchild..., ROOT]
                                                                              ↑  totalNodes - 1
```

**后果：** 从 index 0 开始只遍历根的第一个子节点，其他子树永远不会被访问。

**修复（Task C）：** `traverseBlock` 使用 `totalNodes - 1` 作为根节点索引。

### 问题 2：childStart 跨 block 偏移

合并多个 block 时，block 内 `childStart[i]` 是 block-local 索引。当把所有 block 的节点数组拼接后，每个 block 的 `childStart` 需要加上自己的 `nodeOffset`。

**处理方式（Task D）：** 在 `BlockTree.addBlock(data, nodeOffset)` 中，对 `childStart` 做一次性地偏移。

### 问题 3：Budget 分配

当前 `BlockTree.traverse` 每个 block 独立用 `maxSplats` 作为预算。22 块 × 500K = 11M GS → 失去 LOD 意义。

**处理方式（Task C/D）：** Dynamic 模式全局控制。每个 block 不设独立上限，由总输出数决定是否继续。

### 问题 4：Alpha 编码精度损失（LOD opacity）

`merge_nodes` 中 opacity 被 clamp 到 1.0，但 RAD 编码器对 LOD 树使用 `max_alpha = 2.0`：

```
编码: u8 = clamp(opacity / 2.0, 0, 1) × 255 → opacity=1.0 → u8=127
解码: f32 = u8 / 255 = 0.498                   → 损失 ~50%
```

**修复（Task H）：** block-viewer 中 `setAlpha(i, alpha * 2)` 补偿。

### 问题 5：createSplat 不支持 RawSplatData

必须用 `SuperCompressedSplatData`，逐个调用 `setCenter/setScale/setQuat/setColor/setAlpha`。

**处理方式（Task H）：** 合并 decoded 数据 → 初始化 SuperCompressedSplatData → 循环设置每个节点。

### 兼容性总表

| 阶段                                           | 状态 | 说明                                                                      |
| ---------------------------------------------- | :--: | ------------------------------------------------------------------------- |
| `build_lod_tree` → `LodTreeResult`             |  ✅  | C++ 输出 f32 数组 + u32/u16 树结构                                        |
| `encodeRad` 编码                               |  ✅  | F32LeBytes(center)、Ln0R8(scale)、Oct88R8(quat)、R8(rgba)、U32/U16(child) |
| `.rad` 二进制格式                              |  ✅  | RAD0/RADC 魔数、JSON 元数据、deflate 压缩                                 |
| `decodeRad` 解码                               |  ✅  | 所有属性正确解析                                                          |
| Alpha 精度                                     |  ⚠️  | `max_alpha=2.0` 导致 opacity 减半                                         |
| `RadDecodeResult` → `SuperCompressedSplatData` |  ⚠️  | 需要逐 GS 设置                                                            |
| `BlockTree.addBlock()` 偏移                    |  ⚠️  | 需要 nodeOffset                                                           |
| `traverseBlock` 根节点                         |  ❌  | 应为 `totalNodes - 1`                                                     |
| `depthSort`                                    |  ✅  | 独立于树结构                                                              |
| `setExternalOrder`                             |  ✅  | 接受 Uint32Array + count                                                  |
