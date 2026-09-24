# webidl_shape

- `shape.js`, `shape.html`: written for this tree (MIT, like the rest of `tests/`).
- `chrome-153.json`: the output of `shape.html` in Google Chrome 153 on macOS,
  captured 2026-09-24 with Playwright over `http://127.0.0.1`. It is a
  measurement of that browser's object model (interface names, prototype
  chains, member kinds), not copied source; it contains no third-party code.
