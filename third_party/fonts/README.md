# Vendored font sources

Aether vendors the two unmodified variable TrueType inputs used to build its
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
are licensed under SIL Open Font License 1.1. The Aether subsets are modified
font software, are renamed internally to `Aether UI` and `Aether Mono`, and
remain under the OFL rather than the repository's MIT license.

To reproduce the checked-in subsets in a clean Python environment:

```sh
python3 -m pip install -r tools/requirements-fonts.txt
make regen-fonts
make verify-fonts
```

Expected output hashes are recorded in `fsroot/fonts/README.md`.
