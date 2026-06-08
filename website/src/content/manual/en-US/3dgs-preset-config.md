---
title: 3DGS Preset Config
description: Choose a preset configuration based on the 3DGS data format, precision needs, and performance target.
order: 4
---

## Background

No single configuration covers every 3DGS scene. Scenes can differ significantly in data precision, file size, GPU memory usage, device performance, and image quality requirements, so choose a configuration set that matches the business target.

This page summarizes common data formats, `packType` differences, preset options, and the parameters you can tune after choosing a preset.

## Quick Choice

Choose the preset according to product constraints first, then tune only the most important parameters. Avoid changing precision, sorting, and blur parameters at the same time at the start.

| Target Scenario                                           | Recommended Preset    | Key Settings                                                                                             |
| --------------------------------------------------------- | --------------------- | -------------------------------------------------------------------------------------------------------- |
| Image quality first, with strong user hardware            | Max Quality           | `compressed`, `pack.highPrecisionEnabled`, `composite.highPrecisionEnabled`, `sort.highPrecisionEnabled` |
| Large scenes that can fail under low precision            | Quality First         | `compressed`, `pack.highPrecisionEnabled`                                                                |
| Weaker devices that still need to open large-scale scenes | Balanced              | `super-compressed`, `pack.cameraRelativeEnabled`                                                         |
| Weaker devices that still need a mostly complete image    | Performance First     | `super-compressed`, `raster.detailCullingThreshold`, `raster.maxPixelRadius`                             |
| Very large scenes or very low-end devices                 | Extreme Performance 0 | `pack.sortedLayoutEnabled`, `sort.minIntervalMs`, more aggressive precision compression                  |
| Source data is sog and the goal is to open larger scenes  | Extreme Performance 1 | `sog`, `pack.precalculateEnabled`, GPU memory usage                                                      |

## 3DGS File Formats

| Format           | Size                      | Render Quality               | Implementation Notes                                                                                                                                                                                                                                |
| ---------------- | ------------------------- | ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ply`            | 100%                      | Good                         | High original precision and the largest file size.                                                                                                                                                                                                  |
| `compressed ply` | 30%, about 17% after gzip | Good                         | Uses 256 splats per chunk and is likely spatially partitioned similarly to ksplat. `center`, `quat`, `scale`, and `rgb` are compressed by min/max, rescale, and quantization. SH can be compressed to u8; observed data is about 5 bit.             |
| `spz`            | 10%                       | Average                      | Retains relatively high precision for core splat data, especially `center`, so sharpness loss is lower. SH precision is very low and can cause visible color shifts in fine-detail scenes.                                                          |
| `splat`          | 14%                       | Average, not universal       | Drops `shN` during compression. Layout: `center.xyz (f32)`, `scale.xyz (f32)`, `color.rgba (u8)`, `quat (u8)`, 32 bytes in total.                                                                                                                   |
| `ksplat`         | 20%-30%                   | Depends on compression level | Level 0 is uncompressed, level 1 is 16 bit, and level 2 is 8 bit. It spatially clusters splats for local coordinate compression, following a similar approach to compressed ply.                                                                    |
| `sog`            | 5%                        | Average                      | Applies PLAS sorting to `center`, `scales`, `quats`, and `sh0(rgba)`, then computes min/max values and quantizes the data. `shN` uses k-means clustering with centroids and labels to restore data while reducing size. Images tend to be blurrier. |

### compressed ply Quantization Example

![compressed ply quantization](../assets/3dgs-preset-config/compressed-ply-quantization.png)

## packType

`packType` controls the data precision generated when parsing splats. Different settings trade off size, quality, and performance.

### Compressed

| Field           | Precision    |
| --------------- | ------------ |
| `position`      | `f32 (3)`    |
| `scale`         | `f16 (3)`    |
| `quat`          | `f16 (4)`    |
| `color & alpha` | `f16 (4)`    |
| `shN`           | `s_11_10_11` |

`Compressed` favors image quality and data precision. Use it for quality-sensitive output, large scenes, or scenes that show artifacts at lower precision.

### SuperCompressed

| Field           | Precision                              |
| --------------- | -------------------------------------- |
| `position`      | `f16 (3)`                              |
| `scale`         | `u8 (3)`                               |
| `quat`          | `u8 (4)`                               |
| `color & alpha` | `u8 (4)`                               |
| `shN`           | `sh1 (sint5)`, `sh2` and `sh3 (sint4)` |

`SuperCompressed` favors file size, memory, and GPU memory control. Use it when resources are constrained, devices are lower-end, or performance is the priority.

### Sog

`Sog` is for sog data. It has the smallest size, but the image can look blurrier. Prefer it when the source format is sog and the data has no `shN`, or when extreme scene scale is required.

## Preset List

| Preset                | Recommended Scenario                                                                                                                                                         |
| --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Max Quality           | Use when image quality has the highest priority and the device is very powerful.                                                                                             |
| Quality First         | Use when image quality matters and device performance is still acceptable.                                                                                                   |
| Balanced              | Use when image quality requirements are low and device performance is limited, but large scenes still need to be supported.                                                  |
| Performance First     | Use when image quality requirements are low and device performance is limited.                                                                                               |
| Extreme Performance 0 | Use on extremely low-end devices or for extremely large scenes.                                                                                                              |
| Extreme Performance 1 | Use on extremely low-end devices or for extremely large scenes when the source data is sog. Prefer this preset when the condition is met, because it can open larger scenes. |

### Max Quality

```typescript
// set parser config
const splatData = await SplatLoader.parseSplatData(
    // file type and data
    splatFileType,
    content,
    // compress config
    SplatLoader.SplatPackType.Compressed,
);
const splat = await SplatUtils.createSplat(splatData);
splat.autoFreeResourceOnGpuPacked = true;
viewer.getScene().add(splat);

