import type { BufferReader } from './BufferReader.js';

export interface ChunkDecoder {
    init(): [number, number]; // [totals, itemSize]
    decode(offset: number, counts: number, buffer: Uint8Array): void;
}

export class StreamChunkDecoder {
    private reader: BufferReader;
    private decoders: ChunkDecoder[];
    private decodedTotals: Uint32Array;
    private currentIndex: number = 0;
    private currentTotals: number;
    private currentItemSize: number;

    constructor(reader: BufferReader) {
        this.reader = reader;
    }

    setDecoders(decoders: ChunkDecoder[]) {
        this.decoders = decoders;
        this.decodedTotals = new Uint32Array(decoders.length);
        const [totals, itemSize] = decoders[this.currentIndex].init();
        this.currentTotals = totals;
        this.currentItemSize = itemSize;
    }

    flush() {
        const { reader, decoders, decodedTotals, currentIndex, currentTotals, currentItemSize } = this;
        const stage = decoders[currentIndex];
        const decoded = decodedTotals[currentIndex];
        const counts = Math.min(currentTotals - decoded, (reader.remaining / currentItemSize) | 0);
        const buf = reader.read(counts * currentItemSize);
        stage.decode(decoded, counts, buf);
        decodedTotals[currentIndex] += counts;
        if (decodedTotals[currentIndex] === currentTotals) {
            this.currentIndex++;
            if (this.currentIndex < decoders.length) {
                const [totals, itemSize] = decoders[this.currentIndex]!.init();
                this.currentTotals = totals;
                this.currentItemSize = itemSize;
                this.flush();
            }
        }
    }
}
