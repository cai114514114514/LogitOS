# Native GUI library fixtures

- `main.as`: host ABI transport oracle, forced GC, event reuse and input bounds.
- `guest.as`: actual rectangle/blit/text scanout, followed by QMP keyboard exit.
- `no-font.png`: actual release scanout from the initial private guest disk,
  `build/a3-cutover/typed-guest-gui-window/gui-window-release.ppm`. Both colored
  rectangles were correct while all text was absent because the disk omitted
  fonts. The required host negative control must reject this captured frame.

  Correction (2026-10-26, kept beside the original): that capture was never
  committed -- `.gitignore` excludes `/tests/`, so new files here need
  `git add -f` -- and `test-as-gui-lib-negctl` failed on every checkout with
  `FileNotFoundError` instead of exercising the oracle. The file now present is
  a fresh capture of the same failure, taken on the macOS dev host: the real
  typed guest harness (`tests/boot/run-as-typed.py --case gui-window`, both
  modes) with the five `/fonts/` entries left off the disk and everything else
  packaged as usual, accepting a frame only after it had stayed textless for
  3 s. Release scanout, 1280x800, from
  `build/nofont-capture-20261026-154945/gui-window-release.ppm`: both
  rectangles complete, text ink 0, program output and exit status as expected.

The guest packager now reads font and notice paths from the production Make
inventory. The scanout oracle checks complete colored rectangles, relative
positions, common display scale and visible text ink. ABI transport tests
separately verify the exact text bytes; the pixel check is not OCR.
