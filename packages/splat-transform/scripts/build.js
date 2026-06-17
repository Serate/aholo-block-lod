import { createRequire } from 'node:module';
import path from 'node:path';
import fs from 'node:fs/promises';
import { program } from 'commander';
import { spawnProcess, execCommand } from '@internal/utils/process.js';

const __dirname = import.meta.dirname;
const require = createRequire(import.meta.url);

program.command('build').action(async function () {
    await fs.rm('./dist', { recursive: true, force: true });
    await execCommand('tsc', {
        env: {
            ...process.env,
            PATH: path.join(__dirname, '../node_modules/.bin') + path.delimiter + process.env.PATH,
        },
    }).promise;
});

program.parse();
