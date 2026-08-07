# Font test fixtures

Real fonts, used only by the host-side tests (`make test-font`, `test-font-otl`,
`test-font-color`, `test-font-fuzz`, `test-font-control`). **None of these ship
on the disk image** — the fonts LogitOS actually installs are `fsroot/fonts/`,
built by `tools/mkfont.py`. These exist because the parsers have to be checked
against font files somebody else made, with all the shapes real tooling emits.

| File | What it is for | Derived from | Licence |
|---|---|---|---|
| `SourceSans3-Regular.otf` | **CFF** outlines: Type 2 charstrings, global + local subroutines, hintmasks, flex. Also GSUB/GPOS/GDEF/BASE. 2478 glyphs, unmodified upstream release. | [adobe-fonts/source-sans](https://github.com/adobe-fonts/source-sans) `OTF/SourceSans3-Regular.otf` | SIL OFL 1.1 |
| `cid-cff-subset.otf` | **CID-keyed CFF**: FDArray (11 font DICTs) + FDSelect, per-FD Private DICTs and local subroutines, charset format 2. 1033 glyphs. | Subset of [adobe-fonts/source-han-sans](https://github.com/adobe-fonts/source-han-sans) `SubsetOTF/CN/SourceHanSansCN-Regular.otf`, via `pyftsubset` | SIL OFL 1.1 |
| `colr-emoji-subset.ttf` | **COLR/CPAL** layered colour glyphs (version 0). 560 glyphs, 113 with colour layers. | Subset of [mozilla/twemoji-colr](https://github.com/mozilla/twemoji-colr) v0.7.0 `Twemoji.Mozilla.ttf` | Font build MIT; the emoji artwork is Twemoji, CC-BY 4.0 |
| `cbdt-emoji-subset.ttf` | **CBDT/CBLC** bitmap strike (PNG, imageFormat 17, indexFormat 1) and a font with *no outline table at all*. 32 glyphs. | Subset of [googlefonts/noto-emoji](https://github.com/googlefonts/noto-emoji) `NotoColorEmoji.ttf` | SIL OFL 1.1 |
| `kern-subset.ttf` | The legacy **`kern`** table (plenty of fonts still carry only that), plus GPOS/GDEF/MATH. Also the base for the synthesised sbix fixture. 391 glyphs. | Subset of DejaVu Sans (`/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`) | DejaVu licence (Bitstream Vera + Arev, permissive) |

Subsets were produced with `pyftsubset --layout-features='*' --no-hinting
--notdef-outline`, which keeps the layout tables and the outline flavour intact
while dropping glyphs; the resulting files are modified font software and carry
their upstream name/licence records.

There is **no committed sbix fixture**. The only sbix font in wide use is Apple
Color Emoji, which is not redistributable, so `tests/unit/font_color_ref.py
--make-sbix` synthesises one at test time: it grafts a real `sbix` table onto
`kern-subset.ttf` with two strikes of hand-built PNGs and a `dupe` record, which
between them reach every branch of `sbix_lookup`.

Corrupted fonts are not committed either. `tests/unit/font_fuzz.c` builds them
from these files — every truncation, hundreds of thousands of bit-flipped
variants, and a set of *named* corruptions (a `ttcf` header, an unknown sfnt
version, each required table pointed past EOF, an outline table truncated to
four bytes) that must be rejected rather than trusted.
