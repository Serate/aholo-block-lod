import { type TreeData, type TraverseConfig, prepareTreeData, depthSort, computePixelScale } from './traverse-block.js';
import { WasmTraverser, type WasmTraverseParams } from './wasm-traverse.js';

/**
 * Decoded block data from a .rad file.
 */
export interface DecodedBlock {
    count: number;
    totalNodes: number;
    shDegree: number;
    childStart: Uint32Array;
    childCount: Uint16Array;
    center: Uint16Array;
    rgba: Uint8Array;
    scale: Uint8Array;
    quat: Uint8Array;
    sh?: Uint8Array;
}

/**
 * Holds per-block decoded tree data and provides per-frame LOD traversal.
 * Uses WASM traversal when available, falls back to JS.
 */
export class BlockTree {
    private _btLog = 0;
    blocks: DecodedBlock[] = [];
    private treeDataCache: (TreeData | null)[] = [];
    private nodeOffsets: number[] = [];
    private centerChunks: Float32Array[] = [];
    totalNodes: number = 0;
    private _cachedCenters: Float32Array | null = null;
    private _wasm: WasmTraverser | null = null;
    private _wasmBusy = false;

    addBlock(data: DecodedBlock, nodeOffset: number): void {
        this.blocks.push(data);
        this.nodeOffsets.push(nodeOffset);
        const td = prepareTreeData(data.center, data.scale, data.childStart, data.childCount, data.totalNodes);
        this.treeDataCache.push(td);
        this.centerChunks.push(td.centers);
        this.totalNodes += td.totalNodes;
        this._cachedCenters = null;
    }

    clear(): void {
        this.blocks.length = 0;
        this.treeDataCache.length = 0;
        this.nodeOffsets.length = 0;
        this.centerChunks.length = 0;
        this.totalNodes = 0;
    }

    /** Attempt to initialize WASM traverser (async, non-blocking). */
    initWasm(): void {
        if (this._wasm || this._wasmBusy) return;
        this._wasmBusy = true;
        const t = new WasmTraverser();
        t.ready
            .then(() => {
                this._wasm = t;
            })
            .catch(() => {
                /* WASM not available, use JS fallback */
            });
    }

