# lod_tree.cpp Code Review 记录

## 文件范围

`packages/splat-transform-native/source/src/splat/lod_tree.cpp`
`packages/renderer/src/traverse-block.ts` (TS 端遍历)

---

## merge_nodes 审查结果

### 与 Spark 的实现对比

| 环节                            | 我们的实现                         | Spark 实现                | 一致？ |
| ------------------------------- | ---------------------------------- | ------------------------- | :----: |
| 权重计算                        | `ellipsoid_area(scales) * opacity` | `area() * opacity()`      |   ✅   |
| 中心                            | 加权平均                           | 加权平均                  |   ✅   |
| RGB                             | 加权平均                           | 加权平均                  |   ✅   |
| 协方差                          | `R*S²*R^T` 特征分解                | `R*S²*R^T` 特征分解       |   ✅   |
| filter2 扩散                    | `(0.5*step)²`                      | `(0.5*step)²`             |   ✅   |
| 协方差 + delta + filter2 加权和 | 一致                               | 一致                      |   ✅   |
| 尺度                            | `sqrt(eigenvalue)`                 | `sqrt(eigenvalue)`        |   ✅   |
| 四元数                          | 从特征向量矩阵提取                 | 从特征向量矩阵提取        |   ✅   |
| **opacity**                     | **clamp 到 1.0**                   | **不 clamp（允许 >1.0）** |   ❌   |
| SH                              | 加权平均                           | 加权平均                  |   ✅   |
| featureSize                     | `max(scale) * 2`                   | `max(scale) * 2`          |   ✅   |

**结论：merge_nodes 算法的核心逻辑与 Spark 一致，仅 opacity 处理不同。**

但 opacity clamp 不是同心椭球的直接原因 — 它影响透明度而非位置/大小。

---

## 潜在问题清单

### 问题 1：opacity clamp + RAD 编码不匹配（中优先级）

```
merge_nodes: clamp(opacity, 1.0)
encodeRad: max_alpha = 2.0 (因为 has_lod=true)
解码: alpha_f32 = u8 / 255 = (clamped_opacity / 2.0)
```

内部节点 opacity 被 clamp 到 1.0 → 编码为 u8 时用 max_alpha=2.0 → 解码后 alpha_f32=0.5 → 父节点半透明，看起来"虚"。

### 问题 2：featureSize 可能太小（低优先级）

遍历日志显示大多数 block 只输出 8 个 GS（root 的 8 个子节点），说明子节点的 featureSize < pixelScaleLimit（0.001），展开过早停止。

如果 eigendecomposition 产生很小的特征值（由于数值问题），newScale 就会很小，featureSize 也会很小。但 filter2 扩散项应该防止这种情况。

### 问题 3：`from_scale_quat` 的 naming 误导（无功能影响）

变量命名 `axy` 容易让人误以为是协方差 xy 分量，但实际是 `(R*S)[1][0]`。已验证公式本身正确。

### 问题 4：`lastSelectedCount = 0` 时显示 totalNodes（前端问题）

block-viewer 中用 `(blockRenderer.lastSelectedCount || totalTreeNodes)` 作为 GS 数显示。当遍历返回 0 时，显示 7.8M 造成迷惑。实际渲染的是默认全部节点。

---

## 已排除的问题

- `from_scale_quat` 协方差公式 ✅ 正确
- Jacobi 特征分解 ✅ 正确（50 次迭代对 3x3 足够）
- 四元数提取 ✅ 正确
- `ellipsoid_area` ✅ 正确
- Morton 排序和 cell 分组 ✅ 正确
- level-morton permutation ✅ 根节点在 totalNodes-1
- childStart/childCount 偏移 ✅ BlockTree 中 local→global 映射正确
- flush 策略 ✅ 已匹配 Spark（不展开时输出节点本身）

---

## 最可能的根因

同心椭球的**直接原因不是合并算法**，而是 **全 7.8M 节点同时渲染造成的视觉混乱**（bv-allnodes 一团糊）。当用 LOD 遍历选出子集时（bv-lod-test），因为 budget 不足导致大部分区域只有内部节点被输出（粗 LOD），其视觉质量不佳（opacity 减半、颜色偏平均），形成"同心椭球"外观。

**修复方向（按优先级）：**

1. 去除 opacity clamp → 内部节点视觉更自然
2. 改善 RAD 编码的 alpha 精度 → 内部节点透明度正确
3. 验证遍历选中节点 → 确保覆盖场景的关键区域而非随机选择
