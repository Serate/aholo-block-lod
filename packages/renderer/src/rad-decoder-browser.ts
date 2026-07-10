/**
 * Browser-side .rad file decoder.
 *
 * Parses the RAD binary format in pure TypeScript using the Web DecompressionStream API.
 * Output is compatible with SplatData for use with createSplat().
 *
 * .rad format:
 *   [RAD0 magic:4][meta_len:4][JSON metadata:meta_len][padding:0-7]
 *   [RADC magic:4][cm_len:4][JSON chunk meta:cm_len][padding:0-7][payload_size:8][payload:payload_size]
 *   ... more chunks ...
 *
 * Each property in the payload:
 *   [tag:1][data...]  where tag=0 raw, tag=1 raw_deflate
 */

// ── helpers ──

function readU32LE(data: Uint8Array, off: number): number {
    return data[off] | (data[off + 1] << 8) | (data[off + 2] << 16) | (data[off + 3] << 24);
}

function readU64LE(data: Uint8Array, off: number): number {
    const lo = readU32LE(data, off);
    const hi = readU32LE(data, off + 4);
    return lo + hi * 0x100000000;
}

export function f16ToF32(h: number): number {
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

/** Decode Ln0R8 scale value to float */
function ln0r8ToF32(u: number, lnMin = -12, lnMax = 9): number {
    const ln = (u / 255) * (lnMax - lnMin) + lnMin;
    return Math.exp(ln);
}

/** Decode Oct88R8 quaternion (3 bytes → f32[4]) */
function oct88r8ToF32(u: number, v: number, a: number): [number, number, number, number] {
    const uf = (u / 255) * 2 - 1;
    const vf = (v / 255) * 2 - 1;
    const angle = (a / 255) * Math.PI;
    let qz = 1 - Math.abs(uf) - Math.abs(vf);
    let qx = uf,
        qy = vf;
    if (qz < 0) {
        const tx = qx,
            ty = qy;
        qx = (1 - Math.abs(ty)) * (tx >= 0 ? 1 : -1);
        qy = (1 - Math.abs(tx)) * (ty >= 0 ? 1 : -1);
        qz = 1 - Math.abs(qx) - Math.abs(qy);
    }
    const len = Math.sqrt(qx * qx + qy * qy + qz * qz);
    if (len > 1e-10) {
        qx /= len;
        qy /= len;
        qz /= len;
    }
    const ha = angle * 0.5;
    const sinHa = Math.sin(ha);
    return [qx * sinHa, qy * sinHa, qz * sinHa, Math.cos(ha)];
}

// ── decompression ──

async function decompressRawDeflate(data: Uint8Array): Promise<Uint8Array> {
    const cs = new DecompressionStream('deflate-raw');
    const writer = cs.writable.getWriter();
    writer.write(data as BufferSource);
    writer.close();
    const reader = cs.readable.getReader();
    const chunks: Uint8Array[] = [];
    while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value!);
    }
    const total = chunks.reduce((s, c) => s + c.length, 0);
    const result = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
        result.set(c, off);
        off += c.length;
    }
    return result;
}

/** Decode a single property payload: 1-byte tag + data */
async function decodeProperty(data: Uint8Array): Promise<Uint8Array> {
    if (data.length < 1) return new Uint8Array(0);
    const tag = data[0];
    if (tag === 0) return data.slice(1); // raw
    if (tag === 1) return decompressRawDeflate(data.slice(1)); // raw deflate
    return data; // unknown tag, return as-is
}

// ── public API ──

export interface RadDecodeResult {
    count: number;
    totalNodes: number;
    shDegree: number;
    center: Uint16Array; // f16[3]
    rgba: Uint8Array; // u8[4]
    scale: Uint8Array; // u8[3] Ln0R8
    quat: Uint8Array; // u8[3] Oct88R8
    sh?: Uint8Array;
    childStart: Uint32Array;
    childCount: Uint16Array;
}

