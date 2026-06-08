import { SplatData } from '../../SplatData.js';
export interface FilterClusterOptions {
    voxelResolution?: number;
    opacityCutoff?: number;
    minContribution?: number;
    seed?: {
        x: number;
        y: number;
        z: number;
    };
}
export interface FilterClusterRuntimeOptions {
    backend?: 'cpu' | 'gpu';
    cpuWorkerCount?: number;
    box?: {
        minCorner: [number, number, number];
        maxCorner: [number, number, number];
    };
}
export declare function filterCluster(data: SplatData, options?: FilterClusterOptions, runtime?: FilterClusterRuntimeOptions): Promise<SplatData>;
