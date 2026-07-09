import { Vector3, __INTERNAL__ } from '@qunhe/egs';
import { BlockManager, type BlockManagerConfig } from './block-manager.js';
import { SharedTexturePool } from './shared-texture-pool.js';
import { BlockTree } from './block-tree.js';

/**
 * Coordinates LOD block rendering: loads .rad data, manages block lifecycle,
 * traverses active blocks each frame, and feeds the result into the
 * SplattingPlugin as the rendering order.
 *
 * Usage:
 *   const renderer = new BlockLodRenderer();
 *   renderer.init(lodMeta, texturePool);  // from lod-meta.json
 *
 *   // Each frame, before SplattingPlugin.updateEffect():
 *   renderer.update(cameraPos, splattingPlugin);
 */
export class BlockLodRenderer {
    readonly blockManager: BlockManager;
    readonly blockTree = new BlockTree();
    readonly texturePool: SharedTexturePool;

    /** Native traverseBlock function (set after native module loads). */
    traverseFn?: (
        childStart: Uint32Array,
        childCount: Uint16Array,
        center: Uint16Array,
        size: Uint16Array,
        cameraPos: Float32Array,
        maxSplats: number,
        lodScale: number,
        pixelScaleLimit: number,
    ) => { indices: Uint32Array; numSplats: number };

    constructor(config?: BlockManagerConfig & { maxPoolPages?: number }) {
        this.blockManager = new BlockManager(config);
        this.texturePool = new SharedTexturePool(config?.maxPoolPages ?? 64);
    }

    /** Whether this renderer has active blocks. */
    get hasActiveBlocks(): boolean {
        return this.blockManager.blocks.length > 0;
    }

    /**
     * Per-frame update. Call BEFORE SplattingPlugin.updateEffect().
     *
     * @param cameraPos World-space camera position.
     * @param splattingPlugin The SplattingPlugin instance to set external order on.
     * @param lodScale LOD scale factor (default 1.0).
     * @param pixelScaleLimit Pixel scale threshold (default 0.001).
     * @param maxSplatsPerBlock Max GS output per block traversal (default 500000).
     */
    update(
        cameraPos: Vector3,
        splattingPlugin: __INTERNAL__.SplattingPlugin,
        lodScale = 1.0,
        pixelScaleLimit = 0.001,
        maxSplatsPerBlock = 500000,
    ): void {
        if (!this.traverseFn) return;

        this.texturePool.newFrame();

        // Phase 1: evaluate block lifecycle
        const activeBlockIds = this.blockManager.update(cameraPos);

        if (activeBlockIds.length === 0) {
            splattingPlugin.setExternalOrder(new Uint32Array(0), 0);
            return;
        }

        // Phase 2: traverse active blocks
        const camPosArray = new Float32Array([cameraPos.x, cameraPos.y, cameraPos.z]);
        const { order, totalSplats } = this.blockTree.traverse(
            activeBlockIds,
            camPosArray,
            lodScale,
            pixelScaleLimit,
            maxSplatsPerBlock,
            this.traverseFn,
        );

        // Phase 3: feed into SplattingPlugin
        splattingPlugin.setExternalOrder(order, totalSplats);
    }
}