export async function decodeRad(data: Uint8Array): Promise<RadDecodeResult | null> {
    if (data.length < 8) return null;

    const magic = readU32LE(data, 0);
    if (magic !== 0x30444152) return null; // "RAD0"

    const metaLen = readU32LE(data, 4);
    const metaEnd = 8 + ((metaLen + 7) & ~7);
    if (data.length < metaEnd) return null;

    // Parse header JSON
    const metaJson = new TextDecoder().decode(data.slice(8, 8 + metaLen));
    const meta = JSON.parse(metaJson);

    const total = meta.count as number;
    const shDegree = (meta.maxSh as number) ?? 0;
    const chunkSize = (meta.chunkSize as number) ?? 16384;
    const chunks = meta.chunks as Array<{ offset: number; bytes: number }>;

    const shStride = shDegree >= 3 ? 45 : shDegree >= 2 ? 24 : shDegree >= 1 ? 9 : 0;

    // Allocate output arrays
    const result: RadDecodeResult = {
        count: total,
        totalNodes: total,
        shDegree,
        center: new Uint16Array(total * 3),
        rgba: new Uint8Array(total * 4),
        scale: new Uint8Array(total * 3),
        quat: new Uint8Array(total * 3),
        childStart: new Uint32Array(total),
        childCount: new Uint16Array(total),
    };
    if (shStride > 0) result.sh = new Uint8Array(total * shStride);

    const chunksStart = metaEnd;
    let base = 0;

    for (const cr of chunks) {
        if (base >= total) break;
        const chunkOff = chunksStart + cr.offset;
        const chunkBytes = Math.min(cr.bytes, data.length - chunkOff);
        if (chunkBytes < 8) break;

        const chunkData = data.slice(chunkOff, chunkOff + chunkBytes);
        const cmMagic = readU32LE(chunkData, 0);
        if (cmMagic !== 0x43444152) break; // "RADC"

        const cmLen = readU32LE(chunkData, 4);
        const cmEnd = 8 + ((cmLen + 7) & ~7);
        const payloadBytes = readU64LE(chunkData, cmEnd);
        const payloadStart = cmEnd + 8;

        // Parse chunk meta JSON
        const cmJson = new TextDecoder().decode(chunkData.slice(8, 8 + cmLen));
        const cm = JSON.parse(cmJson);
        const props = cm.properties as Array<{ property: string; encoding: string; offset: number; bytes: number }>;

        // Decode each property
        for (const prop of props) {
            if (prop.offset + prop.bytes > payloadBytes) continue;
            const propData = chunkData.slice(payloadStart + prop.offset, payloadStart + prop.offset + prop.bytes);
            const decomp = await decodeProperty(propData);
            switch (prop.property) {
                case 'center': {
                    // F32LeBytes: per-dimension plane layout
                    // [x0,x1,x2..., y0,y1,y2..., z0,z1,z2...]
                    const nItems = Math.min(Math.floor(decomp.length / 12), total - base);
                    const f32view = new Float32Array(decomp.buffer, decomp.byteOffset, decomp.length / 4);
                    const perDim = nItems;
                    for (let i = 0; i < nItems; i++) {
                        result.center[(base + i) * 3] = _f32toF16(f32view[i]); // x
                        result.center[(base + i) * 3 + 1] = _f32toF16(f32view[perDim + i]); // y
                        result.center[(base + i) * 3 + 2] = _f32toF16(f32view[perDim * 2 + i]); // z
                    }
                    break;
                }
                case 'alpha': {
                    const nItems = Math.min(decomp.length, total - base);
                    for (let i = 0; i < nItems; i++) {
                        result.rgba[(base + i) * 4 + 3] = decomp[i];
                    }
                    break;
                }
                case 'rgb': {
                    const nItems = Math.min(Math.floor(decomp.length / 3), total - base);
                    for (let i = 0; i < nItems; i++) {
                        result.rgba[(base + i) * 4] = decomp[i * 3];
                        result.rgba[(base + i) * 4 + 1] = decomp[i * 3 + 1];
                        result.rgba[(base + i) * 4 + 2] = decomp[i * 3 + 2];
                    }
                    break;
                }
                case 'scale': {
                    const nItems = Math.min(Math.floor(decomp.length / 3), total - base);
                    for (let i = 0; i < nItems; i++) {
                        result.scale[(base + i) * 3] = decomp[i * 3];
                        result.scale[(base + i) * 3 + 1] = decomp[i * 3 + 1];
                        result.scale[(base + i) * 3 + 2] = decomp[i * 3 + 2];
                    }
                    break;
                }
                case 'quat': {
                    const nItems = Math.min(Math.floor(decomp.length / 3), total - base);
                    for (let i = 0; i < nItems; i++) {
                        result.quat[(base + i) * 3] = decomp[i * 3];
                        result.quat[(base + i) * 3 + 1] = decomp[i * 3 + 1];
                        result.quat[(base + i) * 3 + 2] = decomp[i * 3 + 2];
                    }
                    break;
                }
                case 'sh': {
                    if (result.sh && shStride > 0) {
                        const items = Math.min(Math.floor(decomp.length / shStride), total - base);
                        for (let i = 0; i < items; i++) {
                            for (let j = 0; j < shStride; j++) {
                                result.sh[(base + i) * shStride + j] = decomp[i * shStride + j];
                            }
                        }
                    }
                    break;
                }
                case 'child_count': {
                    const nItems = Math.min(Math.floor(decomp.length / 2), total - base);
                    for (let i = 0; i < nItems; i++) {
                        result.childCount[base + i] = decomp[i * 2] | (decomp[i * 2 + 1] << 8);
                    }
                    break;
                }
                case 'child_start': {
                    const nItems = Math.min(Math.floor(decomp.length / 4), total - base);
                    for (let i = 0; i < nItems; i++) {
                        result.childStart[base + i] = readU32LE(decomp, i * 4);
                    }
                    break;
                }
            }
        }
        base += chunkSize;
        if (base > total) base = total;
    }

    return result;
}

