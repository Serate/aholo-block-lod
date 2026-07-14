/**
 * Minimal loader for the traverse-wasm module.
 * Uses WebAssembly.instantiate with streaming.
 */

// Layout of the WASM memory for our flat arrays.
// Block data is stored as contiguous typed arrays per block:
//   [block0_centers..., block1_centers..., ...]
//   (same for feature_sizes, child_starts, child_counts)
// The block_offsets array tells each block's start index.

export interface WasmTraverseParams {
    centers: Float32Array;
    featureSizes: Float32Array;
    childStarts: Uint32Array;
    childCounts: Uint16Array;
    blockOffsets: Uint32Array;
    blockCounts: Uint32Array;
    budgets: Uint32Array;
    totalNodes: number;
    numBlocks: number;
    camX: number;
    camY: number;
    camZ: number;
    fwdX: number;
    fwdY: number;
    fwdZ: number;
    lodScale: number;
    pixelScaleLimit: number;
    maxSplats: number;
}

export class WasmTraverser {
    private instance: WebAssembly.Instance | null = null;
    private _ready: Promise<void>;
    /** Cached WASM memory layout (set after first data upload). */
    private _memLayout: {
        offCenters: number;
        offFeatSizes: number;
        offChildStarts: number;
        offChildCounts: number;
        offBlockOffsets: number;
        offBlockCounts: number;
        offBudgets: number;
        offOut: number;
        totalBytes: number;
        memory: WebAssembly.Memory;
        centers: Float32Array;
        featureSizes: Float32Array;
        childStarts: Uint32Array;
        childCounts: Uint16Array;
        blockOffsets: Uint32Array;
        blockCounts: Uint32Array;
        budgets: Uint32Array;
        out: Uint32Array;
    } | null = null;

    constructor() {
        this._ready = this._init();
    }

    get ready(): Promise<void> {
        return this._ready;
    }

    get isReady(): boolean {
        return this.instance !== null;
    }

    private async _init(): Promise<void> {
        const response = await fetch('/wasm/traverse_wasm.wasm');
        const bytes = await response.arrayBuffer();
        const module = await WebAssembly.compile(bytes);
        // No imports needed for this minimalist module
        this.instance = await WebAssembly.instantiate(module, {});
    }

    /** Returns true if WASM loaded successfully. */
    get available(): boolean {
        return this.instance !== null;
    }

    /**
     * Run traversal. Returns selected index count.
     * Static data is uploaded to WASM memory once (first call).
     * Per-frame data (budgets) is uploaded each call.
     */
    traverse(params: WasmTraverseParams, outBuffer: Uint32Array): number {
        const inst = this.instance!;
        const {
            centers,
            featureSizes,
            childStarts,
            childCounts,
            blockOffsets,
            blockCounts,
            budgets,
            totalNodes,
            numBlocks,
        } = params;

        const memory = (inst.exports as any).memory as WebAssembly.Memory;
        const heapBase = ((inst.exports as any).__heap_base as number) || 0;
        const DATA_START = heapBase + 64; // 64 bytes padding after heap base

        // First call: allocate WASM memory and upload all static data
        if (!this._memLayout) {
            const align4 = (n: number) => (n + 3) & ~3;
            const offCenters = DATA_START;
            const szCenters = centers.byteLength;
            const offFeatSizes = align4(offCenters + szCenters);
            const szFeatSizes = featureSizes.byteLength;
            const offChildStarts = align4(offFeatSizes + szFeatSizes);
            const szChildStarts = childStarts.byteLength;
            const offChildCounts = align4(offChildStarts + szChildStarts);
            const szChildCounts = childCounts.byteLength;
            const offBlockOffsets = align4(offChildCounts + szChildCounts);
            const szBlockOffsets = blockOffsets.byteLength;
            const offBlockCounts = align4(offBlockOffsets + szBlockOffsets);
            const szBlockCounts = blockCounts.byteLength;
            const offBudgets = align4(offBlockCounts + szBlockCounts);
            const offOut = align4(offBudgets + budgets.byteLength);
            const totalBytes = offOut + outBuffer.byteLength * 4;

            // Grow WASM memory
            const wasmMem = new Uint8Array(memory.buffer);
            const needed = Math.ceil((totalBytes - wasmMem.length) / 65536);
            if (needed > 0) memory.grow(needed);

            // Upload static data
            const mem8 = new Uint8Array(memory.buffer);
            mem8.set(new Uint8Array(centers.buffer, centers.byteOffset, szCenters), offCenters);
            mem8.set(new Uint8Array(featureSizes.buffer, featureSizes.byteOffset, szFeatSizes), offFeatSizes);
            mem8.set(new Uint8Array(childStarts.buffer, childStarts.byteOffset, szChildStarts), offChildStarts);
            mem8.set(new Uint8Array(childCounts.buffer, childCounts.byteOffset, szChildCounts), offChildCounts);
            mem8.set(new Uint8Array(blockOffsets.buffer, blockOffsets.byteOffset, szBlockOffsets), offBlockOffsets);
            mem8.set(new Uint8Array(blockCounts.buffer, blockCounts.byteOffset, szBlockCounts), offBlockCounts);

            this._memLayout = {
                offCenters,
                offFeatSizes,
                offChildStarts,
                offChildCounts,
                offBlockOffsets,
                offBlockCounts,
                offBudgets,
                offOut,
                totalBytes,
                memory,
                centers,
                featureSizes,
                childStarts,
                childCounts,
                blockOffsets,
                blockCounts,
                budgets: new Uint32Array(0),
                out: new Uint32Array(0),
            };
        }

        const layout = this._memLayout;
        const mem8 = new Uint8Array(memory.buffer);

        // Upload per-frame budgets
        mem8.set(new Uint8Array(budgets.buffer, budgets.byteOffset, budgets.byteLength), layout.offBudgets);

        // Call traverse
        const traverseFn = (inst.exports as any).traverse as Function;
        const count = traverseFn(
            layout.offCenters,
            totalNodes,
            layout.offFeatSizes,
            layout.offChildStarts,
            layout.offChildCounts,
            layout.offBlockOffsets,
            numBlocks,
            layout.offBlockCounts,
            params.camX,
            params.camY,
            params.camZ,
            params.fwdX,
            params.fwdY,
            params.fwdZ,
            params.lodScale,
            params.pixelScaleLimit,
            params.maxSplats,
            layout.offBudgets,
            layout.offOut,
        ) as number;

        // Copy result back
        const resultView = new Uint32Array(memory.buffer, layout.offOut, count);
        outBuffer.set(resultView);
        return count;
    }
}
