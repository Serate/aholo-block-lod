/**
 * TypeScript port of C++ traverseBlock (lod_tree.cpp).
 *
 * BFS traversal of the cycling_lod tree with pixel_scale-based LOD selection.
 *
 * Each frame:
 *   traverseBlock(treeData, config, output)
 *   → returns number of selected GS indices for rendering via setExternalOrder.
 */

// ── Heap entry ──

interface HeapEntry {
    pixelScale: number;
    nodeIndex: number;
}

/**
 * Binary max-heap (by pixelScale) for LOD tree traversal.
 */
class MaxHeap {
    private data: HeapEntry[] = [];
    private _size = 0;

    get size(): number {
        return this._size;
    }

    push(entry: HeapEntry): void {
        let i = this._size++;
        this.data[i] = entry;
        // sift up
        while (i > 0) {
            const parent = (i - 1) >> 1;
            if (this.data[parent].pixelScale >= this.data[i].pixelScale) break;
            [this.data[parent], this.data[i]] = [this.data[i], this.data[parent]];
            i = parent;
        }
    }

    pop(): HeapEntry | undefined {
        if (this._size === 0) return undefined;
        const top = this.data[0];
        const last = --this._size;
        if (last > 0) {
            this.data[0] = this.data[last];
            let i = 0;
            while (true) {
                let largest = i;
                const left = (i << 1) | 1;
                const right = left + 1;
                if (left < this._size && this.data[left].pixelScale > this.data[largest].pixelScale) largest = left;
                if (right < this._size && this.data[right].pixelScale > this.data[largest].pixelScale) largest = right;
                if (largest === i) break;
                [this.data[i], this.data[largest]] = [this.data[largest], this.data[i]];
                i = largest;
            }
        }
        return top;
    }

    /** Peek at top entry without removing. */
    peek(): HeapEntry | undefined {
        return this._size > 0 ? this.data[0] : undefined;
    }

    clear(): void {
        this._size = 0;
    }
}

// ── Public API ──

/**
 * Pre-computed tree data needed for traversal.
 * Populated once when loading a .rad file.
 */
export interface TreeData {
    totalNodes: number;
    /** childStart[i] = index of first child */
    childStart: Uint32Array;
    /** childCount[i] = number of children (0 = leaf) */
    childCount: Uint16Array;
    /** Node centers as f32[totalNodes * 3] (decoded from RAD f16) */
    centers: Float32Array;
    /** Feature size per node: max(scale_x, scale_y, scale_z) * 2, as f32 */
    featureSizes: Float32Array;
}

/** Configuration for LOD tree traversal. */
export interface TraverseConfig {
    cameraPos: [number, number, number];
    cameraForward: [number, number, number];
    lodScale: number;
    pixelScaleLimit: number;
    maxSplats: number;
    foveated?: Partial<FoveatedConfig>;
    /** Enable dynamic budget mode. Default false. */
    dynamicMode?: boolean;
}

export interface TraverseResult {
    /** Output indices ordered by LOD traversal */
    indices: Uint32Array;
    /** Number of valid entries in indices */
    count: number;
}

/** Foveated rendering configuration for computePixelScale. */
export interface FoveatedConfig {
    /** Center fovea half-angle in degrees. Inside this cone, no attenuation. Default 5. */
    coneFov0: number;
    /** Periphery half-angle in degrees. Outside this cone, full behindFoveate attenuation. Default 30. */
    coneFov: number;
    /** Attenuation factor for behind-camera and far-periphery GS. Default 0.1. */
    behindFoveate: number;
}

// Reusable temp arrays to avoid per-frame allocation in hot loop
const heap = new MaxHeap();

/**
 * Compute pixel_scale for a node with optional foveated attenuation.
 *
 * pixel_scale = (featureSize / distance) * lodScale * coneFactor * behindFactor
 *
 * coneFactor:
 *   angle < coneFov0 (center fovea):    1.0
 *   coneFov0 < angle < coneFov (transition):  1.0 → behindFoveate linear
 *   angle > coneFov (periphery):              behindFoveate
 *
 * behindFactor:
 *   camera front (dot > 0):  1.0
 *   camera back  (dot < 0):  behindFoveate
 */
