const path = require('path');

const pkg = require('../package.json');

/**
 * The library is the parent directory rather than an entry in node_modules, so
 * autolinking is pointed at it explicitly.
 *
 * It is not a dependency in package.json on purpose: bun's `link:` protocol
 * means the global link registry, not a relative path, and `file:..` copies the
 * directory, which would leave the app building a stale snapshot of the library
 * after every edit. Metro (metro.config.js) and TypeScript (tsconfig.json) are
 * pointed at the same place the same way.
 */
module.exports = {
  dependencies: {
    [pkg.name]: {
      root: path.resolve(__dirname, '..'),
    },
  },
};
