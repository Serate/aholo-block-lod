import { Vector3 } from '@qunhe/egs';

/**
 * Metadata for one block from lod-meta.json.
 */
export interface BlockMeta {
    bound: {
        min: [number, number, number];
        max: [number, number, number];
    };
    file: number;
    count: number;
}

/**
 * Runtime state of a single block.
 */
export enum BlockState {
    Free = 0,
    Loading,
    Active,
    Fading,
}

/**
 * A single block tracked by the BlockManager.
 */
export interface BlockHandle {
    id: number;
    meta: BlockMeta;
    state: BlockState;

    /** World-space bounding box center. */
    center: Vector3;
    /** AABB diagonal length. */
    diagonal: number;

    /** Remaining fade frames. */
    fadeFrames: number;
}

/**
 * Configuration for BlockManager.
 */
export interface BlockManagerConfig {
    /** Max concurrently active blocks. Default 4. */
    maxActiveBlocks?: number;
    /** Preload distance in units of diagonal. Default 2.0. */
    preloadDist?: number;
    /** Eviction distance in units of diagonal. Default 2.5. */
    evictDist?: number;
    /** Fade-out frame count. Default 8. */
    fadeFrames?: number;
}

const DEFAULT_CONFIG: Required<BlockManagerConfig> = {
    maxActiveBlocks: 4,
    preloadDist: 2.0,
    evictDist: 2.5,
    fadeFrames: 8,
};

/**
 * Manages the lifecycle of LOD blocks using distance-based heuristics.
 *
 * State machine:
 *   Free → Loading → Active → Fading → Free
 *
 * Distance rule:
 *   dist ≤ preloadDist × diagonal  → load
 *   dist >  evictDist  × diagonal  → unload (fade then free)
 *
 * The caller is responsible for providing the camera position each frame.
 * Frustum culling is handled by the tree traversal at a per-node level.
 */
export class BlockManager {
    private config: Required<BlockManagerConfig>;

    /** All known blocks. Indexed by block file index. */
    readonly blocks: BlockHandle[] = [];

    /** Callbacks for lifecycle events. */
    onBlockLoad?: (blockId: number) => void;
    onBlockFree?: (blockId: number) => void;

    constructor(config?: BlockManagerConfig) {
        this.config = { ...DEFAULT_CONFIG, ...config };
    }

    /** Initialize with block metadata from lod-meta.json. */
    init(metaBlocks: BlockMeta[]): void {
        this.blocks.length = 0;
        for (let i = 0; i < metaBlocks.length; i++) {
            const m = metaBlocks[i];
            const min = new Vector3(m.bound.min[0], m.bound.min[1], m.bound.min[2]);
            const max = new Vector3(m.bound.max[0], m.bound.max[1], m.bound.max[2]);
            const center = new Vector3((min.x + max.x) * 0.5, (min.y + max.y) * 0.5, (min.z + max.z) * 0.5);
            const dx = max.x - min.x,
                dy = max.y - min.y,
                dz = max.z - min.z;
            const diag = Math.sqrt(dx * dx + dy * dy + dz * dz);
            this.blocks.push({
                id: i,
                meta: m,
                state: BlockState.Free,
                center,
                diagonal: diag,
                fadeFrames: 0,
            });
        }
    }

    /**
     * Per-frame update. Returns block IDs that are Active this frame.
     */
    update(cameraPos: Vector3): number[] {
        const { blocks, config } = this;
        const { preloadDist, evictDist, maxActiveBlocks, fadeFrames } = config;

        // Phase 1: evaluate all blocks
        const active: { id: number; dist: number }[] = [];
        let activeCount = 0;

        for (let i = 0; i < blocks.length; i++) {
            const block = blocks[i];
            const dist = cameraPos.distanceTo(block.center);
            const threshold = block.diagonal;

            switch (block.state) {
                case BlockState.Free:
                    if (dist <= preloadDist * threshold) {
                        block.state = BlockState.Loading;
                        this.onBlockLoad?.(i);
                    }
                    break;

                case BlockState.Loading:
                    // Await markLoaded() from caller
                    break;

                case BlockState.Active:
                    activeCount++;
                    if (dist > evictDist * threshold) {
                        block.state = BlockState.Fading;
                        block.fadeFrames = fadeFrames;
                    } else {
                        active.push({ id: i, dist });
                    }
                    break;

                case BlockState.Fading:
                    block.fadeFrames--;
                    if (block.fadeFrames <= 0) {
                        block.state = BlockState.Free;
                        this.onBlockFree?.(i);
                    } else if (dist <= preloadDist * threshold) {
                        // Came back into range → reactivate
                        block.state = BlockState.Active;
                        active.push({ id: i, dist });
                    }
                    break;
            }
        }

        // Phase 2: cap active count — evict farthest if over limit
        if (activeCount > maxActiveBlocks) {
            const sorted = blocks
                .map((b, i) => ({ id: i, dist: cameraPos.distanceTo(b.center), state: b.state }))
                .filter(b => b.state === BlockState.Active)
                .sort((a, b) => b.dist - a.dist);

            const toEvict = activeCount - maxActiveBlocks;
            for (let i = 0; i < toEvict && i < sorted.length; i++) {
                const block = blocks[sorted[i].id];
                block.state = BlockState.Fading;
                block.fadeFrames = fadeFrames;
                // Remove from active list
                const idx = active.findIndex(a => a.id === sorted[i].id);
                if (idx >= 0) active.splice(idx, 1);
            }
        }

        // Return active block IDs sorted near-first
        active.sort((a, b) => a.dist - b.dist);
        return active.map(a => a.id);
    }

    /** Mark a loading block as active (call after async decode). */
    markLoaded(blockId: number): void {
        const block = this.blocks[blockId];
        if (block?.state === BlockState.Loading) {
            block.state = BlockState.Active;
        }
    }

    /** Mark a block as free after a failed load. */
    markFailed(blockId: number): void {
        const block = this.blocks[blockId];
        if (block?.state === BlockState.Loading) {
            block.state = BlockState.Free;
        }
    }

    /** Immediately free a block (no fade). */
    forceFree(blockId: number): void {
        const block = this.blocks[blockId];
        if (!block) return;
        block.state = BlockState.Free;
        block.fadeFrames = 0;
        this.onBlockFree?.(blockId);
    }
}
