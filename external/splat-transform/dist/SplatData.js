import { Quaternion, deferred } from './utils/index.js';
import { SH_MAPS } from './constant.js';
export var ColIdx;
(function (ColIdx) {
    ColIdx[ColIdx["x"] = 0] = "x";
    ColIdx[ColIdx["y"] = 1] = "y";
    ColIdx[ColIdx["z"] = 2] = "z";
    ColIdx[ColIdx["sx"] = 3] = "sx";
    ColIdx[ColIdx["sy"] = 4] = "sy";
    ColIdx[ColIdx["sz"] = 5] = "sz";
    ColIdx[ColIdx["qx"] = 6] = "qx";
    ColIdx[ColIdx["qy"] = 7] = "qy";
    ColIdx[ColIdx["qz"] = 8] = "qz";
    ColIdx[ColIdx["qw"] = 9] = "qw";
    ColIdx[ColIdx["r"] = 10] = "r";
    ColIdx[ColIdx["g"] = 11] = "g";
    ColIdx[ColIdx["b"] = 12] = "b";
    ColIdx[ColIdx["a"] = 13] = "a";
    ColIdx[ColIdx["shOffset"] = 14] = "shOffset";
})(ColIdx || (ColIdx = {}));
const tempQuat = new Quaternion(0, 0, 0, 1);
export class SplatData {
    constructor(blockCounts = 1, maxShDegree = 3) {
        this.blockOffsets = [];
        this.blockContentCounts = [];
        this.totalBlockCounts = 0;
        this.totalBlockShDegree = 3;
        this.blockExecs = [];
        this.currentBlockIndex = 0;
        this.blockCounts = blockCounts;
        this.maxShDegree = maxShDegree;
    }
    initBlock(counts, shDegree) {
        this.blockContentCounts.push(counts);
        this.blockOffsets.push(this.totalBlockCounts);
        this.totalBlockCounts += counts;
        this.totalBlockShDegree = Math.min(shDegree, this.totalBlockShDegree);
        const { promise, resolve } = deferred();
        this.blockExecs.push(resolve);
        if (this.blockOffsets.length === this.blockCounts) {
            this.init(this.totalBlockCounts, this.totalBlockShDegree);
            this.blockExecs[this.currentBlockIndex](this.blockOffsets[0]);
        }
        return promise;
    }
    finishBlock() {
        this.currentBlockIndex++;
        this.blockExecs[this.currentBlockIndex]?.(this.blockOffsets[this.currentBlockIndex]);
    }
    init(counts, shDegree) {
        this.counts = counts;
        this.shDegree = Math.min(shDegree, this.maxShDegree);
        const shCounts = (this.shCounts = SH_MAPS[this.shDegree]);
        this.table = new Array(14 + shCounts).fill(0).map(() => new Float32Array(counts));
        return this;
    }
    set(i, single) {
        const { table } = this;
        table[ColIdx.x][i] = single.x;
        table[ColIdx.y][i] = single.y;
        table[ColIdx.z][i] = single.z;
        table[ColIdx.sx][i] = single.sx;
        table[ColIdx.sy][i] = single.sy;
        table[ColIdx.sz][i] = single.sz;
        tempQuat.set(single.qx, single.qy, single.qz, single.qw).normalize();
        table[ColIdx.qx][i] = tempQuat.x;
        table[ColIdx.qy][i] = tempQuat.y;
        table[ColIdx.qz][i] = tempQuat.z;
        table[ColIdx.qw][i] = tempQuat.w;
        table[ColIdx.r][i] = single.r;
        table[ColIdx.g][i] = single.g;
        table[ColIdx.b][i] = single.b;
        table[ColIdx.a][i] = single.a;
    }
    setCenter(i, x, y, z) {
        const { table } = this;
        table[ColIdx.x][i] = x;
        table[ColIdx.y][i] = y;
        table[ColIdx.z][i] = z;
    }
    setScale(i, sx, sy, sz) {
        const { table } = this;
        table[ColIdx.sx][i] = sx;
        table[ColIdx.sy][i] = sy;
        table[ColIdx.sz][i] = sz;
    }
    setQuat(i, qx, qy, qz, qw) {
        const { table } = this;
        tempQuat.set(qx, qy, qz, qw).normalize();
        table[ColIdx.qx][i] = tempQuat.x;
        table[ColIdx.qy][i] = tempQuat.y;
        table[ColIdx.qz][i] = tempQuat.z;
        table[ColIdx.qw][i] = tempQuat.w;
    }
    setColor(i, r, g, b) {
        const { table } = this;
        table[ColIdx.r][i] = r;
        table[ColIdx.g][i] = g;
        table[ColIdx.b][i] = b;
    }
    setAlpha(i, a) {
        const { table } = this;
        table[ColIdx.a][i] = a;
    }
    setShN(i, shN) {
        const { table, shCounts } = this;
        for (let j = 0; j < shCounts; j++) {
            table[ColIdx.shOffset + j][i] = shN[j];
        }
    }
    get(i, single) {
        const { table } = this;
        single.x = table[ColIdx.x][i];
        single.y = table[ColIdx.y][i];
        single.z = table[ColIdx.z][i];
        single.sx = table[ColIdx.sx][i];
        single.sy = table[ColIdx.sy][i];
        single.sz = table[ColIdx.sz][i];
        single.qx = table[ColIdx.qx][i];
        single.qy = table[ColIdx.qy][i];
        single.qz = table[ColIdx.qz][i];
        single.qw = table[ColIdx.qw][i];
        single.r = table[ColIdx.r][i];
        single.g = table[ColIdx.g][i];
        single.b = table[ColIdx.b][i];
        single.a = table[ColIdx.a][i];
    }
    getCenter(i, single) {
        const { table } = this;
        single.x = table[ColIdx.x][i];
        single.y = table[ColIdx.y][i];
        single.z = table[ColIdx.z][i];
    }
    getScale(i, single) {
        const { table } = this;
        single.sx = table[ColIdx.sx][i];
        single.sy = table[ColIdx.sy][i];
        single.sz = table[ColIdx.sz][i];
    }
    getQuat(i, single) {
        const { table } = this;
        single.qx = table[ColIdx.qx][i];
        single.qy = table[ColIdx.qy][i];
        single.qz = table[ColIdx.qz][i];
        single.qw = table[ColIdx.qw][i];
    }
    getColor(i, single) {
        const { table } = this;
        single.r = table[ColIdx.r][i];
        single.g = table[ColIdx.g][i];
        single.b = table[ColIdx.b][i];
    }
    getAlpha(i, single) {
        const { table } = this;
        single.a = table[ColIdx.a][i];
    }
    getShN(i, shN) {
        const { shCounts, table } = this;
        for (let j = 0; j < shCounts; j++) {
            shN[j] = table[ColIdx.shOffset + j][i];
        }
    }
    destroy() {
        this.counts = 0;
        this.table = [];
    }
}
