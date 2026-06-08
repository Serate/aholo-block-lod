import type { FilterClusterOptions } from '../utils/voxel/filter-cluster.js';
import type { VoxelNodeEncoding } from '../utils/voxel/binary.js';
import { type Context, BaseTask } from './BaseTask.js';
export interface VoxelTaskConfig {
    input: string;
    output: string;
    voxelResolution?: number;
    opacityCutoff?: number;
    backend?: 'cpu' | 'gpu';
    collisionMesh?: boolean | 'smooth' | 'faces';
    navExteriorRadius?: number;
    floorFill?: boolean;
    floorFillDilation?: number;
    cpuWorkerCount?: number;
    box?: {
        minCorner: [number, number, number];
        maxCorner: [number, number, number];
    };
    navCapsule?: {
        height: number;
        radius: number;
    };
    navSeed?: {
        x: number;
        y: number;
        z: number;
    };
    gzip?: boolean;
    nodeEncoding?: VoxelNodeEncoding;
    filterCluster?: boolean | FilterClusterOptions;
}
export declare class VoxelTask extends BaseTask<VoxelTaskConfig> {
    exec(config: VoxelTaskConfig, { logger, resources }: Context): Promise<void>;
    requiresGPU(_config: VoxelTaskConfig): boolean;
}
