# Native button automatic dimensions consume authored content geometry

An automatic native button retained its fixed intrinsic chrome size after
computed padding and line-height grew. Its children already used those values,
so the content width became too narrow and the lines could extend below the
button. `layout.c::ctl_metrics` now keeps the native default minimum while
also reserving the measured label plus computed horizontal edges, and the used
line-height plus vertical edges. Existing explicit CSS dimensions and width
constraints still run afterward. Input elements keep their separate metrics.

This is the only changed production file. The patch adds no style field or
header dependency and preserves the previously delivered spacing resolver.
It deliberately retains native minimum sizes rather than implementing new
appearance or UA-origin tracking. It does not implement `text-wrap`, general
button flex-child formatting, or intrinsic sizing for arbitrary nested icons.

## Actual observation and ordinary reduction

The settled visible-r1 Z.ai box group contains 330 items. Category buttons
have the same widths as before (99, 190, 91, 81, 99 px) and the same 24 px
height, but their label lines are at y277 and y297 below controls at y270.
The last line ends 23 px below its control. The public author class constant
uses `display:flex`, 12 px horizontal padding, 6 px vertical padding, a 20 px
line-height and 13 px font. It does not set width or height. It also requests
`text-wrap:nowrap`, which is currently unsupported; this separate shorthand
gap was recorded rather than patched through a partial cascade implementation.
Only the author class constant and saved stylesheet were read; no site code
was executed or request replayed by this work.

The independent plain `Alpha Beta` fixture uses a deterministic 60 px text
advance. The old button is 82×24 both with zero padding and with 6 px/12 px
padding. The padded flex/grid button wraps and ends 23 px below its box.
The same ordinary `div`, and the explicit 86×34 button control, fit correctly.
The fixed automatic button is 86×34 and fits on one line. The unsupported
`text-wrap:nowrap` still differs from the implemented `white-space:nowrap`
under an explicitly narrow width; the auto-size correction does not mask or
claim to fix that separate case.

## Acceptance

`tests/unit/button_auto_metrics_test.c` checks shipping DOM → style → layout
geometry, actual text display items, inline/block, flex and grid hosts,
physical/logical/asymmetric padding, line-height, unchanged native defaults,
content/border-box explicit dimensions and a deliberately small 50×18 box.

- Actual original source: 110 checks / 30 failures.
- Current automatic metrics: 110/110.
- The same ordinary cases under ASan/UBSan: 110/110, no reports
  (`detect_leaks=0`, `halt_on_error=1`).
- `LAYOUT_BUTTON_AUTO_METRICS_LEGACY`: the complete original 30-failure counter
  matches exactly; default and explicit-size controls remain green.
- Related normal suites: intrinsic 65/65, button content boxes 65/65, flex
  used height 75/75, absolute auto widths 99/99, spacing math 144/144.
  Existing negative controls remain active.
- `test-mk-wired`: 305 fragments, 304 reachable and one declared wrapper.

An independent read-only review found no new issue in the metrics block.
This remains a single-line label size floor: nested SVG, mixed fonts and
constrained multiline intrinsic layout are not fully implemented here, and
the existing special treatment of explicit zero height is unchanged.

Final `layout.c` SHA-256 is
`d70c2ba9ee62b4de028a4225099c2ae8d359c1d0721348143cac784d494a7c8d`.
Sanitizer confirmation and source hashes are recorded alongside
the logs in `build-zai-spacing-fix/button-audit/`. Primary permanent gate:
`make BUILD=build-zai-spacing-fix test-button-auto-metrics`.

Real guest integration remains the root task's next immutable build and site
check. No live VM, login input or default disk was operated by this subtask.
Host geometry does not by itself establish that either Z.ai or Kimi is usable.
