import { type TreeData, traverseBlock, computeFeatureSizes } from './traverse-block.js';
import { f16ToF32 } from './rad-decoder-browser.js';

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
 */
export class BlockTree {
    blocks: DecodedBlock[] = [];
    private treeDataCache: (TreeData | null)[] = [];

    addBlock(data: DecodedBlock): void {
        this.blocks.push(data);
        // Pre-compute traverse data (f32 centers + feature sizes)
        const totalNodes = data.totalNodes;
        const centers = new Float32Array(totalNodes * 3);
        for (let i = 0; i < totalNodes; i++) {
            centers[i * 3] = f16ToF32(data.center[i * 3]);
            centers[i * 3 + 1] = f16ToF32(data.center[i * 3 + 1]);
            centers[i * 3 + 2] = f16ToF32(data.center[i * 3 + 2]);
        }
        const featureSizes = computeFeatureSizes(data.scale, totalNodes);
        this.treeDataCache.push({
            totalNodes,
            childStart: data.childStart,
            childCount: data.childCount,
            centers,
            featureSizes,
        });
    }

    clear(): void {
        this.blocks.length = 0;
        this.treeDataCache.length = 0;
    }

    /**
     * Traverse active blocks and produce a merged ordering array.
     */
    traverse(
        activeBlockIds: number[],
        cameraPos: [number, number, number],
        lodScale: number,
        pixelScaleLimit: number,
        maxSplats: number,
    ): { order: Uint32Array; totalSplats: number } {
        const allIndices: Uint32Array[] = [];
        let total = 0;

        for (const blockId of activeBlockIds) {
            const td = this.treeDataCache[blockId];
            if (!td) continue;

            const result = traverseBlock(td, cameraPos, [0, 0, 0], lodScale, pixelScaleLimit, maxSplats);
            if (result.count > 0) {
                allIndices.push(result.indices.subarray(0, result.count));
                total += result.count;
            }
        }

        const order = new Uint32Array(total);
        let offset = 0;
        for (const indices of allIndices) {
            order.set(indices, offset);
            offset += indices.length;
        }

        return { order, totalSplats: total };
    }
}
