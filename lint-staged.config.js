export default {
    '**/*.{ts,tsx}': [
        'oxfmt --write --disable-nested-config --no-error-on-unmatched-pattern',
        'oxlint --fix --disable-nested-config --no-error-on-unmatched-pattern',
    ],
    '**/*.{md,MD,json,txt,yml,yaml}': ['oxfmt --write --disable-nested-config --no-error-on-unmatched-pattern'],
};
