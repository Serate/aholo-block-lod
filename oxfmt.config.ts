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
        '**/build/**',
        '**/dist/**',
        // website
        '**/.astro/**',
        '**/.generated/**',
        // externals
        '**/*.impl.ts',
        '**/wasm/**',
        '**/draco-loader/*.js',
        'external/splat-dev-server/**',
        'external/splat-transform/**',
    ],
});
