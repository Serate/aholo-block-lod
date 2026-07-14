import { Vector3, Frustum, Matrix4, PerspectiveCamera } from '@qunhe/egs';

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

/**
 * Manages LOD blocks with frustum-aware weighting (aholo-style).
 */
export class BlockManager {
    /** All known blocks. Indexed by block file index. */
    readonly blocks: BlockHandle[] = [];

    constructor(_config?: BlockManagerConfig) {}

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
     * Per-frame update with frustum-aware weighting (aholo-style).
     * Returns all block IDs sorted by priority (weight descending).
     */
    update(camera: PerspectiveCamera): { blockIds: number[]; weights: Float32Array } {
        const { blocks } = this;

        const frustum = new Frustum().setFromMatrix(
            new Matrix4().multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse),
        );
        const cameraPos = camera.position;
        const forward = new Vector3(0, 0, -1).applyQuaternion(camera.quaternion);
        const temp = new Vector3();
        const tempMin = new Vector3();
        const tempMax = new Vector3();
        const weighted: { id: number; weight: number }[] = [];

        for (let i = 0; i < blocks.length; i++) {
            const block = blocks[i];
            tempMin.set(block.meta.bound.min[0], block.meta.bound.min[1], block.meta.bound.min[2]);
            tempMax.set(block.meta.bound.max[0], block.meta.bound.max[1], block.meta.bound.max[2]);
            const closest = temp.copy(cameraPos).clamp(tempMin, tempMax);
            const insideBox =
                cameraPos.x >= tempMin.x &&
                cameraPos.x <= tempMax.x &&
                cameraPos.y >= tempMin.y &&
                cameraPos.y <= tempMax.y &&
                cameraPos.z >= tempMin.z &&
                cameraPos.z <= tempMax.z;
            const dist = insideBox ? 0 : cameraPos.distanceTo(closest);
            const isInside = frustum.intersectsBox({ min: tempMin, max: tempMax } as any);
            const dirTo = temp.copy(closest).sub(cameraPos).normalize();
            const isBehind = !insideBox && forward.dot(dirTo) < -0.2 && dist > 2;

            const weight = (1 / (1 + 0.1 * dist * dist)) * (isInside ? 1 : 0.4) * (isBehind ? 0.1 : 1);
            block.state = BlockState.Active;
            weighted.push({ id: i, weight });
        }

        weighted.sort((a, b) => b.weight - a.weight);
        return { blockIds: weighted.map(w => w.id), weights: new Float32Array(weighted.map(w => w.weight)) };
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
    }
}
