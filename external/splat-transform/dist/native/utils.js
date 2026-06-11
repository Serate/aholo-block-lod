import { readFileSync } from 'node:fs';
import child_process from 'node:child_process';
import p from '../../package.json' with { type: 'json' };
function isMusl() {
    let musl = false;
    if (process.platform === 'linux') {
        musl = isMuslFromFilesystem();
        if (musl == null) {
            musl = isMuslFromReport();
        }
        if (musl == null) {
            musl = isMuslFromChildProcess();
        }
    }
    return musl;
}
function isFileMusl(f) {
    return f.includes('libc.musl-') || f.includes('ld-musl-');
}
function isMuslFromFilesystem() {
    try {
        return readFileSync('/usr/bin/ldd', 'utf-8').includes('musl');
    }
    catch {
        return null;
    }
}
function isMuslFromReport() {
    let report = null;
    if (typeof process.report?.getReport === 'function') {
        process.report.excludeNetwork = true;
        report = process.report.getReport();
    }
    if (!report) {
        return null;
    }
    if (report.header && report.header.glibcVersionRuntime) {
        return false;
    }
    if (Array.isArray(report.sharedObjects)) {
        if (report.sharedObjects.some(isFileMusl)) {
            return true;
        }
    }
    return false;
}
function isMuslFromChildProcess() {
    try {
        return child_process.execSync('ldd --version', { encoding: 'utf8' }).includes('musl');
    }
    catch {
        // If we reach this case, we don't know if the system is musl or not, so is better to just fallback to false
        return false;
    }
}
export function getNativePackageName() {
    let runtime = undefined;
    if (process.platform === 'win32') {
        runtime = 'msvc';
    }
    else if (process.platform === 'linux') {
        runtime = isMusl() ? 'musl' : 'gnu';
    }
    return `${p.name}-${process.platform}-${process.arch}${runtime ? `-${runtime}` : ''}`;
}