// update viewer config
setViewerConfig(viewer, {
    pipeline: {
        Splatting: {
            pack: {
                highPrecisionEnabled: true,
                cameraRelativeEnabled: false,
            },
            raster: {
                normalizedFalloff: true,
                detailCullingThreshold: 0,
            },
            sort: {
                highPrecisionEnabled: true,
            },
            composite: {
                enabled: true,
                highPrecisionEnabled: true,
            },
        },
    },
});
```

![max quality render result](../assets/3dgs-preset-config/preset-max-quality-result.png)

### Quality First

```typescript
// set parser config
const splatData = await SplatLoader.parseSplatData(
    // file type and data
    splatFileType,
    content,
    // compress config
    SplatLoader.SplatPackType.Compressed,
);
const splat = await SplatUtils.createSplat(splatData);
splat.autoFreeResourceOnGpuPacked = true;
viewer.getScene().add(splat);

// update viewer config
setViewerConfig(viewer, {
    pipeline: {
        Splatting: {
            pack: {
                highPrecisionEnabled: true,
                cameraRelativeEnabled: false,
            },
        },
    },
});
```

![quality first render result](../assets/3dgs-preset-config/preset-quality-first-result.png)

### Balanced

```typescript
// set parser config
const splatData = await SplatLoader.parseSplatData(
    // file type and data
    splatFileType,
    content,
    // compress config
    SplatLoader.SplatPackType.SuperCompressed,
);
const splat = await SplatUtils.createSplat(splatData);
viewer.getScene().add(splat);

// update viewer config
setViewerConfig(viewer, {
    pipeline: {
        Splatting: {},
    },
});
```

### Performance First

```typescript
// set parser config
const splatData = await SplatLoader.parseSplatData(
    // file type and data
    splatFileType,
    content,
    // compress config
    SplatLoader.SplatPackType.SuperCompressed,
);
const splat = await SplatUtils.createSplat(splatData);
splat.autoFreeResourceOnGpuPacked = true;
viewer.getScene().add(splat);

// update viewer config
setViewerConfig(viewer, {
    pipeline: {
        Splatting: {
            pack: {
                cameraRelativeEnabled: false,
            },
            raster: {
                maxStdDev: Math.sqrt(5),
            },
        },
    },
});
```

![performance first render result](../assets/3dgs-preset-config/preset-performance-first-result.png)

### Extreme Performance 0

```typescript
// set parser config
const splatData = await SplatLoader.parseSplatData(
    // file type and data
    splatFileType,
    content,
    // compress config & sh
    SplatLoader.SplatPackType.SuperCompressed,
    {
        maxShDegree: 0,
    },
);
const splat = await SplatUtils.createSplat(splatData);
splat.autoFreeResourceOnGpuPacked = true;
viewer.getScene().add(splat);

// update viewer config
setViewerConfig(viewer, {
    pipeline: {
        Splatting: {
            pack: {
                precalculateEnabled: false,
                cameraRelativeEnabled: false,
                sortedLayoutEnabled: true,
            },
            raster: {
                detailCullingThreshold: 4,
                maxStdDev: Math.sqrt(5),
            },
            sort: {
                minIntervalMs: 160,
            },
        },
    },
});
```

![extreme performance 0 render result](../assets/3dgs-preset-config/preset-extreme-performance-0-result.png)

### Extreme Performance 1

```typescript
// set parser config
const splatData = await SplatLoader.parseSplatData(
    // file type and data
    SplatFileType.SOG,
    content,
    // compress config & sh
    SplatLoader.SplatPackType.Sog,
    {
        maxShDegree: 0,
    },
);
const splat = await SplatUtils.createSplat(splatData);
splat.autoFreeResourceOnGpuPacked = true;
viewer.getScene().add(splat);

