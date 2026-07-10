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
 *   renderer.traverseFn = native.traverseBlock;
 *
 *   // Each frame:
 *   renderer.update(cameraPos);
 */
export class BlockLodRenderer {
    readonly blockManager: BlockManager;
    readonly blockTree = new BlockTree();
    readonly texturePool: SharedTexturePool;

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
     * Uses the global SplattingPlugin reference if plugin not specified.
     */
    update(
        cameraPos: Vector3,
        splattingPlugin?: __INTERNAL__.SplattingPlugin,
        lodScale = 1.0,
        pixelScaleLimit = 0.001,
        maxSplatsPerBlock = 500000,
    ): void {
        const plugin = splattingPlugin ?? __INTERNAL__.getSplattingPlugin();
        if (!plugin) return;

        this.texturePool.newFrame();

        const activeBlockIds = this.blockManager.update(cameraPos);

        if (activeBlockIds.length === 0) {
            plugin.setExternalOrder(new Uint32Array(0), 0);
            return;
        }

        const camPos: [number, number, number] = [cameraPos.x, cameraPos.y, cameraPos.z];
        const { order, totalSplats } = this.blockTree.traverse(
            activeBlockIds,
            camPos,
            lodScale,
            pixelScaleLimit,
            maxSplatsPerBlock,
        );

        plugin.setExternalOrder(order, totalSplats);
    }
}
