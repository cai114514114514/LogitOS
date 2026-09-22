# Glass readability refactor — 2026-09-22

The material entry point mixed clipping, allocation, two blur passes, geometry,
sampling and shading in one 352-line function. This change rewrites those
responsibilities by hand. No formatter or `.clang-format` configuration was used.
The public entry point is now 21 lines; public signatures and material behavior
are unchanged from the adaptive-glass implementation tested before this refactor.

## Source map

| File under `c/lib/gfx/` | Responsibility |
| --- | --- |
| `effects/openlogit_material.c` | Visible bounds, original lens geometry, scratch allocation, stage dispatch |
| `effects/glass_blur.c` | Horizontal and vertical backdrop sampling |
| `effects/glass_geometry.h` | Signed distance, coverage, normals, straight row spans |
| `effects/glass_shading.h` | Adaptive opacity, shared lighting and color composition |
| `effects/glass_render.c` | Refraction sampling and pixel writes |
| `effects/glass_internal.h` | Private stage data and contracts |
| `adapters/display_pixels.h` | Shared inline channel access and allocator helpers |
| `effects/openlogit_glass.c` | Existing optical lookup tables and integer square root |

The implicit `ctx` buffer aliases and `BDIV` macro are gone. Fast and general
rendering share the lighting and color formulas. Stage state uses named fields
instead of long positional argument lists. Comments retain the reasons for
offscreen geometry, clipped sampling, ring-buffer ordering and inline pixel work.
The two display-only host consumers use one material source list in the Makefile.

## Validation

`make BUILD=build-quality-glass -j6 all build-quality-glass/disk.img` passed,
including real kernel and ring-3 links. The default disk was not rebuilt.

`make BUILD=build-quality-glass test-glass test-glass-material test-glass-geometry
test-glass-geometry-asan test-fb-clip test-fb-native-present-host test-mk-wired`
passed. Evidence is in `build-quality-glass/readability-final-gates.log`:

- Optical arithmetic: 39,652 checks.
- Material: 206 checks, also under ASan/UBSan; patterned image hash remains
  `14544d285d307f91` for both fast and general paths.
- Geometry: 867 checks, also under ASan/UBSan; full image hash remains
  `1d72a955eb3f2e17` for both paths.
- Narrow-root, rebased-crop and static-tint controls failed their intended
  assertions. The existing display-present control also failed as intended.
- Display clipping passed; native presentation passed 21 checks.
- Build gate audit: 381 fragments, 380 reachable, one declared wrapper.

In the isolated guest, all 21 full screenshots match the pre-refactor inline
candidate outside the changing clock rectangle `[1232,10,1269,23]`, including
light glass above black content and dark glass above white content. Source image
hashes were unchanged by the harness and its QEMU process stopped afterward.
See `build-quality-glass/comparison-readable-pixels.json`.

For the same 131,764 requested glass pixels, guest WM glass-time medians over
three high-contrast frames changed from 14.118 to 11.582 ms in light mode and
12.926 to 10.433 ms in dark mode. These are small-sample observations with frame
variation, not a stable speedup claim or a measurement of every app's glass.
See `build-quality-glass/comparison-readable.json` and the raw
`candidate-readable-ui/result.json` beside it. No host wall-clock timings were
used for performance claims.

## Build cleanup

Referenced logs, results and screenshots were preserved in the
[validation archive](evidence/2026-09-22/README.md) before removing temporary
build directories. Historical commands above retain their original output paths.