// update viewer config
setViewerConfig(viewer, {
    pipeline: {
        Splatting: {
            pack: {
                precalculateEnabled: false,
                cameraRelativeEnabled: false,
                sortedLayoutEnabled: true,
            },
            raster: {
                detailCullingThreshold: 4,
                maxStdDev: Math.sqrt(5),
            },
            sort: {
                minIntervalMs: 160,
            },
        },
    },
});
```

![extreme performance 1 render result](../assets/3dgs-preset-config/preset-extreme-performance-1-result.png)

## Custom Configuration

Presets cannot cover every scene. In real integrations, choose the closest preset as the starting point, then tune a small number of key parameters.
Parameters can be adjusted through the [config](../config/) API:

```typescript
setViewerConfig(viewer, {
    pipeline: {
        Splatting: {
            // ... options..
        },
    },
});
```

| Parameter                                    | Purpose                                           | Recommendation                                                                                                                                                                                                                                                                                                                 |
| -------------------------------------------- | ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `pack.highPrecisionEnabled`                  | Enables high-precision data merging.              | Determines the final data precision used for rendering. Usually enable it for `compressed`; evaluate it per scene for `sog`.                                                                                                                                                                                                   |
| `pack.precalculateEnabled`                   | Enables spherical-harmonic calculation.           | Enable it when the data has no `shN` to save performance and GPU memory.                                                                                                                                                                                                                                                       |
| `pack.cameraRelativeEnabled`                 | Enables camera-relative position packing.         | If center values are large but the device cannot afford `highPrecisionEnabled`, try enabling it. When enabled, packing can run on demand on the GPU, so disable `autoFreeResourceOnGpuPacked` to avoid repeated texture uploads. LOD data already needs repeated packing, so it can use this path without the same extra cost. |
| `pack.sortedLayoutEnabled`                   | Enables sorted layout packing.                    | A performance optimization for large scenes, usually used with `sort.minIntervalMs`. It can often improve performance by 50%-100%, but increases GPU memory usage.                                                                                                                                                             |
| `composite.highPrecisionEnabled`             | Enables a high-precision render attachment.       | Consider enabling it when the scene shows ripple-like banding artifacts, or when quality is important. It increases GPU memory usage.                                                                                                                                                                                          |
| `raster.normalizedFalloff`                   | Enables normalized Gaussian falloff.              | Most scenes show little difference. Do not enable it unless you need the best possible quality.                                                                                                                                                                                                                                |
| `raster.preBlurAmount` / `raster.blurAmount` | Controls blur parameters.                         | Non-AA training results usually use `0.3 / 0`; AA training results usually use `0 / 0.3`. Other values are not recommended.                                                                                                                                                                                                    |
| `raster.focalAdjustment`                     | Adjusts splat spread scale.                       | `2` is closer to the reference result.                                                                                                                                                                                                                                                                                         |
| `raster.detailCullingThreshold`              | Approximate detail culling.                       | Usually in `[0, 4]`. Setting it to `1` usually causes minimal visual loss; the performance gain depends on scene detail.                                                                                                                                                                                                       |
| `raster.maxPixelRadius`                      | Maximum screen-space range covered by a Gaussian. | Default is `1024`; the recommended range is `[128, 1024]`. Too small a value can make the scene look broken.                                                                                                                                                                                                                   |
| `raster.maxStdDev`                           | Maximum standard deviation of Gaussian spread.    | Should be between `sqrt(5)` and `sqrt(9)`. Larger values cost more performance but improve quality; `sqrt(8)` is usually a practical quality/performance midpoint.                                                                                                                                                             |
| `sort.highPrecisionEnabled`                  | Controls sorting precision.                       | When enabled, sorting uses float precision. In most rendering scenes, the visual improvement is small.                                                                                                                                                                                                                         |
| `sort.minIntervalMs`                         | Minimum interval between sorting operations.      | Usually used with `pack.sortedLayoutEnabled`. A common setting is `16 * n`, where `n` is no greater than `10`.                                                                                                                                                                                                                 |

### normalizedFalloff Comparison

![normalizedFalloff off](../assets/3dgs-preset-config/normalized-falloff-off.png)
![normalizedFalloff on](../assets/3dgs-preset-config/normalized-falloff-on.png)
