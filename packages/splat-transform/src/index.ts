import { AutoChunkLodTask, type Config as AutoChunkLodConfig } from './tasks/AutoChunkLodTask.js';
import { AutoLodTask, type Config as AutoLodConfig } from './tasks/AutoLodTask.js';
import { FlexLodTask, type Config as FlexLodConfig } from './tasks/FlexLodTask.js';
import { SkeletonLodTask, type Config as SkeletonLodConfig } from './tasks/SkeletonLodTask.js';
import { ModifyTask, type Config as ModifyConfig } from './tasks/ModifyTask.js';
import { ReadTask, type Config as ReadConfig } from './tasks/ReadTask.js';
import { WriteTask, type Config as WriteConfig } from './tasks/WriteTask.js';
import { VoxelTask, type VoxelTaskConfig } from './tasks/VoxelTask.js';
import type { Context } from './tasks/BaseTask.js';
import { enumerateAdapters, logger, releaseSharedDevice, initGPUAdapter } from './utils/index.js';

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

const TaskMap = {
    Read: new ReadTask(),
    Write: new WriteTask(),
    Voxel: new VoxelTask(),
    Modify: new ModifyTask(),
    SkeletonLod: new SkeletonLodTask(),
    FlexLod: new FlexLodTask(),
    AutoLod: new AutoLodTask(),
    AutoChunkLod: new AutoChunkLodTask(),
};

function anyTaskRequireGPU(tasks: PipelineTask[]) {
    for (const t of tasks) {
        if (TaskMap[t.type].requiresGPU(t.config as any)) {
            return true;
        }
    }
    return false;
}

export async function runner(config: PipelineConfig) {
    console.time('Total elapsed time');
    const ctx: Context = {
        logger,
        resources: new Map(),
    };
    if (anyTaskRequireGPU(config.tasks)) {
        logger.prefix = `[Task:GPU]`;
        logger.info('Any task requires GPU detected, initialize GPU adapter.');
        const adapter = (await enumerateAdapters())[config.gpu ?? 0];
        initGPUAdapter([`adapter=${adapter.name}`]);
    }
    for (const taskDef of config.tasks) {
        const { id, type, config: taskConfig, release = [] } = taskDef;
        const task = TaskMap[type];
        if (!task) {
            throw new Error(`Task not found: ${type} (id: ${id})`);
        }
        logger.prefix = `[Task:${type}#${id}]`;
        logger.time('elapsed time');
        await task.exec(taskConfig as any, ctx);
        release.forEach(v => ctx.resources.delete(v));
        logger.timeEnd('elapsed time');
    }
    releaseSharedDevice();
    console.timeEnd('Total elapsed time');
}
