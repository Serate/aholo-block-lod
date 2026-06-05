export default {
    '**/*.{ts,tsx}': ['oxfmt --write --no-error-on-unmatched-pattern', 'oxlint --fix'],
    '**/*.{md,MD,json,txt,yml,yaml}': ['oxfmt --write --no-error-on-unmatched-pattern'],
};
