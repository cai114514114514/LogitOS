# AUI host golden store

`<scene>@<pct>.ppm.gz` and `<scene>@<pct>.v2.ppm.gz`: what ring-3 `aui.c` draws
for four scenes at 100, 125, 150 and 200 %, through the host harness in
`tests/unit/aui_host/` (see `tests/aui-host-harness.mk`). Binary P6, gzip with
mtime 0 and no stored name (`tools/ppm_diff.py gzip IN OUT`), so regenerating
unchanged output reproduces the same bytes. 32 files, about 300 KB.

| scene         | content                                                                    |
|---------------|----------------------------------------------------------------------------|
| `s1_controls` | buttons (default, hover, pressed, disabled), toggle, checkbox, chevron row, segmented control, text field with caret, slider |
| `s2_rows`     | list rows with ellipsis, mixed 13 / 17 px labels, a wide-glyph row         |
| `s3_glass`    | `aui_glass` over a gradient and stripes, a card with `aui_shadow_ex`        |
| `s4_sheet`    | `aui_dialog_begin` sheet 135 ms (half of `AUI_T_SLOW`) into its fade, on the fake clock |

Text is the harness's deterministic box glyphs, not a font, so a golden moves
when layout or paint moves and never when a font does.

## Two modes

* `<scene>@<pct>.ppm.gz`: INFO = 0, the legacy kernel. No v2 handler is active.
* `<scene>@<pct>.v2.ppm.gz`: INFO = the OR of the `AUI_HOST_V2` bits of every
  `tests/unit/aui_host/v2_*.c` file, with their handlers registered. Identical to
  the legacy file until the first `v2_*.c` lands.

## Checking

    make BUILD=build test-aui-host

renders all 32 cases into `build/aui_host/render/` and runs
`tools/ppm_diff.py <render> <golden> --tol 0 --max-bad 0` on each. A failure
leaves `build/aui_host/diff_<name>.ppm` (bad pixels magnified) behind. A
per-scene tolerance is not used; if one is ever needed it goes in the Makefile
fragment with the reason beside it.

## Updating, when a pixel change is intended

    make BUILD=build test-aui-host GOLDEN_UPDATE=1

writes the goldens and lists the ones that changed. Then look before you
commit:

    python3 tools/ppm_diff.py build/aui_host/render/<name>.ppm <old golden from git> --diff-out /tmp/d.ppm

and quote the max delta and changed-pixel count per mode in the commit message.
Regenerate goldens on rebase; never hand-merge them.

Adoption items add v2 handlers as `tests/unit/aui_host/v2_<name>.c` (see
`AUI_HOST_V2` in `aui_host.h`); that changes only the `.v2` goldens.
