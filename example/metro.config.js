const { getDefaultConfig, mergeConfig } = require('@react-native/metro-config');
const path = require('path');

const pkg = require('../package.json');

// The library is the parent directory, so Metro has to watch outside the app.
const root = path.resolve(__dirname, '..');

// The library keeps its own node_modules for nitrogen and typechecking, so
// react, react-native and react-native-nitro-modules exist twice. Two copies of
// react-native in one bundle is a runtime crash rather than a warning, so the
// library's copies are blocked and everything resolves to the app's.
const shared = Object.keys(pkg.peerDependencies ?? {});
const escape = (s) => s.replace(/[/\-\\^$*+?.()|[\]{}]/g, '\\$&');

/**
 * @type {import('@react-native/metro-config').MetroConfig}
 */
const config = {
  watchFolders: [root],
  resolver: {
    blockList: shared.map(
      (name) => new RegExp(`^${escape(path.join(root, 'node_modules', name))}\\/.*$`),
    ),
    extraNodeModules: {
      ...shared.reduce((acc, name) => {
        acc[name] = path.join(__dirname, 'node_modules', name);
        return acc;
      }, {}),
      // The library itself, resolved to its source. Its package.json points
      // `react-native` at src/index, so edits are picked up with no build step.
      [pkg.name]: root,
    },
  },
};

module.exports = mergeConfig(getDefaultConfig(__dirname), config);
