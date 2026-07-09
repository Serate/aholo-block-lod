#!/usr/bin/env node
/**
 * CLI tool: process a .splat file through the block+LOD pipeline.
 *
 * Usage:
 *   node scripts/process-block-splat.mjs <input.splat> <output-dir>
 *
 * Output:
 *   output-dir/lod-meta.json
 *   output-dir/block_0.rad, output-dir/block_1.rad, ...
 */
import { createRequire } from 'node:module';
import { readFileSync, mkdirSync, writeFileSync, existsSync } from 'node:fs';
import { join, resolve } from 'node:path';

const require = createRequire(import.meta.url);

const [,, inputPath, outputDir] = process.argv;
if (!inputPath || !outputDir) {
    console.error('Usage: node process-block-splat.mjs <input.splat> <output-dir>');
    process.exit(1);
}

// Load native module
const nativePath = join(resolve(import.meta.dirname, '..'),
    'packages/splat-transform-native/splat-transform-win32-x64-msvc/splat-transform.node');
if (!existsSync(nativePath)) {
    console.error(`Native module not found at: ${nativePath}`);
    process.exit(1);
}
const native = require(nativePath);

console.log(`Reading ${inputPath}...`);
const splatBuf = readFileSync(inputPath);
const numGs = Math.floor(splatBuf.byteLength / 32);
console.log(`GS count: ${numGs}`);

// Parse .splat format: 6xf32 + 8xu8 per GS
const centerF32 = new Float32Array(numGs * 3);
const scaleF32 = new Float32Array(numGs * 3);
const quatF32 = new Float32Array(numGs * 4);
const rgbaF32 = new Float32Array(numGs * 4);
const view = new DataView(splatBuf.buffer);

for (let i = 0; i < numGs; i++) {
    const o = i * 32;
    centerF32[i * 3]     = view.getFloat32(o, true);
    centerF32[i * 3 + 1] = view.getFloat32(o + 4, true);
    centerF32[i * 3 + 2] = view.getFloat32(o + 8, true);
    scaleF32[i * 3]      = view.getFloat32(o + 12, true);
    scaleF32[i * 3 + 1]  = view.getFloat32(o + 16, true);
    scaleF32[i * 3 + 2]  = view.getFloat32(o + 20, true);
    rgbaF32[i * 4]       = splatBuf[o + 24] / 255;
    rgbaF32[i * 4 + 1]   = splatBuf[o + 25] / 255;
    rgbaF32[i * 4 + 2]   = splatBuf[o + 26] / 255;
    rgbaF32[i * 4 + 3]   = splatBuf[o + 27] / 255;
    quatF32[i * 4]       = (splatBuf[o + 29] - 128) / 128;
    quatF32[i * 4 + 1]   = (splatBuf[o + 30] - 128) / 128;
    quatF32[i * 4 + 2]   = (splatBuf[o + 31] - 128) / 128;
    quatF32[i * 4 + 3]   = (splatBuf[o + 28] - 128) / 128;
    // normalize quat
    const len = Math.sqrt(
        quatF32[i * 4] ** 2 + quatF32[i * 4 + 1] ** 2 +
        quatF32[i * 4 + 2] ** 2 + quatF32[i * 4 + 3] ** 2,
    );
    if (len > 1e-10) {
        quatF32[i * 4]     /= len;
        quatF32[i * 4 + 1] /= len;
        quatF32[i * 4 + 2] /= len;
        quatF32[i * 4 + 3] /= len;
    }
}

// Compute AABB to split into blocks
let cxMin = Infinity, cxMax = -Infinity;
let cyMin = Infinity, cyMax = -Infinity;
let czMin = Infinity, czMax = -Infinity;
for (let i = 0; i < numGs; i++) {
    cxMin = Math.min(cxMin, centerF32[i * 3]);     cxMax = Math.max(cxMax, centerF32[i * 3]);
    cyMin = Math.min(cyMin, centerF32[i * 3 + 1]); cyMax = Math.max(cyMax, centerF32[i * 3 + 1]);
    czMin = Math.min(czMin, centerF32[i * 3 + 2]); czMax = Math.max(czMax, centerF32[i * 3 + 2]);
}

const MAX_BLOCK_GS = 200000;

// Simple spatial split: sort GS along longest axis, divide into blocks
function splitIntoBlocks() {
    // Create index array as regular array (TypedArray.sort returns new array)
    const indices = Array.from({ length: numGs }, (_, i) => i);

    // Find longest axis
    const dx = cxMax - cxMin, dy = cyMax - cyMin, dz = czMax - czMin;
    const axis = dx >= dy && dx >= dz ? 0 : dy >= dz ? 1 : 2;

    // Sort by position along longest axis
    indices.sort((a, b) => centerF32[a * 3 + axis] - centerF32[b * 3 + axis]);

    // Divide into blocks of MAX_BLOCK_GS
    const blocks = [];
    for (let start = 0; start < numGs; start += MAX_BLOCK_GS) {
        const end = Math.min(start + MAX_BLOCK_GS, numGs);
        // Compute AABB
        let mn = [Infinity, Infinity, Infinity];
        let mx = [-Infinity, -Infinity, -Infinity];
        for (let j = start; j < end; j++) {
            const idx = indices[j];
            mn[0] = Math.min(mn[0], centerF32[idx * 3]);
            mn[1] = Math.min(mn[1], centerF32[idx * 3 + 1]);
            mn[2] = Math.min(mn[2], centerF32[idx * 3 + 2]);
            mx[0] = Math.max(mx[0], centerF32[idx * 3]);
            mx[1] = Math.max(mx[1], centerF32[idx * 3 + 1]);
            mx[2] = Math.max(mx[2], centerF32[idx * 3 + 2]);
        }
        blocks.push({ start, end, min: mn, max: mx });
    }
    return blocks;
}

