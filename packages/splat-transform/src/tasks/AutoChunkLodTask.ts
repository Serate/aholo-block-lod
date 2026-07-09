import type { SplatData } from '../SplatData.js';
import { type Context, BaseTask, type SingleFile } from './BaseTask.js';
import { splitSplat, getModule } from '../native/index.js';

export interface Config {
    input: string;
    output: string;
    /** Max GS per block. Default 200000. */
    maxBlockGs?: number;
    /** Cycling LOD multipliers. Default [1.0, 1.4, 1.7]. */
    multipliers?: number[];
}

const DEFAULT_MULTIPLIERS = [1.0, 1.4, 1.7];

/**
 * Convert SplatData column arrays into interleaved AoS Float32Array.
 * Each GS in the output has `dims` consecutive values.
 */
function interleaveColumns(table: Float32Array[], colStart: number, count: number, dims: number): Float32Array {
    const out = new Float32Array(count * dims);
    for (let i = 0; i < count; i++) {
        for (let d = 0; d < dims; d++) {
            out[i * dims + d] = table[colStart + d][i];
        }
    }
    return out;
}

export class AutoChunkLodTask extends BaseTask<Config> {
    override async exec(config: Config, { logger, resources }: Context) {
        const { input, output, maxBlockGs = 200000, multipliers = DEFAULT_MULTIPLIERS } = config;
        const inputData = resources.get(input);
        // TODO: array support
        const splat = Array.isArray(inputData) ? (inputData[0].content as SplatData) : (inputData as SplatData);
        logger.info(`loaded -> "${input}" (${splat.counts} GS, SH=${splat.shDegree})`);

        const shCounts = splat.shCounts;

        // Step 1: Split into 8-tree blocks
        logger.info('splitting into blocks...');
        logger.time('split elapsed');
        const blockPrecision = Math.min(1, maxBlockGs / splat.counts);
        const { blocks, splats } = splitSplat(splat, blockPrecision);
        logger.timeEnd('split elapsed');
        logger.info(`  blocks: ${blocks.length}`);

        // Step 2: For each block, build LOD tree + encode RAD
        const multBuf = new Float32Array(multipliers);
        const outputs: SingleFile[] = [];
        const outputBlocks: Array<{
            bound: { min: [number, number, number]; max: [number, number, number] };
            file: number;
            count: number;
        }> = [];

        for (let bi = 0; bi < blocks.length; bi++) {
            const blockData = splats[bi];
            const count = blockData.counts;
            const table = blockData.table;
            logger.info(`block ${bi}/${blocks.length}: ${count} GS`);

            // Interleave column arrays → AoS Float32Array
            const center = interleaveColumns(table, 0, count, 3);
            const scale = interleaveColumns(table, 3, count, 3);
            const quat = interleaveColumns(table, 6, count, 4);
            const rgba = interleaveColumns(table, 10, count, 4);
            const sh = shCounts > 0 ? interleaveColumns(table, 14, count, shCounts) : null;

            // Build LOD tree (cycling_lod)
            logger.time(`buildTree ${bi}`);
            const tree = getModule().buildLodTree(
                new Float32Array(center.buffer),
                new Float32Array(scale.buffer),
                new Float32Array(quat.buffer),
                new Float32Array(rgba.buffer),
                sh ? new Float32Array(sh.buffer) : null,
                count,
                splat.shDegree,
                multBuf,
            );
            logger.timeEnd(`buildTree ${bi}`);
            logger.info(`  tree nodes: ${tree.treeNodeCount} (${tree.gsCount} leaves)`);

            // Encode to .rad binary
            logger.time(`encodeRad ${bi}`);
            const radBuffer: Buffer = getModule().encodeRad(
                tree.center,
                tree.scale,
                tree.quat,
                tree.rgba,
                tree.sh ?? null,
                tree.childStart,
                tree.childCount,
                tree.treeNodeCount,
                splat.shDegree,
            );
            logger.timeEnd(`encodeRad ${bi}`);
            logger.info(`  .rad size: ${radBuffer.byteLength} bytes`);

            const fileName = `block_${bi}.rad`;
            outputs.push({ name: fileName, content: radBuffer });
            outputBlocks.push({
                bound: blocks[bi].box,
                file: bi,
                count: tree.gsCount,
            });

            if ((bi + 1) % 10 === 0 || bi === blocks.length - 1) {
                logger.info(`  ${bi + 1}/${blocks.length} blocks processed`);
            }
        }

        // Step 3: Write lod-meta.json
        const totalGs = outputBlocks.reduce((s, b) => s + b.count, 0);
        logger.info(`Total blocks: ${outputBlocks.length}, total GS: ${totalGs}`);

        outputs.unshift({
            name: 'lod-meta.json',
            content: JSON.stringify({
                magicCode: 0x262834,
                type: 'lod-splat',
                version: '1.0',
                counts: splat.counts,
                shDegree: splat.shDegree,
                levels: 5,
                files: outputBlocks.map(b => `block_${b.file}.rad`),
                tree: outputBlocks.map(b => ({
                    bound: b.bound,
                    file: b.file,
                    count: b.count,
                })),
            }),
        });

        resources.set(output, outputs);
    }

    override requiresGPU(_config: Config): boolean {
        return false;
    }
}
