# aholo-block-lod 项目状态速查

## 仓库

- `https://github.com/Serate/aholo-block-lod`
- 基于 aholo-viewer 框架，Block + 树遍历架构

## 核心架构

```
场景 → 八叉树分块 → 每块独立 cycling_lod 建树 + level-morton → RAD 编码
→ 运行时按视锥加载 Block → C++ (N-API) 树遍历 → orderTex → GPU 渲染
```

## 已实现的文件

### C++ 核心 (splat-transform-native/source/)

| 文件                          | 作用                                     |
| ----------------------------- | ---------------------------------------- |
| `include/splat/morton_code.h` | Morton 编码函数 (`morton_encode`)        |
| `include/splat/lod_tree.h`    | `build_lod_tree` + `traverse_block` 声明 |
| `src/splat/lod_tree.cpp`      | cycling_lod 建树 + 堆遍历实现            |
| `src/splat/rad_decoder.cpp`   | RAD 解码 + gzip inflate (zlib)           |
| `src/splat/rad_encoder.cpp`   | RAD 编码（简化版）                       |
| `src/node/api_rad_decode.cpp` | N-API: `decodeRad` + `traverseBlock`     |
| `src/node/binding.cpp`        | N-API 注册 `decodeRad`、`traverseBlock`  |
| `src/splat/zlib/*.c`          | zlib inflate 源码                        |

### 待写

| 文件                                           | 作用                                    |
| ---------------------------------------------- | --------------------------------------- |
| `src/node/api_lod_tree.cpp`                    | N-API: `buildLodTree` 绑定              |
| `packages/renderer/src/block-manager.ts`       | Block 状态机 + 视锥测试                 |
| `packages/renderer/src/shared-texture-pool.ts` | 全局纹理页池 LRU                        |
| `website/src/client/viewer.ts`                 | 串联 BlockManager → traverse → orderTex |

### 构建

- C++ 测试编译: `source/build_test.cmd` (MSVC 直接编译)
- 生产构建: `pnpm build:renderer` (cmake-js, 需要 vcpkg)
- zlib 在 `src/splat/zlib/` 下，头文件在 `include/`

### 测试验证

- `test_rad.cpp` — 解码 + 遍历验证
- `test_rad.exe garden-7k-lod.rad` → ALL TESTS PASSED
- 6.1M GS 的 .rad 文件解码 + 树结构提取 + 遍历全链路通过

### N-API 函数签名 (TS 调用方式)

```typescript
// RAD 解码
const rad = native.decodeRad(fileData: Uint8Array) => {
  count, totalNodes, shDegree: number;
  childStart: Uint32Array;     // 树结构
  childCount: Uint16Array;
  center: Uint16Array;          // f16[3]
  rgba: Uint8Array;             // u8[4]
  scale: Uint8Array;            // u8[3] ln0r8
  quat: Uint8Array;             // u8[3] oct88r8
  sh?: Uint8Array;
}

// 遍历
const result = native.traverseBlock(  // 每帧调用
  childStart: Uint32Array,   // decodeRad 返回的
  childCount: Uint16Array,
  center: Uint16Array,       // f16[3]
  size: Uint16Array,          // f16 (feature_size)
  cameraPos: Float32Array,    // [x, y, z]
  maxSplats: number,
  lodScale: number,
  pixelScaleLimit: number
) => { indices: Uint32Array, numSplats: number }
```

### 关键参数

- MAX_BLOCK_GS = 200K, CHUNK_SIZE = 16384
- BASE = 2.0, MULTIPLIERS = [1.0, 1.4, 1.7]
- PRELOAD_DIST = 2.0, EVICT_DIST = 2.5
- MAX_ACTIVE_BLOCKS = 4, TEXTURE_POOL_PAGES = 64

### Spark 参考代码 (移植来源)

`D:\Project\3dgs\spark-2.1.0\`:

- `rust/spark-lib/src/cycling_lod.rs` → lod_tree.cpp
- `rust/spark-lib/src/ordering.rs` → morton_code.h
- `rust/spark-worker-rs/src/lod_tree.rs` → traverse_block
- `rust/spark-lib/src/rad.rs` → rad_encoder/decoder
- `rust/spark-lib/src/gsplat.rs` → `new_merged` 合并公式
- `src/SplatPager.ts` → SharedTexturePool 参考
- `src/SparkRenderer.ts` → orderTex / pixel_scale 参考

### 待解决问题

- cmake-js 完整构建需要 vcpkg (装好但网络问题导致 zlib 没装成；已直接用 zlib 源码绕过)
- `api_lod_tree.cpp` (buildLodTree N-API) 未写
- orderTex 访问方式需要确认 (SplattingPlugin 的 private 属性)
- rad_encoder.cpp 是简化版，编码参数未精确匹配 Spark
