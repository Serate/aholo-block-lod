/**
 * Portions of this voxel pipeline are adapted from:
 * https://github.com/playcanvas/splat-transform
 * Copyright (c) 2011-2026 PlayCanvas Ltd.
 * Licensed under the MIT License.
 */
import { type SplatData } from '../SplatData.js';
import { type VoxelNodeEncoding } from '../utils/voxel/binary.js';
import { type NavSeed } from '../utils/voxel/nav.js';
import { type CollisionMeshShape } from '../utils/voxel/mesh.js';
import { type FilterClusterOptions } from '../utils/voxel/filter-cluster.js';
import { decodeMorton3, encodeMorton3, getChildOffset } from '../utils/voxel/common.js';
type VoxelBackend = 'cpu' | 'gpu';
type CollisionMeshOption = boolean | CollisionMeshShape;
interface BoundsBox {
    minCorner: [number, number, number];
    maxCorner: [number, number, number];
}
export declare function writeVoxelFiles(outputDir: string, data: SplatData, options?: {
    voxelResolution?: number;
    opacityCutoff?: number;
    backend?: VoxelBackend;
    collisionMesh?: CollisionMeshOption;
    navExteriorRadius?: number;
    floorFill?: boolean;
    floorFillDilation?: number;
    cpuWorkerCount?: number;
    box?: BoundsBox;
    navCapsule?: {
        height: number;
        radius: number;
    };
    navSeed?: NavSeed;
    gzip?: boolean;
    nodeEncoding?: VoxelNodeEncoding;
    filterCluster?: boolean | FilterClusterOptions;
}): Promise<void>;
export declare const voxelUtils: {
    getChildOffset: typeof getChildOffset;
    encodeMorton3: typeof encodeMorton3;
    decodeMorton3: typeof decodeMorton3;
};
export {};
