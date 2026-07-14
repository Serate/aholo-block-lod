# LOD 渲染参考 Spark 的重设计方案

## 核心发现

Spark 的 LOD 能正确工作的关键不在于遍历算法，而在于**合并节点的视觉质量**。

### 遍历算法的行为（我们与 Spark 一致）

```
遍历过程中:
  - 能展开的节点：pop 父节点 → push 子节点 → 父节点不在输出中
  - 预算不足展开的节点：留在 frontier → flush 时输出作为 LOD 表示
  - 叶节点：始终输出

输出中不可能出现父子同框，因为：
  展开时父节点已被 pop，不会出现在 flush 中
  flush 中的节点都是未展开的，它们的子节点不在输出中
```

### Spark 的合并节点看起来正确的原因

Spark 的 `new_merged` 生成的内部节点:

- **opacity = totalWeight / ellipsoid_area(new_scales)**（不 clamp 到 1.0）
- **RGB 是子节点的加权平均**（和我们的相同）
- **scale 来自协方差特征分解**（和我们的相同）
- opacity 可能 > 1.0（表示合并覆盖度），渲染器会特殊处理

### 我们的合并节点看起来不对的可能原因

| 问题                             | 影响                           |
| -------------------------------- | ------------------------------ |
| opacity 被 clamp 到 1.0          | 内部节点透明度错误             |
| featureSize 计算异常（可能太小） | 遍历展开过早停止，选中过少节点 |
| 颜色/位置偏移                    | 内部节点渲染位置异常           |

---

## 修改方案

### 方案 A：修复 merge_nodes 的 opacity

`lod_tree.cpp` 中取消 opacity clamp：

```cpp
// 修改前
newOpacity = std::min(newOpacity, 1.0);

// 修改后
newOpacity = std::min(newOpacity, 2.0);  // 允许 LOD opacity
```

同时需要确保 SplattingPlugin 能处理 >1.0 的 opacity。如果不行，做第二步：使用不透明度 remap（Spark 的 `encode_lod_opacity()`）。

### 方案 B：RAD 编码精度提升

当前 `encodeRad` 用 `max_alpha=2.0` 编码 LOD opacity，但解码后 alpha/255 得到 [0,1] 范围。如果内部节点 opacity > 1.0，这个编码会丢失信息。

需要改为编码时用实际 max_alpha，或解码时用 `max_alpha` 还原。

### 方案 C：可视化验证节点数据

暂时禁用 LOD 遍历，直接手动选择不同层级的节点来渲染，观察单个内部节点的视觉效果：

1. 只渲染 root 节点 → 看是否像一个正常的大 GS
2. 只渲染 root 的 8 个子节点 → 看是否覆盖合理
3. 只渲染叶子节点（已做，清晰）

### 方案 D：与 .splat 对比渲染

在场景中并排渲染两个 GS 集合：

- 左：.rad 叶子节点（用 set\* API 创建）
- 右：.splat 原始 GS（parseSplatData）

如果两者视觉一致，说明 .rad 解码 + set\* 链路正确。问题在合并算法。

---

## 实施优先级

1. **方案 C**（先看单个内部节点的视觉效果）→ 确认合并算法是否正确
2. **方案 A**（修复 opacity）→ 让内部节点渲染更自然
3. **方案 D**（对比验证）→ 确认整条链路
4. **方案 B**（编码精度）→ 最后优化
