import fs from 'node:fs';
import path from 'node:path';
import { Writable } from 'node:stream';
import { SplatData } from '../SplatData.js';
import { createSplatFile, detectSplatFileType, SplatFileType } from '../utils/index.js';
import { type Context, BaseTask } from './BaseTask.js';

export interface Config {
    input: string;
    output: string;
    enableMortonSort?: boolean;
    compressLevel?: number;
    spzVersion?: number;
}

async function writeSplatFile(
    filepath: string,
    data: SplatData,
    enableMortonSort: boolean,
    compressLevel?: number,
    spzVersion?: number,
) {
    let indices: Uint32Array | undefined;
    if (!enableMortonSort) {
        indices = new Uint32Array(data.counts);
        for (let i = 0; i < data.counts; i++) {
            indices[i] = i;
        }
    }
    const file = createSplatFile(filepath, undefined, compressLevel, spzVersion);
    const stream = Writable.toWeb(fs.createWriteStream(filepath)) as WritableStream<Uint8Array>;
    await file.write(stream, data, indices);
}

export class WriteTask extends BaseTask<Config> {
    override async exec(config: Config, { logger, resources }: Context) {
        const { input, output, enableMortonSort = true, compressLevel, spzVersion } = config;
        const source = resources.get(input)!;
        if (source instanceof SplatData) {
            logger.info(`writing splat -> file="${output}" count=${source.counts} SH=${source.shDegree}`);
            await writeSplatFile(output, source, enableMortonSort, compressLevel, spzVersion);
            logger.info(`writing done`);
            return;
        }

        if (fs.existsSync(output)) {
            fs.rmSync(output, { recursive: true });
            logger.info(`exist dir ${output}, removed`);
        }
        fs.mkdirSync(output, { recursive: true });

        logger.info(`writing bundle -> dir="${output}" files=${source.length}`);
        logger.silent = true;
        const sogList: Array<{ name: string; content: SplatData }> = [];
        const promises: Array<Promise<void>> = [];
        let idx: number = 1;
        const totals = source.length;
        for (let i = 0; i < source.length; i++) {
            const { name, content, preserveOrder = false } = source[i];
            const filepath = path.join(output, name);
            if (typeof content === 'string') {
                logger.info(`- ${filepath} (${idx++}/${totals})`, true);
                fs.writeFileSync(filepath, content);
                continue;
            }
            const type = detectSplatFileType(filepath);
            if (type === SplatFileType.SOG) {
                sogList.push({ name, content: content as SplatData });
                continue;
            }
            logger.info(`- ${filepath} (${idx++}/${totals})`, true);
            const promise = writeSplatFile(
                filepath,
                content,
                enableMortonSort && !preserveOrder,
                compressLevel,
                spzVersion,
            );
            promises.push(promise);
        }
        await Promise.all(promises);
        for (let i = 0; i < sogList.length; i++) {
            const { name, content } = sogList[i];
            const filepath = path.join(output, name);
            logger.info(`- ${filepath} (${idx++}/${totals})`, true);
            await writeSplatFile(filepath, content, false);
        }
        logger.silent = false;
        logger.info(`writing bundle done -> dir="${output}" files=${source.length}`);
    }

    override requiresGPU(config: Config): boolean {
        return config.output.endsWith('sog');
    }
}