export function computePixelScale(
    cx: number,
    cy: number,
    cz: number,
    featureSize: number,
    camX: number,
    camY: number,
    camZ: number,
    forwardX: number,
    forwardY: number,
    forwardZ: number,
    lodScale: number,
    foveated?: Partial<FoveatedConfig>,
): number {
    const dx = cx - camX;
    const dy = cy - camY;
    const dz = cz - camZ;
    const dist = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-6) return (featureSize / 1e-6) * lodScale;

    let scale = (featureSize / dist) * lodScale;

    // Behind-camera attenuation
    const dotFwd = dx * forwardX + dy * forwardY + dz * forwardZ;
    if (dotFwd < 0) {
        const behindFoveate = foveated?.behindFoveate ?? 0.1;
        scale *= behindFoveate;
        // Skip cone check for behind-camera GS (already fully attenuated)
        return scale;
    }

    // Foveated cone attenuation
    if (foveated) {
        const coneFov0 = foveated.coneFov0 ?? 5;
        const coneFov = foveated.coneFov ?? 30;
        const behindFoveate = foveated.behindFoveate ?? 0.1;

        // Angle between view direction and GS direction
        const cosAngle = dotFwd / dist;
        const angle = Math.acos(Math.min(1, Math.max(-1, cosAngle))) * (180 / Math.PI);

        if (angle > coneFov0) {
            if (angle >= coneFov) {
                scale *= behindFoveate;
            } else {
                // Linear falloff: 1.0 → behindFoveate
                const t = (angle - coneFov0) / (coneFov - coneFov0);
                scale *= 1 - t + t * behindFoveate;
            }
        }
    }

    return scale;
}

// Module-level cache for dynamic mode's limit across frames
let _lastDynamicLimit = 0;
let _tbLog = 0;

/**
 * Internal: BFS traversal with a given pixelScaleLimit.
 *
 * Algorithm:
 * 1. Push root (totalNodes - 1) with its pixel_scale
 * 2. While heap not empty and output < maxSplats:
 *    - Pop highest pixel_scale entry
 *    - If pixel_scale <= limit: output node
 *    - Else: expand children, push/ output based on pixel_scale vs limit
 * 3. Flush remaining heap entries as output
 *
 * Returns number of indices written to outputIndices.
 */
function _standardTraverse(
    tree: TreeData,
    pixelScaleLimit: number,
    maxSplats: number,
    config: TraverseConfig,
    outputIndices: Uint32Array,
): number {
    const { totalNodes, childStart, childCount, centers, featureSizes } = tree;
    if (totalNodes === 0 || maxSplats === 0) return 0;

    const [camX, camY, camZ] = config.cameraPos;
    const [fwdX, fwdY, fwdZ] = config.cameraForward;
    const foveated = config.foveated;
    const lodScale = config.lodScale;
    let outputCount = 0;

    heap.clear();

    // Push root (always at totalNodes - 1 in level-morton ordering)
    const rootIdx = totalNodes - 1;
    const rootPs = computePixelScale(
        centers[rootIdx * 3],
        centers[rootIdx * 3 + 1],
        centers[rootIdx * 3 + 2],
        featureSizes[rootIdx],
        camX,
        camY,
        camZ,
        fwdX,
        fwdY,
        fwdZ,
        lodScale,
        foveated,
    );
    if (!isFinite(rootPs) || rootPs <= 0) {
        console.warn('[traverse] root invalid:', {
            block: tree.totalNodes,
            rootIdx,
            rootPs,
            fs: featureSizes[rootIdx],
            cx: centers[rootIdx * 3],
            cy: centers[rootIdx * 3 + 1],
            cz: centers[rootIdx * 3 + 2],
        });
    } else if (rootPs < 0.01) {
        console.warn('[traverse] root too small:', { rootPs, fs: featureSizes[rootIdx], limit: pixelScaleLimit });
    }
    heap.push({ pixelScale: rootPs, nodeIndex: rootIdx });
    let numSplats = 1;

    while (heap.size > 0 && outputCount < maxSplats) {
        const entry = heap.peek()!;
        if (entry.pixelScale <= pixelScaleLimit) break;

        const idx = entry.nodeIndex;
        const cnt = childCount[idx];

        if (cnt === 0) {
            // Leaf → output
            heap.pop();
            outputIndices[outputCount++] = idx;
            continue;
        }

        const start = childStart[idx];
        const newTotal = numSplats - 1 + cnt;
        if (newTotal > maxSplats) {
            // Can't expand → output this node as LOD representation
            heap.pop();
            outputIndices[outputCount++] = idx;
            numSplats--; // node removed from heap without expansion
            if (outputCount >= maxSplats) return outputCount;
            continue;
        }

        heap.pop(); // confirm expansion

        for (let c = 0; c < cnt; c++) {
            const childIdx = start + c;
            const co = childIdx * 3;
            const ps = computePixelScale(
                centers[co],
                centers[co + 1],
                centers[co + 2],
                featureSizes[childIdx],
                camX,
                camY,
                camZ,
                fwdX,
                fwdY,
                fwdZ,
                lodScale,
                foveated,
            );

            if (ps <= pixelScaleLimit) {
                outputIndices[outputCount++] = childIdx;
                if (outputCount >= maxSplats) {
                    numSplats = newTotal;
                    return outputCount;
                }
            } else {
                heap.push({ pixelScale: ps, nodeIndex: childIdx });
            }
        }
        numSplats = newTotal;
    }

    // Flush: output all remaining entries (leaf + internal = LOD representations)
    while (heap.size > 0 && outputCount < maxSplats) {
        const entry = heap.pop()!;
        outputIndices[outputCount++] = entry.nodeIndex;
    }

    if (outputCount === 0)
        console.warn('[traverse] exit 0:', { limit: pixelScaleLimit, max: maxSplats, rootPs, heapSize: heap.size });
    return outputCount;
}

