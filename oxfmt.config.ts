import { defineConfig } from 'oxfmt';

export default defineConfig({
    printWidth: 120,
    singleQuote: true,
    arrowParens: 'avoid',
    sortPackageJson: false,
    endOfLine: 'lf',
    ignorePatterns: [
        '*.log',
        '.env',
        '.env.*',
        '.codex/',
        '.vscode/',
        '**/*.impl.ts',
        '**/wasm/**',
        '**/draco-loader/*.js',
        '**/build/**',
        '**/dist/**',
        '**/.astro/**',
        '**/.generated/**',
        'external/splat-dev-server/**',
        'external/splat-transform/**',
    ],
});
