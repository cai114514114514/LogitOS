# Runtime fonts

The disk image uses four checked-in, redistributable font subsets:

| File | Internal name | Derived from | Coverage | SHA-256 |
|---|---|---|---|---|
| `ui.ttf` | Logit UI Regular | Noto Sans SC `wght=400` | GB2312, ASCII, and selected CJK/fullwidth punctuation | `e773892e65aba7712d3e0a04d67da2a1092a4ad70057b4a64a58eff0b8fe8821` |
| `ui-bold.ttf` | Logit UI Bold | Noto Sans SC `wght=700` | identical codepoint set to `ui.ttf` | `e0c768b5279eb97741d35ac2ac872df66cb1ac542b85dbf7a85c04a1b26b89bf` |
| `mono.ttf` | Logit Mono Regular | Noto Sans Mono `wght=400 wdth=100` | printable ASCII plus non-breaking space | `360015a138312fd70b834a5157a7164602ccafa3ccac0ece12327c0bb3b0cd27` |
| `mono-bold.ttf` | Logit Mono Bold | Noto Sans Mono `wght=700 wdth=100` | identical codepoint set to `mono.ttf` | `fa25e03e0d6ace14132c45cbe31cfe4446a0b435df29294b57bb0865f76beaf8` |

All four are modified font software distributed under SIL Open Font License
1.1, not under Logit's MIT license. Their internal name tables retain upstream
copyright and license metadata. The complete notices, exact upstream commit,
source hashes, and unmodified source fonts are in `third_party/fonts/`.
The disk image also installs the complete OFL texts and source record under
`/licenses/fonts/`.

**The Bold pair is an INSTANCE, not a new download.** Both sources are variable
fonts; the only axes they carry are `wght` (and `wdth` on Mono). So the same
vendored file, the same licence and the same `tools/mkfont.py` produce both
weights, and `third_party/fonts/SHA256SUMS` is unchanged by their existence.

**There is no italic and it is not derivable from what is vendored.** Neither
source has an `ital` or a `slnt` axis, and neither Noto family ships an italic
upstream. Shearing the regular outlines is deliberately not done; the argument
is in `tools/mkfont.py`'s docstring.

**The four codepoint sets are two, not four:** each Bold face is subset to
exactly the same codepoints as its Regular twin. A Bold face covering less
would make `<strong>` fall back per character, so half a run would be bold and
half would not — a failure that reads as a shaping bug and is an asset bug.

## Both weights of a face have the same vertical metrics

`c/lib/text/ttf.c` reads `hhea` for ascent/descent/lineGap, and
`c/kernel/gui/text.c` puts the baseline at `ascent * px / upem` using the FIRST
font of the run's fallback set. A Bold face whose `hhea.ascent` differed from
its Regular twin's would therefore shift every bold line vertically against the
regular text beside it. Measured on the generated pair: `ui.ttf` and
`ui-bold.ttf` both report `ascent=1160 descent=-288 lineGap=0 upem=1000`;
`mono.ttf` and `mono-bold.ttf` both report `ascent=1069 descent=-293
lineGap=0`. Re-check this after any regeneration — it is a property of the
sources, not of this pipeline, and nothing else in the tree asserts it.

## Regeneration

Run `make regen-fonts` to rebuild all four with the pinned FontTools version in
`tools/requirements-fonts.txt`, and `make verify-fonts` to check both source and
output hashes. A normal `make` uses the tracked subsets and does not inspect
`/System/Library/Fonts`, other host fonts, or the network.

The hashes above were regenerated on 2026-08-28 and `make regen-fonts` is
reproducible against them again. It had stopped being: the previous tracked
bytes were built while this OS was called **Aether**, and `mkfont.py`'s strings
were later renamed to **Logit** without the fonts being rebuilt, so
`regen-fonts` rewrote the files and then failed its own `sha256sum -c`.
Measured before rebuilding: with `fonttools==4.61.1` every table except `name`
and `head` (whose `checksumAdjustment` follows `name`) was **byte-identical**
between the tracked files and a fresh build — `glyf`, `loca`, `hmtx`, `cmap`,
`hhea`, `OS/2`, `maxp`, `post`, `prep`, `gasp` all matched — so the rebuild
changed the internal family strings from `Aether UI`/`Aether Mono` to
`Logit UI`/`Logit Mono` and moved no pixel.