/**
 * Traverse a single LOD tree with pixel_scale-based selection.
 *
 * In standard mode: uses config.pixelScaleLimit directly.
 * In dynamic mode: iteratively tightens limit to hit maxSplats budget.
 *
 * @returns number of selected GS indices written to outputIndices
 */
export function traverseBlock(tree: TreeData, config: TraverseConfig, outputIndices: Uint32Array): number {
    if (tree.totalNodes === 0 || config.maxSplats === 0) {
        console.warn('[tb] skip: empty', { nodes: tree.totalNodes, max: config.maxSplats });
        return 0;
    }

    if (!config.dynamicMode) {
        const c = _standardTraverse(tree, config.pixelScaleLimit, config.maxSplats, config, outputIndices);
        console.log('[tb] standard:', {
            nodes: tree.totalNodes,
            limit: config.pixelScaleLimit,
            max: config.maxSplats,
            count: c,
        });
        return c;
    }

    // Dynamic mode: iterate to meet budget
    const pixelScaleLimit = config.pixelScaleLimit;
    const maxSplats = config.maxSplats;

    let currentLimit = _lastDynamicLimit > pixelScaleLimit ? _lastDynamicLimit : pixelScaleLimit * 100;

    let lastCount = 0;
    const MAX_ITER = 5;

    for (let iter = 0; iter < MAX_ITER; iter++) {
        const t0 = performance.now();
        const count = _standardTraverse(tree, currentLimit, maxSplats, config, outputIndices);
        const dt = performance.now() - t0;
        console.log(
            `[tb] dyn iter ${iter}: limit=${currentLimit.toFixed(6)} count=${count} max=${maxSplats} dt=${dt.toFixed(1)}ms`,
        );

        if (count === 0) {
            if (_tbLog++ % 30 === 0) console.warn('[tb] dyn break: count=0', { iter, limit: currentLimit });
            break;
        }

        const ratio = count / maxSplats;
        if (ratio >= 0.95 && ratio <= 1.0) {
            if (_tbLog++ % 30 === 0) console.log('[tb] dyn converged', { limit: currentLimit, count });
            _lastDynamicLimit = currentLimit;
            return count;
        }

        currentLimit *= Math.pow(ratio, 0.5);

        if (currentLimit < pixelScaleLimit) {
            currentLimit = pixelScaleLimit;
            const finalCount = _standardTraverse(tree, currentLimit, maxSplats, config, outputIndices);
            if (_tbLog++ % 30 === 0) console.log('[tb] dyn clamped', { limit: currentLimit, count: finalCount });
            _lastDynamicLimit = currentLimit;
            return finalCount;
        }

        lastCount = count;
    }

    console.warn('[tb] dyn fallback', { lastCount, limit: currentLimit });
    _lastDynamicLimit = currentLimit;
    return lastCount;
}

