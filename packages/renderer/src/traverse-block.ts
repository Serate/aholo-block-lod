/**
 * TypeScript port of C++ traverseBlock (lod_tree.cpp).
 *
 * BFS traversal of the cycling_lod tree with pixel_scale-based LOD selection.
 *
 * Each frame:
 *   traverseBlock(treeData, cameraPos, cameraForward, lodScale, pixelScaleLimit, maxSplats)
 *   → returns ordered Uint32Array of GS indices for rendering via orderTex.
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

export interface TraverseResult {
    /** Output indices ordered by LOD traversal */
    indices: Uint32Array;
    /** Number of valid entries in indices */
    count: number;
}

// Reusable temp arrays to avoid per-frame allocation in hot loop
const heap = new MaxHeap();
const pos: [number, number, number] = [0, 0, 0];

/**
 * Compute pixel_scale for a node.
 * pixel_scale = (featureSize / distance) * lodScale
 */
function computePixelScale(
    cx: number,
    cy: number,
    cz: number,
    featureSize: number,
    camX: number,
    camY: number,
    camZ: number,
    lodScale: number,
): number {
    const dx = cx - camX;
    const dy = cy - camY;
    const dz = cz - camZ;
    const dist = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-6) return (featureSize / 1e-6) * lodScale;
    return (featureSize / dist) * lodScale;
}

/**
 * Traverse the LOD tree using BFS with pixel_scale-based thresholding.
 *
 * Algorithm (matches C++ traverse_block):
 * 1. Push root node with its pixel_scale
 * 2. While heap not empty and output < maxSplats:
 *    - Pop highest pixel_scale entry
 *    - If pixel_scale <= pixelScaleLimit: output node (leaf or LOD-clamped)
 *    - Else: expand children — for each child:
 *        compute pixel_scale, if <= limit output directly, else push to heap
 * 3. Flush remaining heap entries as output
 * 4. Return ordered index array
 */
export function traverseBlock(
    tree: TreeData,
    cameraPos: [number, number, number],
    _cameraForward: [number, number, number],
    lodScale: number,
    pixelScaleLimit: number,
    maxSplats: number,
): TraverseResult {
    const { totalNodes, childStart, childCount, centers, featureSizes } = tree;
    if (totalNodes === 0 || maxSplats === 0) return { indices: new Uint32Array(0), count: 0 };

    const [camX, camY, camZ] = cameraPos;
    const indices = new Uint32Array(Math.min(totalNodes, maxSplats));
    let outputCount = 0;

    heap.clear();

    // Push root
    const rootPs = computePixelScale(centers[0], centers[1], centers[2], featureSizes[0], camX, camY, camZ, lodScale);
    heap.push({ pixelScale: rootPs, nodeIndex: 0 });
    let numSplats = 1;

    while (heap.size > 0 && outputCount < maxSplats) {
        const entry = heap.peek()!;
        if (entry.pixelScale <= pixelScaleLimit) break;

        const idx = entry.nodeIndex;
        const cnt = childCount[idx];

        if (cnt === 0) {
            // Leaf → output
            heap.pop();
            indices[outputCount++] = idx;
            continue;
        }

        const start = childStart[idx];
        const newTotal = numSplats - 1 + cnt;
        if (newTotal > maxSplats) break; // entry stays on heap for flush

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
                lodScale,
            );

            if (ps <= pixelScaleLimit) {
                indices[outputCount++] = childIdx;
                if (outputCount >= maxSplats) {
                    numSplats = newTotal;
                    return { indices, count: outputCount };
                }
            } else {
                heap.push({ pixelScale: ps, nodeIndex: childIdx });
            }
        }
        numSplats = newTotal;
    }

    // Flush remaining heap entries as output
    while (heap.size > 0 && outputCount < maxSplats) {
        const entry = heap.pop()!;
        indices[outputCount++] = entry.nodeIndex;
    }

    return { indices, count: outputCount };
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
