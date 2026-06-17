import path from 'node:path';
import { createRequire } from 'node:module';
import { program } from 'commander';
import { spawnProcess } from '@internal/utils/process.js';

const __dirname = import.meta.dirname;
const require = createRequire(import.meta.url);

program
    .command('build')
    .option('--preset <string>', 'CMake preset', 'default')
    .requiredOption('--target <string>', 'platform target')
    .action(async function (options) {
        await spawnProcess(
            'node',
            [
                require.resolve('cmake-js/bin/cmake-js'),
                'build',
                `--CDBINDING_BINARY_DIR=${path.resolve(`../splat-transform-${options.target}`)}`,
                '--',
                '--preset',
                options.preset,
            ],
            {
                cwd: process.cwd(),
                env: {
                    ...process.env,
                    FORCE_COLOR: 1,
                    PATH: path.join(__dirname, '.`/node_modules/.bin') + path.delimiter + process.env.PATH,
                },
            },
        ).promise;
    });

program.parse();
