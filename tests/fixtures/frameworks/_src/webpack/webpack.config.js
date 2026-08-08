const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');

// Webpack's own defaults, stated rather than tuned: production mode, a single
// entry, and code splitting left entirely to the dynamic import in src/index.js
// so the emitted runtime is webpack's, not a configuration of mine.
module.exports = {
  entry: './src/index.js',
  output: {
    path: path.resolve(__dirname, 'dist'),
    filename: '[name].[contenthash:8].js',
    chunkFilename: '[name].[contenthash:8].chunk.js',
    clean: true,
  },
  plugins: [new HtmlWebpackPlugin({ template: './src/index.html', scriptLoading: 'defer' })],
};
