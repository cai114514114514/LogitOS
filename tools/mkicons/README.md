# tools/mkicons

Host tool that draws the OS's app icon masters (design_language.md 3.7.4, 3.7.5):
12 app tiles plus `logit.png`, the Logit mark tile, each 256 x 256 RGBA.

    mkicons [--sheet SHEET.png [--sheet-only]] [--only STEM] OUTDIR

`make BUILD=build` builds it (`$(MKICONS)`, tests/settings2.mk), generates the icons into
`$(BUILD)/icons/<stem>.png` and packs them as `/icons/<stem>.png`. Nothing generated is tracked
(`*.png` is ignored). `make BUILD=build ui-art-sheet` writes `$(BUILD)/icons/sheet.png`, the
contact sheet: all 13 icons at 256, 64 and 32 px on `#F4F4F8` and `#1E1E24`.

## Files

| File | What |
|---|---|
| `mkicons.c` | tile (squircle, gradient, bottom shade, rim), glyph layer renderer, PNG and sheet output |
| `svgpath.c` | SVG path `d` parser (M L H V C Q Z, absolute and relative) mapping onto `gfx_move_to` ... `gfx_cubic_to`; unity-included by `mkicons.c` |
| `glyphs_svg.inc` | the 13 glyphs as path data in a 100 x 100 box, plus per-layer colour role, alpha, stroke, gradient and transform |

Colours, ink and the glyph key come from `c/include/ui/logit_apps.h`, the same table the Dock and
Spotlight read.

## Iterating on art

Edit a path string in `glyphs_svg.inc`, then

    make BUILD=build ui-art-sheet        # rebuilds the tool, redraws, writes the sheet

and look at `build/icons/sheet.png`. A layer is one fill or one stroke: `.d` the path, `.role`
(`R_INK`, `R_ACCENT`, `R_RGB`), `.alpha`, `.stroke` (box units; round caps and joins), an optional
vertical gradient (`.grad`, `.gy0`, `.gy1`, `.rgb2`, `.alpha2`, `.fade`) and `.rot` `.sc` `.tx`
`.ty` about the box centre. The header of `glyphs_svg.inc` has the full list. The box is placed at
56 % of the tile, centred.

## Tile recipe (3.7.4)

- squircle r = 0.225 x size, smoothing 154/256 (`gfx_path_squircle`), full-bleed 2-stop vertical
  gradient (the Logit tiles use the 135 degree 3-stop brand gradient of 3.1.6), dithered;
- glyph at 56 %, white or `#1D1D23` on a light tile;
- soft bottom shade, `#000000` 0 to 20/255 over the bottom 6 %, kept inside the squircle;
- 1 dev px inner rim, `#FFFFFF` @ 90 at the top fading to @ 0 at 50 % of the height (an even-odd
  ring of two squircles, so it follows the corner);
- edge hairline on a white tile (top stop luma > 0.85: 时钟, 预览, 文本编辑) of `#000000` @ 20 at
  the top to @ 30 at the bottom, and on a near-black tile (bottom stop luma < 0.10: 活动监视器,
  终端) of `#FFFFFF` @ 34, all round. It is 256/54 master px wide, so after the box filter it is
  one dev px at the Dock's 54 (about two at 2x): without it those tiles vanish on light surfaces
  (Finder `#F3F4F8`, a white sidebar) and in the dark Dock or dark Finder;
- no baked drop shadow: the Dock draws the contact shadow.

Glyph weight: strokes are about 5-6 % of the tile (the `</>`, the sparkline, the Logit mark's
ring on its tile), so no glyph reads as a hairline next to the solid ones at 32-54 px. The 14 pt
menu-bar mark keeps its own thin ratio (3.7.5); that is the kernel's, not this tool's.

## Determinism and the gate

The output is byte-identical run to run (integer rasteriser, `-ffp-contract=off`, stored-deflate
PNG encoder). `make BUILD=build test-ui-art-assets` checks that, the sizes, the corner, the
gradient ends, glyph coverage, the rim and the edge hairline; its control builds the tool with
`-DUI_ART_NEGCTL` (no rim, no edge) and must see `FAIL rim`.
