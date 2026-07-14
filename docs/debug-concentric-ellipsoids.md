# 同心椭球问题 — 分层隔离验证方案

## 问题现象

block-viewer 中渲染出现"同心椭球"（concentric ellipsoids），即多个大小不同的椭球体以同一中心嵌套排列。

## 假设

可能的原因（由外到内）：

| 层级 | 假设                                                   | 验证方法   |
| ---- | ------------------------------------------------------ | ---------- |
| A    | `SuperCompressedSplatData` 构造（set\* API）有格式错误 | 已部分验证 |
| B    | 内部（父）节点被渲染成可见大 GS                        | 新建测试   |
| C    | LOD 遍历选中了父子节点同时输出                         | 日志分析   |
| D    | `setExternalOrder` 的 order 索引映射到错误位置         | 索引校验   |
| E    | WebGL 纹理上载失败（`texSubImage2D` 错误）导致数据损坏 | 错误分析   |

---

## 验证页说明

已创建的独立测试页：

| 页面           | 路径                   | 用途                                      |
| -------------- | ---------------------- | ----------------------------------------- |
| `bv-radonly`   | `/zh-CN/bv-radonly/`   | .rad 叶子节点（4.38M），无 LOD，无 .splat |
| `bv-allnodes`  | `/zh-CN/bv-allnodes/`  | .rad 全部节点（7.8M），无 LOD             |
| `block-viewer` | `/zh-CN/block-viewer/` | .splat 原始方案（对照基准）               |
| `bv-debug`     | `/zh-CN/bv-debug/`     | 开发者调试页                              |

---

## Step-by-Step 验证

### Step 1：确认 bv-radonly 是否正常

打开 `bv-radonly`。预期：

- 场景与 block-viewer（.splat 基准）**基本一致**
- **无**同心椭球
- 略糊（Ln0R8 量化精度损失）
- 右下 GS = 4,386,142

**结果：** ✅ 用户已确认"变糊了但整体清晰，无大 GS"

**结论：** `.rad` 解码 + `set*` API 写入 `SuperCompressedSplatData` → `createSplat` → 渲染 **链路正确**。
排除假设 A。

---

### Step 2：确认 bv-allnodes 是否正常

打开 `bv-allnodes`。预期：

- 如果有同心椭球 → 问题在**内部节点本身被渲染为可见 GS**
- 如果无同心椭球 → 问题在 LOD 遍历或索引映射

**前提：** 此页面与主 block-viewer 使用**完全相同的 7.8M SplatData 构造代码**，唯一区别是不调用 `setExternalOrder`。

---

### Step 3：根据 Step 2 结果选择分支

#### 分支 A：bv-allnodes 有同心椭球

问题出在 7.8M 节点的 SplatData 本身。

需要进一步细分：

**Step 3a：** 将 bv-allnodes 改为只渲染**叶子节点**（跳过内部节点），构造 4.38M SplatData。

- 如果正常 → 问题确认：**内部节点的数据被渲染成椭球**
- 原因：内部节点 scale 大、位置在簇中心，多个不同层级节点形成嵌套

**Step 3b：** 如果 Step 3a 仍有问题 → 问题在 4.38M 的叶子数据本身。

- 对比 bv-radonly（也是 4.38M 叶子）→ 两者代码是否完全一致

**修复方向：**

- 确保 setExternalOrder 只选中叶子节点（已做 flush-only-leaves）
- 如果内部节点不应出现在渲染数据中 → 只构造叶子节点的 SplatData

#### 分支 B：bv-allnodes 正常（无同心椭球）

问题出在 LOD 遍历或 setExternalOrder。

**Step 3c：** 在 bv-allnodes 基础上，手动构造一个简单的 `setExternalOrder`：

```javascript
// 构造一个只选中前 1000 个节点的 order
const order = new Uint32Array(1000);
for (let i = 0; i < 1000; i++) order[i] = i;
const plugin = __INTERNAL__.getSplattingPlugin();
if (plugin) plugin.setExternalOrder(order, 1000);
```

- 如果正常 → 选中的 1000 个 GS 渲染正确
- 如果不正常 → Order 索引指向了错误的纹理位置，说明索引映射有问题

**Step 3d：** 用 BlockLodRenderer 的 traverse 输出替代手动 order：

- 在 bv-allnodes 中加入 blockTree + traverse
- 对比 traverse 输出与手动 order 的渲染差异

**Step 3e：** 打印 traverse 选中索引的前几个值，验证其合理性：

```javascript
console.log('selected indices:', Array.from(order.subarray(0, 10)));
```

---

### Step 4：WebGL 错误排查

`texSubImage2D: ArrayBufferView not big enough for request` 是否影响渲染：

**Step 4a：** 在 bv-allnodes 中减小 GS 数量（如只加载 block 0），看错误是否消失
**Step 4b：** 如果错误由空 SH 纹理引起（shDegree=0），确认不影响主纹理

---

## 验证结果决策树

```
bv-allnodes 有同心椭球?
├── YES → 内部节点被渲染为可见椭球
│         └── 修复：SplatData 只包含叶子节点
│              或：setExternalOrder 确保不选中内部节点
│
└── NO → 问题在 LOD 遍历或索引映射
          ├── 手动 setExternalOrder 正常?
          │   ├── YES → traverse 输出索引有问题
          │   │         └── 打印选中索引，检查是否超出范围或偏移错误
          │   └── NO  → setExternalOrder 本身有问题
          │               └── Order 纹理或索引映射有 bug
          │
          └── WebGL 错误导致纹理损坏?
              └── 修复空 SH 纹理创建
```

---

## 当前状态

| Step                 |  状态   | 结论                            |
| -------------------- | :-----: | ------------------------------- |
| Step 1 (bv-radonly)  | ✅ 通过 | RAD 解码 + set\* API 链路正确   |
| Step 2 (bv-allnodes) | 🔲 待测 | **需用户打开 bv-allnodes 确认** |
| Step 3 分支          | 🔲 待定 | 取决于 Step 2                   |
