import { defineConfig } from 'oxfmt';

export default defineConfig({
    printWidth: 120,
    singleQuote: true,
    arrowParens: 'avoid',
    sortPackageJson: false,
    endOfLine: 'lf',
    ignorePatterns: [
        'node_modules/',
        '.pnpm-store/',
        'pnpm-lock.yaml',
        '*.log',
        '.env',
        '.env.*',
        '.codex/',
        '.vscode/',
        '.astro/',
        '.generated/',
        '.github/',
        'dist/',
        'external/',
    ],
});
