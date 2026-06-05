import { defineConfig } from 'oxlint';

export default defineConfig({
    env: {
        browser: true,
        node: true,
        es2026: true,
    },
    ignorePatterns: [
        '.github/*',
        '**/build/**',
        '**/dist/**',
        '**/bin/*',
        '**/scripts/**',
        // website
        '**/.astro/**',
        '**/.generated/**',
        // externals
        '**/*.impl.ts',
        '**/wasm/*',
        '**/draco-loader/*.js',
        'external/splat-dev-server/**',
        'external/splat-transform/**',
    ],
    rules: {
        'no-console': 'off',
        'guard-for-in': 'off',
        'no-shadow': 'off',
        'no-use-before-define': 'off',
        'typescript/prefer-for-of': 'off',
        'no-loss-of-precision': 'off',
        'no-unused-expressions': [
            'deny',
            {
                allowShortCircuit: true,
                allowTernary: true,
            },
        ],
        'unicorn/no-new-array': 'off',
        'no-extra-boolean-cast': 'off',
        'typescript/no-this-alias': 'off',
        'typescript/no-duplicate-enum-values': 'off',
        'erasing-op': 'off',
    },
});
