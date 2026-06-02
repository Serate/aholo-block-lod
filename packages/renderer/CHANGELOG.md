# ChangeLOG

## 1.3.0

1. Features
    - `SplatUtils`add support for `center` and `ellipsoid`
        > **`constructor` has been changed, migrate: `new SplatBVH(operator)` -> `new BVH(SplatCenterPrimitiveSource(operator))`**
2. Fixes
    - `SplatUtils`state texture type change to `r8uint`

## 1.2.9

1. Features
    - use `api-extractor` to rollup dts.
    - add `esz` and `spzV4` format support.
2. Fixes
    - fix typing for `MeshBasicMaterial.setValues`
    - fix typing for `MeshPhongMaterial.setValues`
    - Simplify some material typings
    - fix type only classes
    - cleanup `package.json`

## 1.1.0

1. Features
    - upgrade packages: `typescript@^6.0.3`, `tslib@^2.8.1`
    - sync base packages
    - remove unused module `render-cloud`

## 1.0.0

1. Features
    - First release