// ── f32 → f16 conversion ──
function _f32toF16(f: number): number {
    const buf = new Float32Array(1);
    buf[0] = f;
    const u = new Uint32Array(buf.buffer)[0];
    const sign = (u >> 16) & 0x8000;
    let exp = ((u >> 23) & 0xff) - 127 + 15;
    let mant = u & 0x007fffff;
    if (exp <= 0) {
        mant = (mant | 0x00800000) >> (1 - exp);
        exp = 0;
    } else if (exp >= 31) {
        return sign | 0x7c00 | (mant ? 0x0200 : 0);
    }
    mant >>= 13;
    return sign | (exp << 10) | mant;
}

/**
 * Convert a decoded .rad block to SplatData-compatible column arrays.
 * Returns { table: Float32Array[], counts: number }.
 */
export function decodedBlockToSplatData(decoded: RadDecodeResult): {
    table: Float32Array[];
    counts: number;
    shDegree: number;
} {
    const n = decoded.totalNodes;
    const shDegree = decoded.shDegree;
    const shCounts = shDegree >= 3 ? 45 : shDegree >= 2 ? 24 : shDegree >= 1 ? 9 : 0;

    const cols = 14 + shCounts;
    const table: Float32Array[] = new Array(cols);
    for (let i = 0; i < cols; i++) table[i] = new Float32Array(n);

    for (let i = 0; i < n; i++) {
        // Center: f16 → f32
        table[0][i] = f16ToF32(decoded.center[i * 3]);
        table[1][i] = f16ToF32(decoded.center[i * 3 + 1]);
        table[2][i] = f16ToF32(decoded.center[i * 3 + 2]);

        // Scale: Ln0R8 → f32
        table[3][i] = ln0r8ToF32(decoded.scale[i * 3]);
        table[4][i] = ln0r8ToF32(decoded.scale[i * 3 + 1]);
        table[5][i] = ln0r8ToF32(decoded.scale[i * 3 + 2]);

        // Quat: Oct88R8 → f32[4]
        const [qx, qy, qz, qw] = oct88r8ToF32(decoded.quat[i * 3], decoded.quat[i * 3 + 1], decoded.quat[i * 3 + 2]);
        table[6][i] = qx;
        table[7][i] = qy;
        table[8][i] = qz;
        table[9][i] = qw;

        // RGBA: u8 → f32
        table[10][i] = decoded.rgba[i * 4] / 255;
        table[11][i] = decoded.rgba[i * 4 + 1] / 255;
        table[12][i] = decoded.rgba[i * 4 + 2] / 255;
        table[13][i] = decoded.rgba[i * 4 + 3] / 255;

        // SH
        if (shCounts > 0 && decoded.sh) {
            for (let j = 0; j < shCounts; j++) {
                table[14 + j][i] = (decoded.sh[i * shCounts + j] - 128) / 128;
            }
        }
    }

    return { table, counts: n, shDegree };
}
