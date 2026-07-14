# WASM 遍历方案

## 动机

当前 JS 遍历 500K 节点需 380ms，瓶颈在：

- JS 堆操作（数组交换、临时对象）
- `Math.sqrt` / `Math.acos` 在 JS 中慢
- 函数调用开销大

Rust → WASM 预计 < 10ms（基于 Spark 的性能）

---

## 新增文件

| 文件                                     | 用途                          |
| ---------------------------------------- | ----------------------------- |
| `packages/traverse-wasm/Cargo.toml`      | Rust 项目配置                 |
| `packages/traverse-wasm/src/lib.rs`      | Rust 遍历实现                 |
| `packages/traverse-wasm/build.ps1`       | Windows 构建脚本              |
| `packages/renderer/src/wasm-traverse.ts` | JS 胶水代码，加载 WASM + 调用 |

无其他文件需要修改。

---

## 接口设计

### Rust 导出函数（lib.rs）

```rust
#[wasm_bindgen]
pub fn traverse(
    // ── Block 数据（flat arrays, 连续排列）──
    centers: &[f32],          // f32[3] × totalNodes（全部 block 拼接）
    feature_sizes: &[f32],    // f32[1] × totalNodes
    child_starts: &[u32],     // u32[1] × totalNodes
    child_counts: &[u16],     // u16[1] × totalNodes
    block_offsets: &[u32],    // u32[1] × numBlocks（每个 block 的节点起始偏移）
    block_node_counts: &[u32],// u32[1] × numBlocks（每个 block 的节点数）

    // ── 相机参数 ──
    cam_x: f32, cam_y: f32, cam_z: f32,
    fwd_x: f32, fwd_y: f32, fwd_z: f32,

    // ── 遍历参数 ──
    lod_scale: f32,
    pixel_scale_limit: f32,
    max_splats: u32,
    budgets: &[u32],          // u32[1] × numBlocks（per-block 预算上限）

    // ── 输出缓冲（JS 预分配）──
    out_indices: &mut [u32],
) -> u32  // 返回选中的 GS 数
```

### JS 侧胶水（wasm-traverse.ts）

```typescript
export class WasmTraverser {
    private wasm: any;
    private initialized = false;

    async init(): Promise<void> {
        const wasm = await import('../../traverse-wasm/pkg/traverse_wasm');
        this.wasm = wasm;
        this.initialized = true;
    }

    traverse(
        trees: TreeData[],
        offsets: number[],
        config: TraverseConfig,
        budgets: Uint32Array,
        output: Uint32Array,
    ): number {
        // 数据已是在 JS 侧分配的 TypedArray
        // 直接传给 WASM（零拷贝）
        return this.wasm.traverse(
            centers,
            featureSizes,
            childStarts,
            childCounts,
            new Uint32Array(offsets),
            new Uint32Array(blockCounts),
            camX,
            camY,
            camZ,
            fwdX,
            fwdY,
            fwdZ,
            lodScale,
            pixelScaleLimit,
            maxSplats,
            budgets,
            output,
        );
    }
}
```

---

## 数据流

```
JS 侧（每帧）:
  BlockTree.traverse() → 已拼接好的 TypedArray 数据
         ↓
  WasmTraverser.traverse() → 传入 WASM
         ↓
WASM 侧:
  1. 解析 block_offsets → 每个 block 的数据范围
  2. 所有 block 的 root 入堆（BinaryHeap<(pixelScale, nodeIdx, blockIdx)>）
  3. BFS 遍历（同现有 JS 算法）
  4. 选中节点写入 out_indices
  5. 返回 count
         ↓
JS 侧:
  out_indices[0..count] → depthSort → setExternalOrder
```

---

## 构建

### 环境要求

- Rust toolchain（`rustup` + `wasm32-unknown-unknown` target）
- `wasm-pack`（`cargo install wasm-pack`）

### 构建命令

```bash
cd packages/traverse-wasm
wasm-pack build --target web
```

输出：`packages/traverse-wasm/pkg/traverse_wasm.js` + `.wasm`

