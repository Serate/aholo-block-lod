export class BufferReader {
    head = 0;
    tail = 0;
    buffer: Uint8Array;
    view: DataView;

    get remaining(): number {
        return this.tail - this.head;
    }

    constructor(buffer: Uint8Array = new Uint8Array()) {
        this.buffer = buffer;
        this.view = new DataView(this.buffer.buffer);
    }

    private grow(required: number) {
        const newCap = Math.max(required, this.buffer.length * 2);
        const next = new Uint8Array(newCap);
        next.set(this.buffer.subarray(this.head, this.tail), 0);

        this.tail -= this.head;
        this.head = 0;
        this.buffer = next;
        this.view = new DataView(next.buffer);
    }

    private compact() {
        if (this.head === 0) {
            return;
        }
        this.buffer.copyWithin(0, this.head, this.tail);
        this.tail -= this.head;
        this.head = 0;
    }

    write(chunk: Uint8Array) {
        const remaining = this.tail - this.head;
        const required = remaining + chunk.length;
        if (this.buffer.length < required) {
            this.grow(required);
        } else if (this.head > 0 && this.buffer.length - this.tail < chunk.length) {
            this.compact();
        }

        this.buffer.set(chunk, this.tail);
        this.tail += chunk.length;
    }

    read(counts: number): Uint8Array {
        const head = this.head;
        const tail = (this.head = head + counts);
        return this.buffer.subarray(head, tail);
    }
}
