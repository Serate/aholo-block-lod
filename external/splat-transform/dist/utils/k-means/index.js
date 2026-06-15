import { clusterAverage } from '../../native/index.js';
import { logger } from '../index.js';
import GpuClustering from './GpuClustering.js';
// in the 1d case we use quantile-based initialization for better handling of skewed data
function initializeCentroids1D(data, centroids) {
    const n = data.length;
    const k = centroids.length;
    // Sort data to compute quantiles
    const sorted = Float32Array.from(data).sort((a, b) => a - b);
    for (let i = 0; i < k; ++i) {
        // Place centroid at the center of its expected cluster region
        const quantile = (2 * i + 1) / (2 * k);
        const index = Math.min(Math.floor(quantile * n), n - 1);
        centroids[i] = sorted[index];
    }
}
// use floyd's algorithm to pick m unique random indices from 0..n-1
function pickRandomIndices(n, m) {
    const chosen = new Set();
    for (let j = n - m; j < n; j++) {
        const t = Math.floor(Math.random() * (j + 1));
        chosen.add(chosen.has(t) ? j : t);
    }
    return [...chosen];
}
function initializeCentroids(dataTable, centroids) {
    const indices = pickRandomIndices(dataTable[0].length, centroids[0].length);
    for (let i = 0; i < centroids[0].length; i++) {
        for (let j = 0; j < dataTable.length; j++) {
            centroids[j][i] = dataTable[j][indices[i]];
        }
    }
}
// https://github.com/playcanvas/splat-transform/blob/main/src/lib/spatial/k-means.ts
export async function kMeans(points, k, iterations, device) {
    const numRows = points.length > 0 ? points[0].length : 0;
    if (numRows < k) {
        return {
            centroids: points,
            // use a typed array here so downstream code can rely on
            // labels supporting subarray(), even in this early-return
            // path used for very small datasets.
            labels: new Uint32Array(numRows).map((_, i) => i),
        };
    }
    const centroids = points.map(_ => new Float32Array(k));
    if (points.length === 1) {
        initializeCentroids1D(points[0], centroids[0]);
    }
    else {
        initializeCentroids(points, centroids);
    }
    const gpuClustering = new GpuClustering(device, numRows, points.length, k);
    const labels = new Uint32Array(numRows);
    let converged = false;
    let steps = 0;
    while (!converged) {
        logger.info(`kmeans iteration ${steps + 1}`);
        await gpuClustering.execute(points, centroids, labels);
        clusterAverage(points, labels, k, centroids);
        steps++;
        if (steps >= iterations) {
            converged = true;
        }
    }
    gpuClustering.destroy();
    return {
        centroids,
        labels,
    };
}
