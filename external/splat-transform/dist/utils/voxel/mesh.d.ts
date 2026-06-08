import type { SparseVoxelGrid, Bounds } from './common.js';
export type CollisionMeshShape = 'smooth' | 'faces';
export declare function buildCollisionMesh(grid: SparseVoxelGrid, gridBounds: Bounds, voxelResolution: number, shape?: CollisionMeshShape): Uint8Array | undefined;
