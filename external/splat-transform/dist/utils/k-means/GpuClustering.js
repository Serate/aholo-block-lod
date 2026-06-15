import { logger } from '../index.js';
const chunkSize = 128;
const workgroupSize = 128;
function clusterWgsl(vecColumns) {
    return /* wgsl */ `
struct Uniforms {
    numPoints: u32,
    numCentroids: u32
};

@group(0) @binding(0) var<uniform> uniforms: Uniforms;
@group(0) @binding(1) var<storage, read> points: array<vec4<f32>>;
@group(0) @binding(2) var<storage, read> centroids: array<vec4<f32>>;
@group(0) @binding(3) var<storage, read> centroidSq: array<f32>;
@group(0) @binding(4) var<storage, read_write> results: array<u32>;

const vecColumns = ${vecColumns}u;
const chunkSize = ${chunkSize}u;
const workgroupSize = ${workgroupSize}u;
var<workgroup> sharedChunk: array<vec4<f32>, vecColumns * chunkSize>;
var<workgroup> sharedSq: array<f32, chunkSize>;

fn calcDistance(point: array<vec4<f32>, vecColumns>, centroid: u32) -> f32 {
    let ci = centroid * vecColumns;
    var result = sharedSq[centroid];

    for (var i = 0u; i < vecColumns; i++) {
        // euclid distance simplify
        // (centroid - point) ^ 2 = centroid ^ 2 - 2 * dot(centroid, point) + point ^ 2
        // point ^ 2 omitted, for same point find nearest centroid is not necessary
        result -= 2.0 * dot(point[i], sharedChunk[ci + i]);
    }

    return result;
}

@compute @workgroup_size(workgroupSize)
fn main(
    @builtin(local_invocation_index) localId : u32,
    @builtin(global_invocation_id) globalId: vec3u,
    @builtin(num_workgroups) numWorkgroups: vec3u
) {
    let pointIndex = globalId.x + globalId.y * numWorkgroups.x * workgroupSize;

    var point: array<vec4<f32>, vecColumns>;
    if (pointIndex < uniforms.numPoints) {
        for (var i = 0u; i < vecColumns; i++) {
            point[i] = points[pointIndex * vecColumns + i];
        }
    }

    var mind = 1000000.0;
    var mini = 0u;

    let numChunks = u32(ceil(f32(uniforms.numCentroids) / f32(chunkSize)));
    for (var i = 0u; i < numChunks; i++) {
        let chunkToLoad = min(chunkSize, uniforms.numCentroids - i * chunkSize);
        for (var row = localId; row < chunkToLoad; row += workgroupSize) {
            let srcRow = i * chunkSize + row;
            let dst = row * vecColumns;
            let src = srcRow * vecColumns;

            for (var c = 0u; c < vecColumns; c++) {
                sharedChunk[dst + c] = centroids[src + c];
            }
            sharedSq[row] = centroidSq[srcRow];
        }

        workgroupBarrier();

        if (pointIndex < uniforms.numPoints) {
            let thisChunkSize = min(chunkSize, uniforms.numCentroids - i * chunkSize);
            for (var c = 0u; c < thisChunkSize; c++) {
                let d = calcDistance(point, c);
                if (d < mind) {
                    mind = d;
                    mini = i * chunkSize + c;
                }
            }
        }

        workgroupBarrier();
    }

    if (pointIndex < uniforms.numPoints) {
        results[pointIndex] = mini;
    }
}
`;
}
function packVec4Data(result, dataTable, numRows, rowOffset, vecColumns, norms) {
    const numColumns = dataTable.length;
    const stride = vecColumns * 4;
    for (let r = 0; r < numRows; r++) {
        const dst = r * stride;
        let norm = 0.0;
        for (let c = 0; c < numColumns; c++) {
            const v = dataTable[c][rowOffset + r];
            result[dst + c] = v;
            if (norms) {
                norm += v * v;
            }
        }
        for (let c = numColumns; c < stride; c++) {
            result[dst + c] = 0.0;
        }
        if (norms) {
            norms[r] = norm;
        }
    }
}
const MAX_CONCURRENCY_BATCHES = 10;
export default class GpuClustering {
    constructor(device, numPoints, numColumns, numCentroids) {
        this.device = device;
        this.numPoints = numPoints;
        this.numCentroids = numCentroids;
        this.vecColumns = Math.ceil(numColumns / 4);
        this.workgroupSize = workgroupSize;
        this.pointStride = this.vecColumns * 4;
        const storageLimit = Math.min(device.limits.maxBufferSize, device.limits.maxStorageBufferBindingSize);
        const workgroupsPerBatch = Math.max(1, Math.min(device.limits.maxComputeWorkgroupsPerDimension, // device dispatch limit
        Math.floor(storageLimit / (this.pointStride * this.workgroupSize * 4)), // point storage limit
        Math.ceil(numPoints / this.workgroupSize)));
        this.batchSize = workgroupsPerBatch * this.workgroupSize;
        this.numBatches = Math.ceil(numPoints / this.batchSize);
        this.concurrencyBatches = Math.min(MAX_CONCURRENCY_BATCHES, this.numBatches);
        this.concurrencyRuns = Math.ceil(this.numBatches / this.concurrencyBatches);
        const shader = device.createShaderModule({
            code: clusterWgsl(this.vecColumns),
        });
        const pipeline = device.createComputePipeline({
            layout: 'auto',
            compute: {
                module: shader,
                entryPoint: 'main',
            },
        });
        const pointsBackBuffer = new Float32Array(this.pointStride * this.batchSize);
        const centroidsBackBuffer = new Float32Array(this.pointStride * numCentroids);
        const centroidSqBackBuffer = new Float32Array(numCentroids);
        const uniformBackBuffer = new Uint32Array([0, numCentroids]);
        const pointsBuffers = [];
        const centroidsBuffer = device.createBuffer({
            size: centroidsBackBuffer.byteLength,
            usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
        });
        const centroidSqBuffer = device.createBuffer({
            size: centroidSqBackBuffer.byteLength,
            usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
        });
        const uniformBuffer = device.createBuffer({
            size: 256 * this.concurrencyBatches,
            usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.UNIFORM,
        });
        const resultBuffer = device.createBuffer({
            size: this.concurrencyBatches * this.batchSize * 4,
            usage: GPUBufferUsage.COPY_SRC | GPUBufferUsage.STORAGE,
        });
        const resultReadBackBuffer = device.createBuffer({
            size: this.concurrencyBatches * this.batchSize * 4,
            usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
        });
        const layout = pipeline.getBindGroupLayout(0);
        const bindGroups = [];
        for (let i = 0; i < this.concurrencyBatches; i++) {
            const pointsBuffer = device.createBuffer({
                size: pointsBackBuffer.byteLength,
                usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.STORAGE,
            });
            pointsBuffers.push(pointsBuffer);
            const entries = [
                {
                    binding: 0,
                    resource: {
                        buffer: uniformBuffer,
                        offset: i * 256,
                        size: 8,
                    },
                },
                {
                    binding: 1,
                    resource: pointsBuffer,
                },
                {
                    binding: 2,
                    resource: centroidsBuffer,
                },
                {
                    binding: 3,
                    resource: centroidSqBuffer,
                },
                {
                    binding: 4,
                    resource: {
                        buffer: resultBuffer,
                        offset: i * this.batchSize * 4,
                        size: this.batchSize * 4,
                    },
                },
            ];
            bindGroups.push(device.createBindGroup({
                layout,
                entries,
            }));
        }
        const gpuBuffers = {
            uniform: uniformBuffer,
            points: pointsBuffers,
            centroids: centroidsBuffer,
            centroidSq: centroidSqBuffer,
            result: resultBuffer,
            resultReadBack: resultReadBackBuffer,
        };
        const backBuffers = {
            uniform: uniformBackBuffer,
            points: pointsBackBuffer,
            centroids: centroidsBackBuffer,
            centroidSq: centroidSqBackBuffer,
        };
        this.resource = {
            pipeline,
            bindGroups,
            gpuBuffers,
            backBuffers,
            uploadedBatches: [],
        };
        logger.info(`GPU k-means kernel bootstrapped with batch ${workgroupsPerBatch}*${this.workgroupSize}*${this.numBatches}, concurrency: ${this.concurrencyBatches}, runs: ${this.concurrencyRuns}`);
    }
    async execute(points, centroids, labels) {
        const { device, numPoints, numCentroids, numBatches, batchSize, resource, concurrencyBatches, concurrencyRuns, pointStride, vecColumns, workgroupSize, } = this;
        // upload centroid data to gpu
        packVec4Data(resource.backBuffers.centroids, centroids, numCentroids, 0, vecColumns, resource.backBuffers.centroidSq);
        device.queue.writeBuffer(resource.gpuBuffers.centroids, 0, resource.backBuffers.centroids.buffer);
        device.queue.writeBuffer(resource.gpuBuffers.centroidSq, 0, resource.backBuffers.centroidSq.buffer);
        for (let i = 0; i < concurrencyRuns; i++) {
            const batchStart = i * concurrencyBatches;
            let resultCount = 0;
            for (let j = 0; j < concurrencyBatches; j++) {
                const batchIndex = batchStart + j;
                if (batchIndex >= numBatches) {
                    break;
                }
                const currentBatchSize = Math.min(numPoints - batchIndex * batchSize, batchSize);
                resultCount += currentBatchSize;
                // write this batch of point data to gpu
                if (resource.uploadedBatches[j] !== batchIndex) {
                    packVec4Data(resource.backBuffers.points, points, currentBatchSize, batchIndex * batchSize, vecColumns);
                    device.queue.writeBuffer(resource.gpuBuffers.points[j], 0, resource.backBuffers.points.buffer, 0, pointStride * currentBatchSize * 4);
                    resource.backBuffers.uniform[0] = currentBatchSize;
                    device.queue.writeBuffer(resource.gpuBuffers.uniform, 256 * j, resource.backBuffers.uniform.buffer, 0, 8);
                    resource.uploadedBatches[j] = batchIndex;
                }
            }
            const encoder = device.createCommandEncoder();
            const computePass = encoder.beginComputePass();
            computePass.setPipeline(resource.pipeline);
            for (let j = 0; j < concurrencyBatches; j++) {
                const batchIndex = batchStart + j;
                if (batchIndex >= numBatches) {
                    break;
                }
                const currentBatchSize = Math.min(numPoints - batchIndex * batchSize, batchSize);
                const groups = Math.ceil(currentBatchSize / workgroupSize);
                computePass.setBindGroup(0, resource.bindGroups[j]);
                computePass.dispatchWorkgroups(groups);
            }
            computePass.end();
            encoder.copyBufferToBuffer(resource.gpuBuffers.result, 0, resource.gpuBuffers.resultReadBack, 0, resultCount * 4);
            device.queue.submit([encoder.finish()]);
            await resource.gpuBuffers.resultReadBack.mapAsync(GPUMapMode.READ);
            const mapped = resource.gpuBuffers.resultReadBack.getMappedRange();
            labels.set(new Uint32Array(mapped, 0, resultCount), batchStart * batchSize);
            resource.gpuBuffers.resultReadBack.unmap();
        }
    }
    destroy() {
        this.resource.gpuBuffers.uniform.destroy();
        this.resource.gpuBuffers.centroids.destroy();
        this.resource.gpuBuffers.centroidSq?.destroy();
        this.resource.gpuBuffers.result.destroy();
        this.resource.gpuBuffers.resultReadBack.destroy();
        for (const buffer of this.resource.gpuBuffers.points) {
            buffer.destroy();
        }
    }
}