console.log('Splitting into blocks...');
console.time('split');
const blockRanges = splitIntoBlocks();
console.timeEnd('split');
console.log(`Blocks: ${blockRanges.length}`);

// Process each block
const multipliers = new Float32Array([1.0, 1.4, 1.7]);
const outDir = resolve(outputDir);
mkdirSync(outDir, { recursive: true });

const outputBlocks = [];

for (let bi = 0; bi < blockRanges.length; bi++) {
    const range = blockRanges[bi];
    const count = range.end - range.start;
    console.log(`Block ${bi}: ${count} GS (${range.start}..${range.end})`);

    // Gather GS data for this block
    const bc = new Float32Array(count * 3);
    const bs = new Float32Array(count * 3);
    const bq = new Float32Array(count * 4);
    const br = new Float32Array(count * 4);

    for (let j = 0; j < count; j++) {
        const gi = j + range.start;
        for (let d = 0; d < 3; d++) {
            bc[j * 3 + d] = centerF32[gi * 3 + d];
            bs[j * 3 + d] = scaleF32[gi * 3 + d];
        }
        for (let d = 0; d < 4; d++) {
            bq[j * 4 + d] = quatF32[gi * 4 + d];
            br[j * 4 + d] = rgbaF32[gi * 4 + d];
        }
    }

    // Write .splat file from the original GS data (before tree construction)
    console.time(`  writeSplat ${bi}`);
    const ITEM = 32;
    const splatBuf = Buffer.alloc(count * ITEM);
    for (let j = 0; j < count; j++) {
        const o = j * ITEM;
        splatBuf.writeFloatLE(bc[j * 3],     o);
        splatBuf.writeFloatLE(bc[j * 3 + 1], o + 4);
        splatBuf.writeFloatLE(bc[j * 3 + 2], o + 8);
        splatBuf.writeFloatLE(bs[j * 3],     o + 12);
        splatBuf.writeFloatLE(bs[j * 3 + 1], o + 16);
        splatBuf.writeFloatLE(bs[j * 3 + 2], o + 20);
        splatBuf[o + 24] = Math.round(br[j * 4] * 255);
        splatBuf[o + 25] = Math.round(br[j * 4 + 1] * 255);
        splatBuf[o + 26] = Math.round(br[j * 4 + 2] * 255);
        splatBuf[o + 27] = Math.round(br[j * 4 + 3] * 255);
        splatBuf[o + 28] = Math.min(255, Math.max(0, Math.round(bq[j * 4 + 3] * 128 + 128))); // qw
        splatBuf[o + 29] = Math.min(255, Math.max(0, Math.round(bq[j * 4] * 128 + 128)));     // qx
        splatBuf[o + 30] = Math.min(255, Math.max(0, Math.round(bq[j * 4 + 1] * 128 + 128))); // qy
        splatBuf[o + 31] = Math.min(255, Math.max(0, Math.round(bq[j * 4 + 2] * 128 + 128))); // qz
    }
    const splatName = `block_${bi}.splat`;
    writeFileSync(join(outDir, splatName), splatBuf);
    console.timeEnd(`  writeSplat ${bi}`);

    // Build LOD tree
    console.time(`  buildTree ${bi}`);
    const tree = native.buildLodTree(bc, bs, bq, br, null, count, 0, multipliers);
    console.timeEnd(`  buildTree ${bi}`);

    // Encode RAD
    console.time(`  encodeRad ${bi}`);
    const rad = native.encodeRad(
        tree.center, tree.scale, tree.quat, tree.rgba,
        null, tree.childStart, tree.childCount, tree.treeNodeCount, 0,
    );
    console.timeEnd(`  encodeRad ${bi}`);
    console.log(`  -> ${tree.treeNodeCount} nodes, ${rad.byteLength} bytes`);

    // Write .rad file
    const fileName = `block_${bi}.rad`;
    writeFileSync(join(outDir, fileName), Buffer.from(rad));

    outputBlocks.push({
        bound: { min: range.min, max: range.max },
        file: bi,
        count: tree.gsCount,
    });
}

// Write lod-meta.json
const lodMeta = {
    magicCode: 0x262834,
    type: 'lod-splat',
    version: '1.0',
    counts: numGs,
    shDegree: 0,
    levels: 5,
    files: outputBlocks.map(b => `block_${b.file}.splat`),
    tree: outputBlocks.map(b => ({
        bound: b.bound,
        file: b.file,
        count: b.count,
    })),
};

writeFileSync(join(outDir, 'lod-meta.json'), JSON.stringify(lodMeta, null, 2));
console.log(`\nDone! Output in ${outDir}`);
console.log(`  ${outputBlocks.length} blocks, ${numGs} total GS`);
