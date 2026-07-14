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

    /** Cached last traverse result to avoid re-traversal when camera is still. */
    private _lastOrder: Uint32Array | null = null;
    private _lastCount = 0;
    private _lastCamPos = new Vector3();
    private _lastCamFwd = new Vector3();

    /**
     * Optional mapping from .rad global index → leaf-only global index.
     * When set, traverse output indices are remapped before setExternalOrder.
     * radToLeaf[radIndex] = leafIndex. Set to 0xFFFFFFFF for internal nodes.
     */
    radToLeaf?: Uint32Array;

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
    /** Performance stats (throttled). */
    private _pf = { bm: 0, tr: 0, ds: 0, so: 0, count: 0 };
    update(
        camera: PerspectiveCamera,
        splattingPlugin?: __INTERNAL__.SplattingPlugin,
        maxSplats = 500000,
        lodScale = 1.0,
        pixelScaleLimit = 0.001,
        foveated?: Partial<FoveatedConfig>,
        dynamicMode = true,
    ): void {
        const t0 = performance.now();

        const plugin = splattingPlugin ?? __INTERNAL__.getSplattingPlugin();
        if (!plugin) {
            if (this._logThrottle++ % 30 === 0) console.warn('[LOD] plugin undefined');
            return;
        }

        this.texturePool.newFrame();

        const { blockIds: activeBlockIds, weights } = this.blockManager.update(camera);
        const t1 = performance.now();
        this._pf.bm += t1 - t0;

        const cameraPos = camera.position;

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

        // Skip traversal if camera hasn't moved significantly
        const camMoved =
            cameraPos.distanceToSquared(this._lastCamPos) > 0.0001 ||
            forward.distanceToSquared(this._lastCamFwd) > 0.0001;
        this._lastCamPos.copy(cameraPos);
        this._lastCamFwd.copy(forward);

        let order: Uint32Array;
        let count: number;
        if (camMoved || !this._lastOrder) {
            const result = this.blockTree.traverse(activeBlockIds, config, weights);
            order = result.order;
            count = result.count;
            this._lastOrder = order;
            this._lastCount = count;
        } else {
            order = this._lastOrder!;
            count = this._lastCount;
        }

        const t2 = performance.now();
        this._pf.tr += t2 - t1;
        this.lastSelectedCount = count;

        if (count > 0) {
            // Ensure order array is large enough for setExternalOrder's w*h requirement
            const n = Math.ceil(Math.sqrt(count));
            const w = Math.max(1, 1 << Math.ceil(Math.log2(n)));
            const h = Math.ceil(count / w);
            const bufSize = Math.max(count, w * h);
            const buf = order.length >= bufSize ? order : new Uint32Array(bufSize);
            if (buf !== order) buf.set(order.subarray(0, count));
            const t3 = performance.now();
            this._pf.ds += t3 - t2;

            if (this.radToLeaf) {
                const map = this.radToLeaf;
                let leafCount = 0;
                for (let i = 0; i < count; i++) {
                    const leafIdx = map[order[i]];
                    if (leafIdx !== 0xffffffff) {
                        buf[leafCount++] = leafIdx;
                    }
                }
                if (leafCount > 0) {
                    plugin.setExternalOrder(buf, leafCount);
                }
                this.lastSelectedCount = leafCount;
            } else {
                plugin.setExternalOrder(buf, count);
            }
            const t4 = performance.now();
            this._pf.so += t4 - t3;
            this._pf.count++;

            // Log every 15 frames
            if (this._pf.count >= 15) {
                const avg = (v: number) => (v / this._pf.count).toFixed(1);
                console.log(
                    `[perf] bm=${avg(this._pf.bm)}ms tr=${avg(this._pf.tr)}ms ds=${avg(this._pf.ds)}ms so=${avg(this._pf.so)}ms`,
                );
                this._pf.bm = this._pf.tr = this._pf.ds = this._pf.so = this._pf.count = 0;
            }
        } else if (this._logThrottle++ % 30 === 0) {
            console.warn('[LOD] traverse 0 for', activeBlockIds.length, 'blocks');
        }
    }
}
