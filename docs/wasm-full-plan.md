# WASM 全流程实施计划

参考 Spark 的 Rust 实现，WASM 内完成全部逻辑。

---

## Rust 依赖

| 依赖          | Source    | 用途                        | Spark 也用 |
| ------------- | --------- | --------------------------- | :--------: |
| `miniz_oxide` | crates.io | deflate 解压（.rad chunks） |     ✅     |
| `half`        | crates.io | f16 ↔ f32 转换              |     ✅     |

**不使用** `serde_json` — .rad header JSON 结构固定，手写解析（更小更快）。

---

## WASM 功能模块

### 模块 1：RAD 解码（参考 rad.rs）

输入：.rad 文件二进制数据（`&[u8]`）
输出：平坦树数据（centers, featureSizes, childStarts, childCounts, blockOffset）

流程：

```
1. 检查 RAD0 magic（0x30444152）
2. 读取 JSON header length → 解析 JSON（手写）：
   - count（总节点数）
   - maxSh（SH degree）
   - lodTree（是否含 LOD 树）
   - chunkSize（chunk 大小，默认 16384）
   - chunks（偏移 + 字节数数组）
3. 对每个 RADC chunk：
   a. 检查 RADC magic（0x43444152）
   b. 读 JSON chunk meta → 解析 properties
   c. 读 payload_size（u64 LE）
   d. 对每个 property：
      - 读 tag（0=raw, 1=raw deflate）
      - 如果 tag=1 → miniz_oxide::inflate::decompress_to_vec
      - 按 property 类型解码
```

### 模块 2：LRU 缓存

```
static BLOCK_CACHE: [BlockCache; 22]

struct BlockCache {
    state: Unloaded | Loaded,
    last_used: u32,
    // 解码后的树数据
    centers: Vec<f32>,        // f32[3] × totalNodes
    feature_sizes: Vec<f32>,  // f32 × totalNodes
    child_starts: Vec<u32>,   // u32 × totalNodes
    child_counts: Vec<u16>,   // u16 × totalNodes
    total_nodes: u32,
    // LRU tracking
    gs_count: u32,             // 叶子 GS 数（用于预算估计）
}
```

### 模块 3：视锥剔除 + 权重

```
输入：相机 pos/forward + viewProj 矩阵
输入：block AABB × 22（常驻 WASM 内存）

对每个 block：
  isInside = frustum.intersectsBox(block.aabb)
  dist = distance(cameraPos, block.center)
  isBehind = dot(dirToBlock, forward) < -0.2 && dist > 2
  weight = 1/(1+0.1*d²) × (isInside ? 1 : 0.4) × (isBehind ? 0.1 : 1)
  budget = maxSplats × weight / totalWeight

返回：load_requests（需要从 JS 获取的 .rad 文件）
```

### 模块 4：BFS 遍历 + Radix Sort

复用现有代码。输出直接是 SuperCompressed 格式的 16 字节/GS 数据。

---

## JS 侧改动

### 初始化

```typescript
// 1. fetch lod-meta.json → 解析 block bounds
const meta = await fetch('/block-data/lod-meta.json').then(r => r.json());
const bounds = meta.tree.map(b => [...b.bound.min, ...b.bound.max]);

// 2. 初始化 WASM（传入 bounds、配置）
wasm.init(bounds, meta.totalTreeNodes);

// 3. 第一帧视锥 → 获取需要加载的 block ID
const requests = wasm.getInitialLoads(camera);
// requests = [8, 9, 10, ...]

// 4. fetch + 解码
for (const id of requests) {
    const rad = await fetch(`/block-data/block_${id}.rad`).then(r => r.arrayBuffer());
    wasm.loadRad(id, new Uint8Array(rad));
}

// 5. 首次遍历 → 渲染
const { data, count } = wasm.traverse(camera);
render(data, count);
```

### 每帧渲染

```typescript
function frame() {
    const result = wasm.traverse(camera); // 返回 GS 数据 + 新 block 请求
    render(result.data, result.count);

    // 处理新 block 加载请求
    for (const id of result.loadRequests) {
        fetch(`/block-data/block_${id}.rad`)
            .then(r => r.arrayBuffer())
            .then(buf => wasm.loadRad(id, new Uint8Array(buf)));
    }
}

function render(data: Uint8Array, count: number) {
    // 数据已经是 SuperCompressed 格式（16 bytes/GS）
    const scd = new SuperCompressedSplatData();
    scd.init(count, 0);
    const buf8 = (scd as any).splatUint8Buffer;
    buf8.set(data);
    const splat = await createSplat(scd);
    sc.add(splat);
    if (oldSplat) sc.remove(oldSplat);
    oldSplat = splat;
}
```

---

## 输出格式

WASM 输出缓冲：`gs_data: Uint8Array`，每 16 字节一个 GS：

```
byte 0-1:   center_x (f16 LE)  ← 来自 RAD 解码
byte 2-3:   center_y (f16 LE)
byte 4-5:   center_z (f16 LE)
byte 6:     scale_x (Ln0R8)     ← 直接透传
byte 7:     scale_y (Ln0R8)
byte 8:     scale_z (Ln0R8)
byte 9:     quat_u (Oct88R8)
byte 10:    quat_v (Oct88R8)
byte 11:    quat_angle (Oct88R8)
byte 12:    red (u8)
byte 13:    green (u8)
byte 14:    blue (u8)
byte 15:    alpha (u8)
```

JS 直接 `buf8.set(gs_data)` → `createSplat` → `sc.add`。

---

## 文件改动清单

| 文件                                         |   动作   | 说明                                         |
| -------------------------------------------- | :------: | -------------------------------------------- |
| `packages/traverse-wasm/Cargo.toml`          |    改    | 增加 `miniz_oxide`, `half` 依赖              |
| `packages/traverse-wasm/src/lib.rs`          | **重写** | RAD 解码 + LRU + 视锥 + BFS + 排序 + GS 输出 |
| `packages/traverse-wasm/src/rad_decoder.rs`  | **新建** | RAD0/RADC 解析、JSON 手写解析、属性解码      |
| `packages/traverse-wasm/src/frustum.rs`      | **新建** | 视锥体 - AABB 相交测试                       |
| `packages/renderer/src/wasm-traverse.ts`     | **重写** | 适配新 WASM 接口                             |
| `packages/renderer/src/block-renderer.ts`    |  **删**  | WASM 全包，不再需要                          |
| `packages/renderer/src/block-tree.ts`        |  **删**  | 同上                                         |
| `packages/renderer/src/block-manager.ts`     |  **删**  | 同上                                         |
| `website/src/pages/[lang]/bv-lod-test.astro` | **重写** | WASM 初始化 + 渲染循环                       |

---

## 实施顺序

1. 新建 `frustum.rs` — 视锥 - AABB 测试
2. 新建 `rad_decoder.rs` — .rad 解码
3. 重写 `lib.rs` — 整合全部模块
4. 重写 `wasm-traverse.ts` — 新接口
5. 重写 `bv-lod-test.astro` — 新渲染循环
6. 删废弃文件
7. 构建 WASM → 测试