    /**
     * Single-heap combined traversal. All roots compete on pixel_scale.
     * Per-block weighted budget limits each block's GS output.
     * Uses WASM when available.
     */
    traverse(
        activeBlockIds: number[],
        config: TraverseConfig,
        weights?: Float32Array,
    ): { order: Uint32Array; count: number } {
        const numBlocks = activeBlockIds.length;
        if (numBlocks === 0 || config.maxSplats === 0) {
            return { order: new Uint32Array(0), count: 0 };
        }

        const [camX, camY, camZ] = config.cameraPos;
        const [fwdX, fwdY, fwdZ] = config.cameraForward;
        const foveated = config.foveated;
        const lodScale = config.lodScale;
        const limit = config.pixelScaleLimit;

        // Weighted per-block budget
        const hasWeights = weights && weights.length >= numBlocks;
        const totalW = hasWeights ? weights!.reduce((s, w) => s + w, 0) : numBlocks;
        const budget = new Float64Array(this.treeDataCache.length);
        for (let i = 0; i < numBlocks; i++) {
            const w = hasWeights ? (weights![i] ?? 1) : 1;
            budget[activeBlockIds[i]] = Math.max(500, Math.floor((config.maxSplats * w) / totalW));
        }
        const spent = new Uint32Array(this.treeDataCache.length);

        if (this._btLog++ % 30 === 0) {
            const s = activeBlockIds
                .slice(0, 4)
                .map(id => `${id}:${budget[id]}`)
                .join(', ');
            console.log(`[bt] budget sample: ${s} (${numBlocks} blocks)`);
        }

        // ── WASM path ──
        if (this._wasm && this._wasm.isReady) {
            const p = this._prepareWasmData();
            const budgets32 = new Uint32Array(activeBlockIds.length);
            for (let i = 0; i < activeBlockIds.length; i++) {
                budgets32[i] = budget[activeBlockIds[i]];
            }
            const outBuf = new Uint32Array(config.maxSplats);
            const count = this._wasm.traverse(
                {
                    centers: p.centers,
                    featureSizes: p.featureSizes,
                    childStarts: p.childStarts,
                    childCounts: p.childCounts,
                    blockOffsets: p.blockOffsets,
                    blockCounts: p.blockCounts,
                    budgets: budgets32,
                    totalNodes: p.totalNodes,
                    numBlocks: activeBlockIds.length,
                    camX,
                    camY,
                    camZ,
                    fwdX,
                    fwdY,
                    fwdZ,
                    lodScale,
                    pixelScaleLimit: limit,
                    maxSplats: config.maxSplats,
                },
                outBuf,
            );

            if (this._btLog++ % 10 === 0) {
                console.log(`[bt] wasm traverse: ${activeBlockIds.length} blocks, ${count} GS`);
            }
            if (count === 0) return { order: new Uint32Array(0), count: 0 };

            const fullCenters = this._buildFullCenters();
            const sorted = depthSort(outBuf, count, fullCenters, camX, camY, camZ, fwdX, fwdY, fwdZ);
            return { order: sorted, count };
        }

        // ── JS fallback path ──
        if (this._btLog++ % 10 === 0)
            console.log('[bt] JS fallback', { wasm: !!this._wasm, ready: this._wasm?.isReady });
        interface HEntry {
            ps: number;
            nodeIdx: number;
            blockIdx: number;
        }
        const heap: HEntry[] = [];
        const heapPush = (e: HEntry) => {
            let i = heap.push(e) - 1;
            while (i > 0) {
                const p = (i - 1) >> 1;
                if (heap[p].ps >= heap[i].ps) break;
                [heap[p], heap[i]] = [heap[i], heap[p]];
                i = p;
            }
        };
        const heapPop = (): HEntry | undefined => {
            if (heap.length === 0) return undefined;
            const top = heap[0];
            const last = heap.pop()!;
            if (heap.length > 0) {
                heap[0] = last;
                let i = 0;
                const n = heap.length;
                while (true) {
                    let largest = i;
                    const l = (i << 1) | 1,
                        r = l + 1;
                    if (l < n && heap[l].ps > heap[largest].ps) largest = l;
                    if (r < n && heap[r].ps > heap[largest].ps) largest = r;
                    if (largest === i) break;
                    [heap[i], heap[largest]] = [heap[largest], heap[i]];
                    i = largest;
                }
            }
            return top;
        };

        // Push all roots
        for (const blockId of activeBlockIds) {
            const td = this.treeDataCache[blockId];
            if (!td || td.totalNodes === 0) continue;
            const ri = td.totalNodes - 1;
            const ps = computePixelScale(
                td.centers[ri * 3],
                td.centers[ri * 3 + 1],
                td.centers[ri * 3 + 2],
                td.featureSizes[ri],
                camX,
                camY,
                camZ,
                fwdX,
                fwdY,
                fwdZ,
                lodScale,
                foveated,
            );
            heapPush({ ps, nodeIdx: ri, blockIdx: blockId });
        }

        const maxOut = config.maxSplats;
        const outBuf = new Uint32Array(maxOut);
        let outCount = 0;

        while (heap.length > 0 && outCount < maxOut) {
            const entry = heapPop()!;
            const bidx = entry.blockIdx;

            // Budget exhausted for this block → skip node
            if (spent[bidx] >= budget[bidx]) continue;
            spent[bidx]++;

            const td = this.treeDataCache[bidx]!;
            const cnt = td.childCount[entry.nodeIdx];

            if (cnt === 0) {
                // Leaf → output
                outBuf[outCount++] = entry.nodeIdx + this.nodeOffsets[bidx];
                continue;
            }

            // Can't expand within budget → output as LOD
            if (spent[bidx] + cnt > budget[bidx]) {
                outBuf[outCount++] = entry.nodeIdx + this.nodeOffsets[bidx];
                continue;
            }

            // Expand children
            const start = td.childStart[entry.nodeIdx];
            for (let c = 0; c < cnt; c++) {
                const ci = start + c;
                const co = ci * 3;
                const ps = computePixelScale(
                    td.centers[co],
                    td.centers[co + 1],
                    td.centers[co + 2],
                    td.featureSizes[ci],
                    camX,
                    camY,
                    camZ,
                    fwdX,
                    fwdY,
                    fwdZ,
                    lodScale,
                    foveated,
                );
                if (ps <= limit) {
                    outBuf[outCount++] = ci + this.nodeOffsets[bidx];
                    if (outCount >= maxOut) break;
                } else {
                    heapPush({ ps, nodeIdx: ci, blockIdx: bidx });
                }
            }
        }

        if (outCount === 0) return { order: new Uint32Array(0), count: 0 };

        const fullCenters = this._buildFullCenters();
        const sorted = depthSort(outBuf, outCount, fullCenters, camX, camY, camZ, fwdX, fwdY, fwdZ);

        if (this._btLog++ % 10 === 0) {
            console.log(`[bt] traverse: ${numBlocks} blocks, ${outCount} GS out of ${config.maxSplats} budget`);
        }
        return { order: sorted, count: outCount };
    }