### 集成到 pnpm build

在 `packages/renderer/package.json` 的 `.build` 脚本中增加：

```json
".build": "cd ../traverse-wasm && wasm-pack build --target web && cd ../renderer && pnpm run .egs:types && node ../../scripts/build-package.mjs"
```

---

## 实现细节

### Rust 端核心算法（对应 JS \_standardTraverse）

```rust
pub fn traverse(
    centers: &[f32], feature_sizes: &[f32],
    child_starts: &[u32], child_counts: &[u16],
    block_offsets: &[u32],
    block_node_counts: &[u32],
    cam_x: f32, cam_y: f32, cam_z: f32,
    fwd_x: f32, fwd_y: f32, fwd_z: f32,
    lod_scale: f32, pixel_scale_limit: f32,
    max_splats: u32,
    budgets: &[u32],
    out: &mut [u32],
) -> u32 {
    // 计算 pixel_scale（无 foveation，保持与当前 JS 一致）
    let compute_ps = |cx, cy, cz, fs| -> f32 {
        let dx = cx - cam_x; let dy = cy - cam_y; let dz = cz - cam_z;
        let dist = (dx*dx + dy*dy + dz*dz).sqrt().max(1e-6);
        fs / dist * lod_scale
    };

    // 所有 root 入堆
    let mut heap = BinaryHeap::with_capacity(num_blocks as usize);
    for bi in 0..num_blocks {
        let offset = block_offsets[bi];
        let count = block_node_counts[bi];
        let ri = offset + count - 1; // root index（全局）
        let co = (ri as usize) * 3;
        let ps = compute_ps(centers[co], centers[co+1], centers[co+2],
                            feature_sizes[ri as usize]);
        heap.push(OrderedFloat(ps), ri, bi);
    }

    let mut spent = vec![0u32; budgets.len()];
    let mut out_count = 0;

    while let Some((ps, node_idx, block_idx)) = heap.pop() {
        let bidx = block_idx as usize;
        if spent[bidx] >= budgets[bidx] { continue; }
        spent[bidx] += 1;

        let cnt = child_counts[node_idx as usize] as u32;
        if cnt == 0 {
            out[out_count] = node_idx; out_count += 1;
            if out_count >= max_splats as usize { break; }
            continue;
        }
        if spent[bidx] + cnt > budgets[bidx] {
            out[out_count] = node_idx; out_count += 1;
            if out_count >= max_splats as usize { break; }
            continue;
        }

        let start = child_starts[node_idx as usize];
        for c in 0..cnt {
            let ci = start + c;
            let co = (ci as usize) * 3;
            let ps = compute_ps(centers[co], centers[co+1], centers[co+2],
                                feature_sizes[ci as usize]);
            if ps <= pixel_scale_limit {
                out[out_count] = ci; out_count += 1;
                if out_count >= max_splats as usize { break; }
            } else {
                heap.push(OrderedFloat(ps), ci, block_idx);
            }
        }
    }
    out_count as u32
}
```

### 为什么这个 WASM 会快

| 优化点            | JS                             | Rust/WASM             |
| ----------------- | ------------------------------ | --------------------- |
| 数值计算          | V8 解释执行                    | 原生机器码            |
| 堆                | `[a,b]=[b,a]` 创建临时数组     | `mem::swap` 单指令    |
| computePixelScale | 函数调用 + 属性访问            | 内联闭包              |
| 对象分配          | `{ps, idx, blockIdx}` 每次分配 | 栈上元组              |
| 内存访问          | 边界检查                       | 无边界检查（release） |
| GC                | 每帧 500K 对象 → 暂停          | 无 GC                 |

---

## 风险

| 风险                 | 缓解                             |
| -------------------- | -------------------------------- |
| 需要安装 Rust 工具链 | 一次性的；可以缓存 prebuilt wasm |
| wasm-pack 构建速度   | 增量和缓存                       |
| 和其他 WASM 冲突     | 独立的 wasm 模块                 |
| 浏览器兼容性         | 所有现代浏览器支持 WASM          |
