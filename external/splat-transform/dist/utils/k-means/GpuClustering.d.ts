export default class GpuClustering {
    private device;
    private numPoints;
    private numCentroids;
    private batchSize;
    private resource;
    private numBatches;
    private concurrencyBatches;
    private concurrencyRuns;
    private workgroupSize;
    private pointStride;
    private vecColumns;
    constructor(device: GPUDevice, numPoints: number, numColumns: number, numCentroids: number);
    execute(points: Float32Array[], centroids: Float32Array[], labels: Uint32Array): Promise<void>;
    destroy(): void;
}
