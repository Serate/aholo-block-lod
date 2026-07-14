# WASM 全流程实现方案

## 目标

JS 侧只收最终数据（已排序的选中 GS 索引），其他全部在 WASM 里完成：遍历 + radix sort。

---

## 改动 1：lib.rs — WASM 内完成遍历 + 排序

### 当前状态

```
WASM BFS: 选中 GS → out[0..count]（无序索引）
JS 收到后: depthSort(out, count) → setExternalOrder
```

### 目标状态

```
WASM BFS: 选中 GS + 存 z 值 → 2-pass radix sort → out[0..count]（有序索引）
JS 收到后: setExternalOrder(out, count)
```

### Rust 代码改动

**a) 启用 fwd 参数计算 z**

当前 `_fwd_x, _fwd_y, _fwd_z` 未使用。改为 `fwd_x, fwd_y, fwd_z`，在 compute_ps 旁计算 z：

```rust
let dx = cx - cam_x; let dy = cy - cam_y; let dz = cz - cam_z;
let dist = (dx*dx + dy*dy + dz*dz).sqrt().max(1e-6);
let pixel_scale = (fs / dist) * lod_scale;
let z = dx * fwd_x + dy * fwd_y + dz * fwd_z;  // 新增
```

**b) 输出时同时存索引和 z 值**

当前 `out[out_count++] = node_idx`。改为双缓冲：

```rust
out[out_count] = node_idx;
z_scratch[out_count] = z;
out_count += 1;
```

**c) BFS 结束后 radix sort**

2-pass radix sort，按 z 降序（远→近）：

```rust
// 1. 计算排序 key
// key = !(f32::to_bits(z) ^ 0x80000000)
//    先翻转符号位（使负数 < 正数），再全部取反（降序）

// 2. Pass 1: 按低 16 位 scatter 到 scratch
for i in 0..count {
    let key = !(z_scratch[i].to_bits() ^ 0x80000000);
    let lo = (key & 0xFFFF) as u32;
    let pos = buckets_lo[lo as usize];
    buckets_lo[lo as usize] += 1;
    scratch[pos as usize] = out[i];
}

// 3. Pass 2: 按高 16 位 scatter 回 out
for i in 0..count {
    let key = !(z_scratch[scratch[i] as usize].to_bits() ^ 0x80000000); // 需要 z 值
    let hi = (key >> 16) as u32;
    let pos = buckets_hi[hi as usize];
    buckets_hi[hi as usize] += 1;
    out[pos as usize] = scratch[i];
}
```

Wait — Pass 2 需要知道每个 scratch 条目的 z 值，但我们在 Pass 1 里把索引写进了 scratch，z 值在 z_scratch 里。所以应该：

```rust
// Pass 1: scatter (out[i], z_scratch[i]) by low 16 bits → scratch_idx, scratch_z
// Pass 2: scatter scratch pairs by high 16 bits → out (sorted)
```

或者更简单：**Pass 1 scatter 索引，Pass 2 用对应的 z 值**（z_scratch 按相同顺序排列）：

```rust
// Pass 1: 按 key 低 16 位 scatter 索引到 scratch
// z_scratch 保持对应的顺序
for i in 0..count {
    let key = key_buf[i];  // 预先计算
    let lo = key & 0xFFFF;
    scratch[buckets_lo[lo] as usize] = out[i];
    z_scratch[buckets_lo[lo] as usize] = z_scratch[i]; // 同步移动
    buckets_lo[lo] += 1;
}

// Pass 2: 按 key 高 16 位 scatter 回 out
// buckets_hi 已计算好前缀和
for i in 0..count {
    let key = key_buf[scratch[i]] // 通过 scratch 值找到原始索引... 不对
}
```

这有点绕。更清晰的实现：

```rust
// 把 (z, idx) 打包成 u64，按高 32 位排序
// 或者直接对 (key, idx) 用 vector 排序
// 但 vector sort 是 O(N log N)，radix 是 O(N)
```

最清晰的 2-pass radix sort：

