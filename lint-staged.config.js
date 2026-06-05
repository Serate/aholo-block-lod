export default {
    '**/*.{ts,tsx}': ['oxfmt --disable-nested-config --write --no-error-on-unmatched-pattern', 'oxlint --fix'],
    '**/*.{md,MD,json,txt,yml,yaml}': ['oxfmt --disable-nested-config --write --no-error-on-unmatched-pattern'],
};
