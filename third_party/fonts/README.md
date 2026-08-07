# Vendored font sources

Logit vendors the two unmodified variable TrueType inputs used to build its
checked-in font subsets. They came from the official Google Fonts repository at
commit `2796410152d4f9524b68ed46e69c1b60f8e0f7c3`, imported on 2026-08-05.

| Local file | Upstream path | SHA-256 |
|---|---|---|
| `NotoSansSC-VF.ttf` | `ofl/notosanssc/NotoSansSC[wght].ttf` | `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da` |
| `NotoSansMono-VF.ttf` | `ofl/notosansmono/NotoSansMono[wdth,wght].ttf` | `2cb2adb378a8f574213e23df697050b83c54c27df465a2015552740b2769a081` |

The corresponding upstream `OFL.txt` and `METADATA.pb` files are preserved as
`OFL-*` and `METADATA-*` in this directory. Raw upstream URLs can be reproduced
by appending the paths above to:

```text
https://raw.githubusercontent.com/google/fonts/2796410152d4f9524b68ed46e69c1b60f8e0f7c3/
```

Noto Sans SC carries the Adobe copyright notice and reserves the font name
`Source`. Noto Sans Mono carries the Noto Project Authors copyright notice. Both
are licensed under SIL Open Font License 1.1. The Logit subsets are modified
font software, are renamed internally to `Logit UI` and `Logit Mono`, and
remain under the OFL rather than the repository's MIT license.

To reproduce the checked-in subsets in a clean Python environment:

```sh
python3 -m pip install -r tools/requirements-fonts.txt
make regen-fonts
make verify-fonts
```

Expected output hashes are recorded in `fsroot/fonts/README.md`.

## DejaVu Sans — the shaping font

`DejaVuSans.ttf` is vendored **unmodified** and shipped to the disk image as
`/fonts/text.ttf`. It is here because the two Noto subsets above carry no
Arabic and no Hebrew, and because subsetting stripped their GSUB/GPOS/GDEF
tables — so with only those two fonts the shaper (`c/lib/text/shape.c`) has
nothing to apply and Arabic still renders as disconnected isolated letters.

| Local file | Provenance | SHA-256 |
|---|---|---|
| `DejaVuSans.ttf` | Debian `fonts-dejavu-core` 2.37-8build1, `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf` | `b4c632e3cdf9acc7f28758fb5a323c8524d7fc6660d46904d9b6cbe2809c419c` |
| `LICENSE-DejaVu.txt` | the same package's `/usr/share/doc/fonts-dejavu-core/copyright` | `63d3ba759d12804c5b31a9d5940d855c1820d1f5999e6b0872eb1c7ff045fbc9` |

Upstream is <https://dejavu-fonts.github.io/>. It is under the Bitstream Vera
Fonts License plus the Arev fonts copyright, both of which permit
redistribution; the rename clause applies only to modified fonts and we do not
modify it, which is also why there is no `mkfont.py` rule for it. It carries
`arab`, `hebr`, `latn`, `grek`, `cyrl` and sixteen other scripts in GSUB and
GPOS, plus a legacy `kern` table — which makes it the same font
`make test-shape` runs the HarfBuzz differential against, so what the device
draws and what the differential checks are the same glyphs.