```rust
// Pass 1: 按低 16 位 scatter (idx, z) → scratch 数组 (idx, z 对)
// Pass 1.5: 恢复 idx→z 的对应关系
// Pass 2: 按高 16 位 scatter → 最终有序数组

// 使用两个 u32 数组并行：idx_buf, z_buf
// 排序过程中同步移动这两个数组

static mut KEY_BUF: Vec<u32> = Vec::new();  // 预计算的 key
static mut IDX_BUF: Vec<u32> = Vec::new();  // 索引
static mut Z_BUF: Vec<f32> = Vec::new();    // z 值（同步移动）
static mut BUCKET_LO: Vec<u32> = Vec::new(); // 65536 个桶
static mut BUCKET_HI: Vec<u32> = Vec::new(); // 65536 个桶
static mut SCRATCH: Vec<u32> = Vec::new();   // 临时索引
static mut SCRATCH_Z: Vec<f32> = Vec::new(); // 临时 z
```

伪代码：

```rust
// 准备 buckets
BUCKET_LO.resize(65536, 0);
BUCKET_HI.resize(65536, 0);
SCRATCH.resize(out_count, 0);
SCRATCH_Z.resize(out_count, 0.0);

// 计算 keys + 统计
for i in 0..out_count {
    let bits = Z_BUF[i].to_bits() ^ 0x8000_0000; // 翻转符号位
    let key = !bits; // 降序
    KEY_BUF[i] = key;
    BUCKET_LO[(key & 0xFFFF) as usize] += 1;
    BUCKET_HI[(key >> 16) as usize] += 1;
}

// 前缀和 → 起始偏移
let mut sum = 0;
for b in BUCKET_LO.iter_mut() { let cnt = *b; *b = sum; sum += cnt; }
sum = 0;
for b in BUCKET_HI.iter_mut() { let cnt = *b; *b = sum; sum += cnt; }

// Pass 1: 低 16 位 scatter
for i in 0..out_count {
    let lo = (KEY_BUF[i] & 0xFFFF) as usize;
    let pos = BUCKET_LO[lo] as usize;
    BUCKET_LO[lo] += 1;
    SCRATCH[pos] = IDX_BUF[i];
    SCRATCH_Z[pos] = Z_BUF[i];
}

// Pass 2: 高 16 位 scatter
for i in 0..out_count {
    let bits = SCRATCH_Z[i].to_bits() ^ 0x8000_0000;
    let key = !bits;
    let hi = (key >> 16) as usize;
    let pos = BUCKET_HI[hi] as usize;
    BUCKET_HI[hi] += 1;
    IDX_BUF[pos] = SCRATCH[i]; // 最终有序
}

// 复制回 out
for i in 0..out_count { out[i] = IDX_BUF[i]; }
```

**d) 静态缓冲区**

所有缓冲区（KEY_BUF、IDX_BUF、Z_BUF、BUCKET_LO、BUCKET_HI、SCRATCH、SCRATCH_Z）都用 `static mut Vec`，预分配足够容量，避免每帧分配。

### 对函数签名的影响

函数签名**不变**。输出参数 `out` 返回已排序的索引（与原接口一致）。

---

## 改动 2：block-tree.ts — 去掉 depthSort

### 当前 WASM 路径（约 100-120 行）

```
const outBuf = new Uint32Array(config.maxSplats);
const count = this._wasm.traverse({...}, outBuf);
const fullCenters = this._buildFullCenters();
const sorted = depthSort(outBuf, count, fullCenters, ...);
return { order: sorted, count };
```

改为：

```
const outBuf = new Uint32Array(config.maxSplats);
const count = this._wasm.traverse({...}, outBuf);
return { order: outBuf, count: outBuf.subarray(0, count) };
```

不需要 `_buildFullCenters()`，不需要 `depthSort`。

### JS fallback 路径

保持现有 `depthSort` 不变（保留兜底，但仍可移除）。

---

## 改动文件清单

| 文件                                  | 改动量                                             |
| ------------------------------------- | -------------------------------------------------- |
| `packages/traverse-wasm/src/lib.rs`   | +80 行（radix sort 实现 + z 跟踪）                 |
| `packages/renderer/src/block-tree.ts` | -5 行（去掉 WASM 路径的 depthSort + centers 构造） |

无其他文件改动。

---

## 性能预期

| 步骤            |    当前     |    改后     |
| --------------- | :---------: | :---------: |
| WASM BFS        |   5-15ms    |   5-15ms    |
| JS depthSort    |    0.5ms    |     0ms     |
| WASM radix sort |     0ms     |   0.05ms    |
| centers 构造    | 0ms（缓存） | 0ms（去掉） |
| 合计            |   5-15ms    |   5-15ms    |

性能不变，但**顺序正确**（WASM radix sort 比 JS `dot()` CPU sort 精确），且**架构干净**。
