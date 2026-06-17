export class Logger {
    prefix: string = '';
    silent: boolean = false;

    private format(msg: string) {
        return this.prefix ? `${this.prefix} ${msg}` : msg;
    }

    info(msg: string, force: boolean = false) {
        if (this.silent && !force) {
            return;
        }
        console.log(this.format(msg));
    }

    warn(msg: string, force: boolean = false) {
        if (this.silent && !force) {
            return;
        }
        console.warn(this.format(msg));
    }

    error(msg: string, force: boolean = false) {
        if (this.silent && !force) {
            return;
        }
        console.error(this.format(msg));
    }

    time(label: string) {
        if (this.silent) {
            return;
        }
        console.time(this.format(label));
    }

    timeEnd(label: string) {
        if (this.silent) {
            return;
        }
        console.timeEnd(this.format(label));
    }
}

export const logger = new Logger();
