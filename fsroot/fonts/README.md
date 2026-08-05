# Runtime fonts

The disk image uses two checked-in, redistributable font subsets:

| File | Internal family | Derived from | Coverage | SHA-256 |
|---|---|---|---|---|
| `ui.ttf` | Aether UI | Noto Sans SC Regular | GB2312, ASCII, and selected CJK/fullwidth punctuation | `f72e3acc0c82771bc018eee807a1073ba4e0ccc359b2cf311245a91eca36a08f` |
| `mono.ttf` | Aether Mono | Noto Sans Mono Regular | Printable ASCII plus non-breaking space | `8ffa7c0dc73933d28ce7d9bf5fd31b0d6dc6b2ee01908cb7dd4689265919c0d8` |

Both files are modified font software distributed under SIL Open Font License
1.1, not under Aether's MIT license. Their internal name tables retain upstream
copyright and license metadata. The complete notices, exact upstream commit,
source hashes, and unmodified source fonts are in `third_party/fonts/`.
The disk image also installs the complete OFL texts and source record under
`/licenses/fonts/`.

Run `make regen-fonts` to rebuild them with the pinned FontTools version in
`tools/requirements-fonts.txt`, and `make verify-fonts` to check both source and
output hashes. A normal `make` uses the tracked subsets and does not inspect
`/System/Library/Fonts`, other host fonts, or the network.
