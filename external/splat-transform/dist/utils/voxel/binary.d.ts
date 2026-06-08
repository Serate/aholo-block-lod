export type VoxelNodeEncoding = 'raw' | 'compact';
export interface DecodedVoxelBinary {
    nodes: Uint32Array;
    leafData: Uint32Array;
}
export declare function encodeRawVoxelBinary(nodes: Uint32Array, leafData: Uint32Array): Uint8Array;
export declare function encodeCompactVoxelBinary(nodes: Uint32Array, leafData: Uint32Array, numInteriorNodes: number, numMixedLeaves: number): Uint8Array;
export declare function decodeCompactVoxelBinary(binary: Uint8Array): DecodedVoxelBinary;
