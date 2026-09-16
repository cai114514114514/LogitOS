# Native GUI library fixtures

- `main.as`: host ABI transport oracle, forced GC, event reuse and input bounds.
- `guest.as`: actual rectangle/blit/text scanout, followed by QMP keyboard exit.
- `no-font.png`: actual release scanout from the initial private guest disk,
  `build/a3-cutover/typed-guest-gui-window/gui-window-release.ppm`. Both colored
  rectangles were correct while all text was absent because the disk omitted
  fonts. The required host negative control must reject this captured frame.

The guest packager now reads font and notice paths from the production Make
inventory. The scanout oracle checks complete colored rectangles, relative
positions, common display scale and visible text ink. ABI transport tests
separately verify the exact text bytes; the pixel check is not OCR.
