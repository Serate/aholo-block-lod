# LOD 可视化方式对比：Spark vs 我的实现 vs aholo-viewer-master

## 一、Spark 的正确方式

```
lodIndices 纹理 (独立于 GS 数据)
     │
     ▼
生成器索引 i (0..numSplats-1)  ──→ maybeLookupIndex ──→ 实际 GS 索引
                                    │
                                    ▼
                              lodSplats 专用数据源 (小数据集)
                                    │
                                    ▼
                              排序 → 绘制

numSplats = LOD 选中数量 (≠ 总 GS 数)
数据源 = 专用 LOD splat (非完整数据集)
```

### 关键设计决策

| 决策       | Spark 的选择                  | 效果                           |
| ---------- | ----------------------------- | ------------------------------ |
| 渲染数据集 | 独立 LOD 专用数据             | 纹理小、加载快、内部节点可渲染 |
| 实例数     | `numSplats = lodResult.count` | 着色器只处理选中节点           |
| 索引映射   | `lodIndices` 纹理间接层       | 独立于 GS 数据，可灵活重映射   |
| 父子共存   | `replace-on-expand`           | 遍历结果中不可能父子同时存在   |

---

## 二、我的错误实现

```
setExternalOrder(order, count)
     │
     ▼
GS 数据纹理 (7.8M 全量节点, 125MB)
     │
     ▼
绘制 count 个实例 (但纹理里还有 7.8M-500K 个未使用的节点)

numSplats = 7,792,529 (全部节点, 未缩小)
数据源 = 同一个 7.8M SuperCompressedSplat (未切换)
```

### 错误总结

| 错误             | 原因                                 | 后果                                            |
| ---------------- | ------------------------------------ | ----------------------------------------------- |
| 未缩小 numSplats | `sc.add(splat)` 时 splat.counts=7.8M | SplattingPlugin 为 7.8M 准备排序缓冲等          |
| 未切换数据源     | 始终用同一个全量 Splat               | 内部节点占用纹理空间，setExternalOrder 可能选中 |
| 无间接层         | setExternalOrder 直接索引 GS 纹理    | 索引偏移错误或指向内部节点时无保护              |
| 内部节点可被渲染 | flush 曾输出内部节点到 order         | 同心椭球                                        |

---

## 三、aholo-viewer-master 的可视化方式

aholo-viewer-master 有 **两条完全独立的 LOD 路径**：

### 路径 A（生产）：LodSplat — 每个 LOD 级别独立 Splat 对象

```
lod-meta.json (空间节点 + LOD 级别)
     │
     ▼
LodSplat  ──→ 每帧 tick(camera) 评估每个节点的 targetLevel
     │
     ├── 需要提升 LOD → ResourceManager.loadSplat() → 创建 Splat → scene.add()
     └── 需要降低 LOD → scene.remove(splat) → ResourceManager.release()
```

**使用页面：**

- `website/src/client/viewer.ts`（主 viewer）
- `website/src/content/examples/walk-demo.ts`
- `website/src/content/examples/splatting-lod-stream.ts`
- `website/src/content/examples/home-interaction.ts`

**LodSplat 工作原理：**

1. lod-meta.json 定义空间节点（AABB + 多个 LOD 级别）
2. 每帧 `tick(camera)` 计算每个节点的 targetLevel（基于距离、视锥体、预算）
3. 异步 `flush()` 循环：将 target 与 current 对比 → 加载/卸载 Splat 对象
4. 每个 LOD 级别的 Splat 对象：**独立的 CompressedSplat/SuperCompressedSplat**，有自己的 GPU 纹理
5. 高级 LOD = 精细 Splat（count 大），低级 LOD = 粗糙 Splat（count 小）
6. 切换时：移除旧 Splat → 添加新 Splat → SplattingPlugin 自动处理排序

**关键特性：没有 setExternalOrder，没有 BlockTree，没有 decodeRad。**

### 路径 B（实验性）：BlockLodRenderer + setExternalOrder

```
.rad 文件 → decodeRad → 合并为单个 SuperCompressedSplat → sc.add(splat)
                                 ↓
                          BlockTree.addBlock() × N
                                 ↓
                          每帧: traverse → depthSort → setExternalOrder
```

**使用页面：**

- `bv-lod-test.astro`
- `bv-debug.astro`

**从未在生产环境中使用。** 这就是我一直在修改的路径。

### 两路径对比

| 方面      | LodSplat（生产）                 | BlockLodRenderer（实验）         |
| --------- | -------------------------------- | -------------------------------- |
| 数据格式  | `.esz`/`.splat` chunks           | `.rad` 编码文件                  |
| GPU 数据  | 多个独立 Splat 对象              | 单个合并的 7.8M 节点 Splat       |
| LOD 选择  | 整数级别（每个节点 0..maxLevel） | 连续 pixel_scale BFS 遍历        |
| 节点切换  | 移除旧 Splat + 添加新 Splat      | setExternalOrder 过滤            |
| 深度排序  | SplattingPlugin 内部 GPU 排序    | CPU depthSort + setExternalOrder |
| 加载/卸载 | ResourceManager 引用计数缓存     | BlockManager 距离状态机          |
| 纹理管理  | 每个 Splat 独立纹理              | SharedTexturePool（未使用）      |
| 同心椭球  | **不存在**（各 LOD 级别不混合）  | **存在**（7.8M 节点大杂烩）      |

### 为什么 LodSplat 没有同心椭球

因为每个 Splat 对象只包含**单个 LOD 级别的 GS**，不存在"同一纹理里既有父节点又有子节点"的情况。
当 camera 移动时，LodSplat 做的是**原子切换**：移除低 LOD 的 Splat → 添加高 LOD 的 Splat。
这两个 Splat 在 GPU 上是完全独立的数据集，不会互相干扰。

