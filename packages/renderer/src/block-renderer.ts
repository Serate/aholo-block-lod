import { Vector3, __INTERNAL__, PerspectiveCamera } from '@qunhe/egs';
import { BlockManager, type BlockManagerConfig } from './block-manager.js';
import { SharedTexturePool } from './shared-texture-pool.js';
import { BlockTree } from './block-tree.js';
import type { TraverseConfig, FoveatedConfig } from './traverse-block.js';

/**
 * Coordinates LOD block rendering: loads .rad data, manages block lifecycle,
 * traverses active blocks each frame, and feeds the result into the
 * SplattingPlugin as the rendering order.
 *
 * Usage:
 *   const renderer = new BlockLodRenderer();
 *
 *   // Each frame, BEFORE SplattingPlugin.updateEffect():
 *   renderer.update(camera);
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

    /** Number of GS selected in the last update (for UI display). */
    lastSelectedCount = 0;

    /**
     * Per-frame update. Call BEFORE SplattingPlugin.updateEffect().
     * Uses the global SplattingPlugin reference if plugin not specified.
     *
     * @param camera  PerspectiveCamera for position, forward direction, and FOV.
     * @param splattingPlugin  Optional explicit plugin reference.
     * @param maxSplats  Global GS budget for this frame. Default 500000.
     * @param lodScale  LOD scale factor. Default 1.0.
     * @param pixelScaleLimit  Minimum pixel scale threshold. Default 0.001.
     * @param foveated  Optional foveated rendering config.
     * @param dynamicMode  Enable dynamic budget mode. Default true.
     */
    private _logThrottle = 0;
    update(
        camera: PerspectiveCamera,
        splattingPlugin?: __INTERNAL__.SplattingPlugin,
        maxSplats = 500000,
        lodScale = 1.0,
        pixelScaleLimit = 0.001,
        foveated?: Partial<FoveatedConfig>,
        dynamicMode = true,
    ): void {
        const plugin = splattingPlugin ?? __INTERNAL__.getSplattingPlugin();
        if (!plugin) {
            if (this._logThrottle++ % 30 === 0) console.warn('[LOD] plugin undefined');
            return;
        }

        this.texturePool.newFrame();

        const cameraPos = camera.position;
        const activeBlockIds = this.blockManager.update(cameraPos);

        if (activeBlockIds.length === 0) {
            if (this._logThrottle++ % 30 === 0) console.warn('[LOD] no active blocks');
            plugin.setExternalOrder(new Uint32Array(0), 0);
            return;
        }

        // Extract camera world-space forward direction
        const forward = new Vector3();
        camera.getWorldDirection(forward);

        const camPos: [number, number, number] = [cameraPos.x, cameraPos.y, cameraPos.z];
        const camFwd: [number, number, number] = [forward.x, forward.y, forward.z];

        const config: TraverseConfig = {
            cameraPos: camPos,
            cameraForward: camFwd,
            lodScale,
            pixelScaleLimit,
            maxSplats,
            foveated,
            dynamicMode,
        };

        const { order, count } = this.blockTree.traverse(activeBlockIds, config);
        this.lastSelectedCount = count;

        if (count > 0) {
            plugin.setExternalOrder(order, count);
        } else if (this._logThrottle++ % 30 === 0) {
            console.warn('[LOD] traverse 0 for', activeBlockIds.length, 'blocks');
        }
    }
}
