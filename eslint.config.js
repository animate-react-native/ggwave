// The template shipped `eslintConfig` in package.json, which eslint 9 does not
// read, and @react-native/eslint-config@0.86 pulls eslint-plugin-ft-flow, which
// calls context.getAllComments, removed in eslint 9. So this lints the
// TypeScript directly instead, which is all this package contains.
const js = require('@eslint/js');
const tseslint = require('typescript-eslint');
const prettier = require('eslint-config-prettier/flat');

module.exports = tseslint.config(
  {
    ignores: [
      'lib/**',
      'nitrogen/generated/**', // regenerated wholesale by `bun run specs`
      'cpp/vendor/**',
      'example/**', // has its own config and its own toolchain
      '.build/**',
    ],
  },
  js.configs.recommended,
  ...tseslint.configs.recommended,
  prettier,
  {
    rules: {
      '@typescript-eslint/no-unused-vars': ['warn', { argsIgnorePattern: '^_' }],
    },
  },
  {
    // The config files are CommonJS and run in node, not in the app.
    files: ['**/*.config.js'],
    languageOptions: {
      sourceType: 'commonjs',
      globals: { require: 'readonly', module: 'writable', __dirname: 'readonly' },
    },
    rules: { '@typescript-eslint/no-require-imports': 'off' },
  },
);
