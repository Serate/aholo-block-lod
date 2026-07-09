/**
 * Decoded block data from a .rad file, as returned by native.decodeRad.
 */
export interface DecodedBlock {
    /** GS count (leaf nodes). */
    count: number;
    /** Total tree nodes (leaves + internal). */
    totalNodes: number;
    /** SH degree. */
    shDegree: number;

    /** Tree structure: u32 × totalNodes, childStart[i] = index of first child */
    childStart: Uint32Array;
    /** Tree structure: u16 × totalNodes, childCount[i] = number of children (0 = leaf) */
    childCount: Uint16Array;

    /** GS center positions in f16[3] × totalNodes. */
    center: Uint16Array;
    /** GS RGBA in u8[4] × totalNodes. */
    rgba: Uint8Array;
    /** GS scale in u8[3] × totalNodes. */
    scale: Uint8Array;
    /** GS quaternion in u8[3] × totalNodes (octahedral encoding). */
    quat: Uint8Array;
    /** Optional SH coefficients. */
    sh?: Uint8Array;
}

/**
 * Holds per-block decoded tree data and provides per-frame traversal.
 *
 * Each frame, for each active block, calls native.traverseBlock to get
 * the LOD-selected indices, which are merged into a global order array.
 */
export class BlockTree {
    /** Decoded tree data for each block. */
    blocks: DecodedBlock[] = [];

    /** Register decoded block data. */
    addBlock(data: DecodedBlock): void {
        this.blocks.push(data);
    }

    /** Clear all block data. */
    clear(): void {
        this.blocks.length = 0;
    }

    /**
     * Traverse active blocks and produce a merged ordering array.
     *
     * @param activeBlockIds  Block IDs to traverse (from BlockManager).
     * @param cameraPos       Camera position [x, y, z].
     * @param lodScale        LOD scale factor.
     * @param pixelScaleLimit Pixel scale threshold.
     * @param maxSplats       Max GS output per block.
     * @param traverseFn      Native traverseBlock function reference.
     * @returns Merged Uint32Array of paged_indices for all traversed blocks.
     */
    traverse(
        activeBlockIds: number[],
        cameraPos: Float32Array,
        lodScale: number,
        pixelScaleLimit: number,
        maxSplats: number,
        traverseFn: (
            childStart: Uint32Array,
            childCount: Uint16Array,
            center: Uint16Array,
            size: Uint16Array,
            cameraPos: Float32Array,
            maxSplats: number,
            lodScale: number,
            pixelScaleLimit: number,
        ) => { indices: Uint32Array; numSplats: number },
    ): { order: Uint32Array; totalSplats: number } {
        const allIndices: Uint32Array[] = [];
        let total = 0;

        for (const blockId of activeBlockIds) {
            const block = this.blocks[blockId];
            if (!block) continue;

            const result = traverseFn(
                block.childStart,
                block.childCount,
                block.center,
                new Uint16Array(block.totalNodes), // size f16 placeholder
                cameraPos,
                maxSplats,
                lodScale,
                pixelScaleLimit,
            );

            if (result.numSplats > 0) {
                allIndices.push(result.indices.subarray(0, result.numSplats));
                total += result.numSplats;
            }
        }

        // Merge into single order array
        const order = new Uint32Array(total);
        let offset = 0;
        for (const indices of allIndices) {
            order.set(indices, offset);
            offset += indices.length;
        }

        return { order, totalSplats: total };
    }
}
