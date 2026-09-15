# Browser wiring inventory, 2026-09-09

Run `make test-browser-wiring` after rebuilding `build/browser.elf`. The report
is `build/wiring/wiring-audit.json`; this command does not boot or measure a guest.

The audit asks make to expand BROWSER_PIPE/BROWSER_JS_SRC and their object lists,
records recursive `.inc` ownership, checks the actual ELF's strong symbols, and
locates reviewed source consumer edges. It also records every js_page installer
call and hashes the inspected sources and ELF. The source scan is lexical, not
a complete C/JS call graph or preprocessor evaluator. A written call and a linked
provider do not prove the call executes or that the feature renders.

The ten reviewed seams are digest, crypto operations, live Range, native mutation
observers, generated content, modal dialogs, native inert/focus, content support
queries, live matchMedia, and synchronous CSSOM reflow. The separate feature host
gates and ordinary guest pages remain the behavioral evidence.

The negative control removes `js_cssom_set_reflow(browser_cssom_reflow)` from a
temporary copy of browser.c, while leaving the real source and linked ELF alone.
Observed failure: `MISSING_CALL cssom_reflow c/apps/browser/browser.c ...`. The
positive inventory then passes. This specifically detects the false inference
that a linked setter necessarily has an embedder caller.

Five reviewed gaps remain visible in JSON, independently of the gate result:

- The full text formatter (`ltx_layout_runs`) has no production caller; spacing,
  text transformation and indentation do not reach that formatter.
- Vertical writing mode is stored without a layout/paint consumer.
- Element scroll offsets stay in a CSSOM side table. The window scroll path is
  separate and has an embedder callback.
- Popover state lives in JS; native top-layer painting does not consume it.
- WAAPI's script-visible interpolation does not drive the native paint state.

Ordinary fixed/sticky behavior and pseudo-element getComputedStyle targeting are
additional semantic gaps, not claims that the distinct modal/generated paths are
unused. CSS animations have their own wired clock and must not be conflated with
WAAPI. The report intentionally does not certify any of these gaps as repaired.

The CSS wiring repairs have separate host evidence: content support queries
35/35; live media and consecutive synchronous geometry reads 9/9; original CSSOM
149/149; generated content 54/54. The corresponding controls were observed red.
None of these host counts substitutes for a guest screenshot or interaction run.

## Correction after native-consumer wiring

The five-gap list above records the initial state; it is retained because the
old claim explained this work. Four entries now have production callers:

- Horizontal text formatting reaches `ltx_layout_runs`, display items and glyph
  spacing paint. Complex inline first-line indentation, vertical glyph/layout
  support and complex-script cluster spacing remain outside that subset.
- Element offsets drive paint projection, clipping, CSSOM geometry and trusted
  hit testing. Wheel scrolling and ordered rendering-step events share state.
- Popover membership drives native top-layer layout/paint. Its integration also
  needs an explicit layout invalidation and the native invoker default; tests
  calling only `HTMLElement.click()` hid both missing app-level edges.
- WAAPI opacity and absolute 2D transforms use the existing page frame queue.
  Completion microtasks require a checkpoint even on a constant-value frame;
  tests that drain jobs after evaluating every expression hid this omission.

The updated inventory checks 14 reviewed features and leaves vertical writing
as a known gap. This is still not a general browser compatibility score. Larger
frame, shadow-tree, message and CSS gaps are documented in
[framework/runtime audit](framework-runtime-audit-2026-09-09.md) and
[CSS audit](css-compat-audit-2026-09-09.md). Behavioral results and release artifact
provenance are recorded separately in the general-browser progress report.