    /** Build flat arrays for WASM traversal (cached). */
    private _wasmData: WasmTraverseParams | null = null;
    private _prepareWasmData(): WasmTraverseParams {
        if (this._wasmData) return this._wasmData;
        const n = this.totalNodes;
        const nb = this.treeDataCache.length;
        const centers = new Float32Array(n * 3);
        const featureSizes = new Float32Array(n);
        const childStarts = new Uint32Array(n);
        const childCounts = new Uint16Array(n);
        const blockOffsets = new Uint32Array(nb);
        const blockCounts = new Uint32Array(nb);

        let off = 0;
        for (let bi = 0; bi < nb; bi++) {
            const td = this.treeDataCache[bi];
            if (!td) {
                blockOffsets[bi] = off;
                continue;
            }
            blockOffsets[bi] = off;
            blockCounts[bi] = td.totalNodes;
            for (let i = 0; i < td.totalNodes; i++) {
                const gi = off + i;
                centers[gi * 3] = td.centers[i * 3];
                centers[gi * 3 + 1] = td.centers[i * 3 + 1];
                centers[gi * 3 + 2] = td.centers[i * 3 + 2];
                featureSizes[gi] = td.featureSizes[i];
                childStarts[gi] = td.childStart[i] + (td.childCount[i] > 0 ? off : 0);
                childCounts[gi] = td.childCount[i];
            }
            off += td.totalNodes;
        }

        this._wasmData = {
            centers,
            featureSizes,
            childStarts,
            childCounts,
            blockOffsets,
            blockCounts,
            budgets: new Uint32Array(0),
            totalNodes: n,
            numBlocks: nb,
            camX: 0,
            camY: 0,
            camZ: 0,
            fwdX: 0,
            fwdY: 0,
            fwdZ: 0,
            lodScale: 0,
            pixelScaleLimit: 0,
            maxSplats: 0,
        };
        return this._wasmData;
    }

    private _buildFullCenters(): Float32Array {
        if (this._cachedCenters) return this._cachedCenters;
        if (this.centerChunks.length === 0) return new Float32Array(0);
        if (this.centerChunks.length === 1) {
            this._cachedCenters = this.centerChunks[0];
            return this._cachedCenters;
        }
        let totalLen = 0;
        for (const c of this.centerChunks) totalLen += c.length;
        const full = new Float32Array(totalLen);
        let off = 0;
        for (const c of this.centerChunks) {
            full.set(c, off);
            off += c.length;
        }
        this._cachedCenters = full;
        return full;
    }
}
