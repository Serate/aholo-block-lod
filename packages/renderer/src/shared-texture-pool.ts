import { SourceTexture, TextureDimension, TextureViewDimension, TextureFormat } from '@qunhe/egs';

function pageKey(blockId: number, chunk: number): string {
    return `${blockId}:${chunk}`;
}

interface PageEntry {
    blockId: number;
    chunk: number;
    lastUsed: number;
}

/**
 * Global texture page pool shared by all blocks.
 *
 * Each page is one layer of a 2D array texture (256×256 RGBA32UI),
 * storing up to 16384 GS attributes in packed Uint32 format.
 */
export class SharedTexturePool {
    readonly maxPages: number;
    readonly pageSplats = 16384;
    readonly pageWidth = 256;
    readonly pageHeight = 256;

    readonly texture: SourceTexture;

    private pageMap = new Map<string, number>();
    private entries: (PageEntry | undefined)[];
    private freelist: number[];
    private frame = 0;

    constructor(maxPages: number) {
        this.maxPages = maxPages;
        this.entries = new Array(maxPages);
        this.freelist = Array.from({ length: maxPages }, (_, i) => i);

        this.texture = new SourceTexture(
            TextureDimension.D2,
            TextureViewDimension.D2Array,
            TextureFormat.Rgba32Uint,
            this.pageWidth,
            this.pageHeight,
            maxPages,
            false,
            false,
        );
    }

    newFrame(): void {
        this.frame++;
    }

    /** Returns the page index for (blockId, chunk), or -1 if not resident. */
    getPage(blockId: number, chunk: number): number {
        const key = pageKey(blockId, chunk);
        const page = this.pageMap.get(key);
        if (page === undefined) return -1;
        const entry = this.entries[page];
        if (entry) entry.lastUsed = this.frame;
        return page;
    }

    /** Allocate a page for (blockId, chunk), evicting LRU if full. Returns -1 on failure. */
    allocPage(blockId: number, chunk: number): number {
        const key = pageKey(blockId, chunk);
        const existing = this.pageMap.get(key);
        if (existing !== undefined) {
            const entry = this.entries[existing];
            if (entry) entry.lastUsed = this.frame;
            return existing;
        }
        let page = this.freelist.pop();
        if (page === undefined) {
            page = this.evictPage();
        }
        if (page === undefined) return -1;
        this.pageMap.set(key, page);
        this.entries[page] = { blockId, chunk, lastUsed: this.frame };
        return page;
    }

    /** Free the page for (blockId, chunk). */
    freePage(blockId: number, chunk: number): void {
        const key = pageKey(blockId, chunk);
        const page = this.pageMap.get(key);
        if (page === undefined) return;
        this.pageMap.delete(key);
        this.entries[page] = undefined;
        this.freelist.push(page);
    }

    /** Upload packed Uint32 data to a specific page layer. */
    uploadPage(page: number, data: Uint32Array): void {
        if (page < 0 || page >= this.maxPages) return;
        this.texture.setLevelLayerData(data, 0, page);
    }

    /** Returns all resident pages in LRU order (oldest first). */
    getLRUOrder(): { blockId: number; chunk: number; page: number }[] {
        const list: { blockId: number; chunk: number; page: number; lastUsed: number }[] = [];
        for (let p = 0; p < this.maxPages; p++) {
            const entry = this.entries[p];
            if (entry) {
                list.push({ blockId: entry.blockId, chunk: entry.chunk, page: p, lastUsed: entry.lastUsed });
            }
        }
        list.sort((a, b) => a.lastUsed - b.lastUsed);
        return list;
    }

    get usedPages(): number {
        return this.maxPages - this.freelist.length;
    }

    get freePages(): number {
        return this.freelist.length;
    }

    // ── private ──

    private evictPage(): number | undefined {
        let oldest = -1;
        let oldestFrame = Infinity;
        for (let p = 0; p < this.maxPages; p++) {
            const entry = this.entries[p];
            if (entry && entry.lastUsed < oldestFrame) {
                oldestFrame = entry.lastUsed;
                oldest = p;
            }
        }
        if (oldest < 0) return undefined;
        const entry = this.entries[oldest];
        if (entry) {
            this.pageMap.delete(pageKey(entry.blockId, entry.chunk));
        }
        this.entries[oldest] = undefined;
        return oldest;
    }
}
