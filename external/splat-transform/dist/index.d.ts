import { type Config as AutoChunkLodConfig } from './tasks/AutoChunkLodTask.js';
import { type Config as AutoLodConfig } from './tasks/AutoLodTask.js';
import { type Config as FlexLodConfig } from './tasks/FlexLodTask.js';
import { type Config as SkeletonLodConfig } from './tasks/SkeletonLodTask.js';
import { type Config as ModifyConfig } from './tasks/ModifyTask.js';
import { type Config as ReadConfig } from './tasks/ReadTask.js';
import { type Config as WriteConfig } from './tasks/WriteTask.js';
import { type VoxelTaskConfig } from './tasks/VoxelTask.js';
interface TaskConfigMap {
    Read: ReadConfig;
    Write: WriteConfig;
    Voxel: VoxelTaskConfig;
    Modify: ModifyConfig;
    SkeletonLod: SkeletonLodConfig;
    FlexLod: FlexLodConfig;
    AutoLod: AutoLodConfig;
    AutoChunkLod: AutoChunkLodConfig;
}
type PipelineTask = {
    [K in keyof TaskConfigMap]: {
        id: string;
        type: K;
        config: TaskConfigMap[K];
        release?: string[];
    };
}[keyof TaskConfigMap];
interface PipelineConfig {
    version: number;
    gpu?: number;
    tasks: PipelineTask[];
}
export declare function runner(config: PipelineConfig): Promise<void>;
export {};