/**
 * Convenience: compute feature_sizes array from decoded RAD scale data.
 * scale is Ln0R8 encoded u8[3] per node.
 * feature_size = max(ln0r8ToF32(sx), ln0r8ToF32(sy), ln0r8ToF32(sz)) * 2
 */
export function computeFeatureSizes(scale: Uint8Array, totalNodes: number): Float32Array {
    const sizes = new Float32Array(totalNodes);
    for (let i = 0; i < totalNodes; i++) {
        const o = i * 3;
        const sx = ln0r8ToF32(scale[o]);
        const sy = ln0r8ToF32(scale[o + 1]);
        const sz = ln0r8ToF32(scale[o + 2]);
        sizes[i] = Math.max(sx, sy, sz) * 2;
        if (sizes[i] < 1e-10) sizes[i] = 1e-10;
    }
    return sizes;
}

/** Decode single Ln0R8 value to float */
function ln0r8ToF32(u: number, lnMin = -12, lnMax = 9): number {
    const ln = (u / 255) * (lnMax - lnMin) + lnMin;
    return Math.exp(ln);
}

/**
 * Build TreeData from decoded .rad arrays.
 * Converts f16 centers to f32 and pre-computes feature sizes.
 */
export function prepareTreeData(
    center: Uint16Array, // f16[3] per node
    scale: Uint8Array, // Ln0R8[3] per node
    childStart: Uint32Array,
    childCount: Uint16Array,
    totalNodes: number,
): TreeData {
    const centers = new Float32Array(totalNodes * 3);
    for (let i = 0; i < totalNodes; i++) {
        centers[i * 3] = f16ToF32(center[i * 3]);
        centers[i * 3 + 1] = f16ToF32(center[i * 3 + 1]);
        centers[i * 3 + 2] = f16ToF32(center[i * 3 + 2]);
    }
    const featureSizes = computeFeatureSizes(scale, totalNodes);
    return { totalNodes, childStart, childCount, centers, featureSizes };
}

/** Decode a single f16 value to f32 */
function f16ToF32(h: number): number {
    const sign = (h & 0x8000) << 16;
    let exp = (h >> 10) & 0x1f;
    let mant = h & 0x3ff;
    if (exp === 0) {
        exp = 1;
        while (!(mant & 0x400) && mant) {
            mant <<= 1;
            exp--;
        }
        mant &= 0x3ff;
        exp += 112;
    } else if (exp === 31) {
        exp = 255;
    } else {
        exp += 112;
    }
    const bits = sign | (exp << 23) | (mant << 13);
    const u32 = new Uint32Array(1);
    u32[0] = bits;
    return new Float32Array(u32.buffer)[0];
}

/**
 * Depth-sort selected GS indices by camera-space Z (far→near).
 *
 * Uses dot(center - cameraPos, cameraForward) as the depth metric.
 * Returns a new Uint32Array with sorted indices.
 */
export function depthSort(
    indices: Uint32Array,
    count: number,
    centers: Float32Array,
    camX: number,
    camY: number,
    camZ: number,
    forwardX: number,
    forwardY: number,
    forwardZ: number,
): Uint32Array {
    if (count <= 1) return indices.slice(0, count);

    // Build parallel arrays (avoids object allocation)
    const zs = new Float32Array(count);
    const order = new Uint32Array(count);
    for (let i = 0; i < count; i++) {
        const idx = indices[i];
        const co = idx * 3;
        const dx = centers[co] - camX;
        const dy = centers[co + 1] - camY;
        const dz = centers[co + 2] - camZ;
        zs[i] = dx * forwardX + dy * forwardY + dz * forwardZ;
        order[i] = i;
    }

    // Sort indices by z descending
    order.sort((a, b) => {
        if (zs[b] > zs[a]) return 1;
        if (zs[b] < zs[a]) return -1;
        return 0;
    });

    const sorted = new Uint32Array(count);
    for (let i = 0; i < count; i++) {
        sorted[i] = indices[order[i]];
    }
    return sorted;
}