### 对我当前路径的启示

我的 `BlockLodRenderer` + `setExternalOrder` 路径的**根本问题**是：

1. 把所有 LOD 级别塞进同一个 Splat（7.8M 大杂烩）
2. 靠 `setExternalOrder` 在运行时过滤
3. `setExternalOrder` 在 aholo 里是实验性 API，生产代码不用它
4. LodSplat 的生产路径证明：**正确的做法是每个 LOD 级别独立 Splat + scene.add/remove**
5. Spark 的路径类似但更先进：`lodIndices` 间接纹理 + `maybeLookupIndex` 着色器映射

**修复方向：** 要么学 LodSplat（每个 LOD 级别独立 Splat），要么学 Spark（lodIndices 间接层 + 独立的 LOD 数据源）。当前"全量数据 + setExternalOrder 过滤"的方案在 aholo 体系内不被支持。

---

## 四、混合方案设计：Block 切换 + Block 内 LOD

### 目标

结合两个体系各自的优点：

- **aholo LodSplat** 的 block 级按需加载/卸载（基于距离）
- **我们的 LOD 树** 的连续 pixel_scale 级别选择

### 架构概览

```
构建时:
  .splat → 空间分块 → 每块构建 LOD 树
                       ├── 叶子节点 → level_0.splat（最精细）
                       ├── 部分合并节点 → level_1.splat
                       ├── 更多合并节点 → level_2.splat（最粗糙）
                       └── 完整 LOD 树 → block_N.rad（供遍历决策）

运行时:
  BlockManager 按距离管理 block 生命周期
       │
       ▼
  每个 active block:
       ├── 显示当前 LOD 级别的 Splat（通过 scene.add/remove 切换）
       └── 每帧用 BlockTree.traverse() 评估 pixel_scale
                │
                ▼
           决定是否需要切换 LOD 级别
```

### 构建时（process-block-splat.mjs 改造）

当前每个 block 已经输出：

- `block_N.splat`（所有叶子 GS，最精细）
- `block_N.rad`（全部树节点）

需要额外生成 N 个离散 LOD 级别的 `.splat` 文件：

```
每个 block:
  build_lod_tree() → LOD 树

  遍历树的层级，提取每层节点：
  level_0: 所有叶子节点 (count = 原始 GS 数) → block_N_l0.splat
  level_1: 取前 X% 的细节点 → block_N_l1.splat
  level_2: 取更少节点 → block_N_l2.splat
  ...

  输出 lod-meta.json：
  {
    "files": {
      "blocks": [
        { "id": 0, "bound": {...}, "lods": [
          { "level": 0, "file": "block_0_l0.splat", "count": 200000 },
          { "level": 1, "file": "block_0_l1.splat", "count": 50000 },
          { "level": 2, "file": "block_0_l2.splat", "count": 10000 },
        ]}
      ]
    }
  }
```

### 运行时 BlockTree 的作用

BlockTree 不再用于 `setExternalOrder`，而是**仅用于 LOD 级别决策**：

```typescript
// 每帧对每个 active block:
const treeData = blockTree.getTreeData(blockId);
const config = { cameraPos, cameraForward, lodScale, pixelScaleLimit: 0, maxSplats: Infinity };
const count = traverseBlock(treeData, config, tempBuffer);
// count = 当前相机下应显示的 GS 数
// 用 count 与各 LOD 级别的节点数对比，选择最接近的级别
```

或者更简单：用 `traverseBlock` 计算 root 的 pixelScale，映射到 LOD 级别。

### 原子切换策略（参考 LodSplat 的生产实践）

```
LOD 级别切换 = 原子操作：
  1. 创建新 LOD 级别的 Splat（从预加载的数据）
  2. 将新 Splat 的 transform 设为 block 的 transform
  3. scene.add(新 Splat)
  4. scene.remove(旧 Splat)（旧 Splat 的 GPU 资源被释放）
```

不需要 `setExternalOrder`，不需要 `depthSort`，SplattingPlugin 内部处理排序。

### LOD 级别数

建议每个 block 3-5 个级别：

| 级别 |    节点占比     | 说明     |
| :--: | :-------------: | -------- |
|  0   |  100%（叶子）   | 满细节   |
|  1   |      ~50%       | 中等 LOD |
|  2   |      ~10%       | 粗糙     |
|  3   | ~2%（合并节点） | 极粗糙   |

### 优缺点

| 优点                                      | 缺点                                 |
| ----------------------------------------- | ------------------------------------ |
| 使用 aholo 已验证的 LodSplat 模式         | 需要生成额外的 .splat 文件           |
| BlockManager 按距离加载/释放              | LOD 级别切换有延迟（需要 loadSplat） |
| 没有 `setExternalOrder`                   | 离散级别切换（非连续）               |
| 每个 Splat 级别独立 GPU 纹理 → 无同心椭球 | 需要更多的 GPU 内存                  |

### 与现状的关系

我当前写的 `BlockTree`、`traverseBlock`、`decodeRad` 代码**大部分可以复用**：

- `BlockTree` 和 `traverseBlock` → 用于 LOD 级别决策（非 setExternalOrder）
- `decodeRad` → 用于构建时的级别提取（或运行时决策）
- `BlockManager` → 用于 block 生命周期管理

需要新写的：

- `process-block-splat.mjs` 中生成多级别 `.splat` 文件的逻辑
- `bv-lod-test.astro` 中 per-block 多 Splat 管理和切换逻辑
