import { type TreeData, type TraverseConfig, traverseBlock, prepareTreeData, depthSort } from './traverse-block.js';

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
 *
 * Each block's TreeData uses LOCAL indices (0..totalNodes-1 within the block).
 * Output indices from traverse() are offset to GLOBAL indices via nodeOffset,
 * matching the merged SplatData layout for setExternalOrder.
 */
export class BlockTree {
    blocks: DecodedBlock[] = [];
    private treeDataCache: (TreeData | null)[] = [];

    /** Global index offset for each block's first node in the merged SplatData. */
    private nodeOffsets: number[] = [];
    /** Accumulated centers chunks for depthSort (global). */
    private centerChunks: Float32Array[] = [];
    /** Total nodes across all registered blocks. */
    totalNodes: number = 0;

    /**
     * Register a decoded block.
     * @param data  Decoded block data from decodeRad().
     * @param nodeOffset  Global index of this block's first node in the merged SplatData.
     */
    addBlock(data: DecodedBlock, nodeOffset: number): void {
        this.blocks.push(data);
        this.nodeOffsets.push(nodeOffset);

        // TreeData stores LOCAL childStart/centers (within this block)
        const td = prepareTreeData(data.center, data.scale, data.childStart, data.childCount, data.totalNodes);
        this.treeDataCache.push(td);

        // Accumulate centers for global depthSort
        this.centerChunks.push(td.centers);
        this.totalNodes += td.totalNodes;
    }

    clear(): void {
        this.blocks.length = 0;
        this.treeDataCache.length = 0;
        this.nodeOffsets.length = 0;
        this.centerChunks.length = 0;
        this.totalNodes = 0;
    }

    /**
     * Traverse active blocks and produce a merged, depth-sorted ordering array.
     *
     * Each block receives an equal share of the maxSplats budget.
     * Output indices are global (zero-based across the merged SplatData)
     * and sorted far→near for correct transparency blending.
     */
    traverse(activeBlockIds: number[], config: TraverseConfig): { order: Uint32Array; count: number } {
        if (activeBlockIds.length === 0 || config.maxSplats === 0) {
            return { order: new Uint32Array(0), count: 0 };
        }

        // Collect per-block results (local indices)
        const allChunks: Uint32Array[] = [];
        let total = 0;
        const numBlocks = Math.max(1, activeBlockIds.length);
        const perBlockBudget = Math.ceil(config.maxSplats / numBlocks);

        for (const blockId of activeBlockIds) {
            const td = this.treeDataCache[blockId];
            if (!td) {
                console.warn('[bt] no TreeData for block', blockId);
                continue;
            }

            const budget = Math.min(perBlockBudget, td.totalNodes);
            const outputIndices = new Uint32Array(budget);
            const blockConfig = { ...config, maxSplats: budget };
            const t0 = performance.now();
            const count = traverseBlock(td, blockConfig, outputIndices);
            const dt = performance.now() - t0;
            console.log(`[bt] block ${blockId}: budget=${budget} count=${count} dt=${dt.toFixed(0)}ms`);

            if (count > 0) {
                // Offset local indices to global
                const offset = this.nodeOffsets[blockId] ?? 0;
                if (offset > 0) {
                    for (let i = 0; i < count; i++) {
                        outputIndices[i] += offset;
                    }
                }
                allChunks.push(outputIndices.subarray(0, count));
                total += count;
            }
        }

        if (total === 0) {
            return { order: new Uint32Array(0), count: 0 };
        }

        // Merge all chunk indices into one contiguous array
        const merged = new Uint32Array(total);
        let offset = 0;
        for (const chunk of allChunks) {
            merged.set(chunk, offset);
            offset += chunk.length;
        }

        // Build global centers array for depthSort
        const fullCenters = this._buildFullCenters();

        // Depth-sort far→near
        const [camX, camY, camZ] = config.cameraPos;
        const [fwdX, fwdY, fwdZ] = config.cameraForward;
        const sorted = depthSort(merged, total, fullCenters, camX, camY, camZ, fwdX, fwdY, fwdZ);

        return { order: sorted, count: total };
    }

    /** Concatenate all center chunks into one Float32Array for depth sorting. */
    private _buildFullCenters(): Float32Array {
        if (this.centerChunks.length === 0) return new Float32Array(0);
        if (this.centerChunks.length === 1) return this.centerChunks[0];

        let totalLen = 0;
        for (const c of this.centerChunks) totalLen += c.length;
        const full = new Float32Array(totalLen);
        let off = 0;
        for (const c of this.centerChunks) {
            full.set(c, off);
            off += c.length;
        }
        return full;
    }
}
