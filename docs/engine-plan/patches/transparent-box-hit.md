# Transparent real border boxes: read-only proposal

The suspected type filter is not present. `browser_hittest_node_scroll()` in browser_paint.c accepts every display item type, checks the existing projected coordinates/clip/inert/modal rules, selects the first back-to-front hit, and obtains href from that item's ancestor chain when `item.href` is absent. A real IT_RECT belonging to an anchor already gives the whole rectangle a DOM click and default navigation. browser.c dispatches down/up/click through that shared hit result.

The producer gap is `st_inked()`: an undecorated `display:block;width:180px;height:40px` anchor gets a `boxrec` and therefore a correct getBoundingClientRect, but gets no border-box item. Its text receives events; blank parts of its actual box have no candidate for the hit test. This also affects transparent div event targets and transparent overlays, so emitting items only for anchors would leave the general defect open.

`transparent-box-hit.patch` is an unapplied proposal against the shared layout.c observed on 2026-09-09. It adds one `box_open_hit()` wrapper, used by seven existing real-box entry points (block/atomic, float, positioned, ordinary block flow, flex, grid, table cell). There is one implementation, not seven independent hit algorithms. It emits a transparent IT_RECT before children only when no ink rectangle already exists, retains the ordinary display-list z/clip/provenance ordering, and synchronizes final geometry in box_close. The flex entry also hands its transparent index to the existing grow/stretch updates. If form-control layout reuses that slot as IT_CONTROL, the same finalization still applies.

Why not put this unconditionally inside every box_open call: the table also includes bare inline unions spanning multiple lines and table-section records emitted after their descendants. Treating those aggregate records as a new rectangle would hit unused wrapped-line areas or place a parent over its children. This proposal intentionally leaves those geometry-only records alone. It also does not change pointer-events, transform hit precision, opacity/visibility semantics, root body/html hit regions, or inline padding hit regions; those remain separate coverage boundaries.

Why IT_RECT and not the existing IT_HIT: IT_HIT specifically represents whitespace; newline2 stretches it during justification. Reusing it for a box would turn a transparent inline-block into stretchable space. A transparent IT_RECT takes the existing no-fill/no-border painter branch and participates in the same ordering as an authored background. This adds one item for each previously inkless real box, with a memory/sort cost that has NOT been benchmarked. The old side-table rationale remains valid for aggregate geometry; this patch's comment explicitly records its narrower correction for interactive real boxes.

Private ordinary fixtures: transparent-box-hit.html and transparent-box-hit-target.html. They print client geometry and report real click events, with no synthetic dispatch. Native acceptance should test the blank center and blank bottom-right of the transparent link; the painted control; a transparent overlay that must receive the event without navigating its covered link; the inside/outside sides of an overflow clip; and preventDefault on a blank portion. Parent/current guest must establish exact press/release coordinates independently. Additional host checks before landing should cover flex stretch, nested scroll, hidden/inert, modal layers, display:none/contents and a wrapped inline's unused line area. The new LAYOUT_HIT_INK_ONLY macro restores the old missing-item behavior; a landing gate must make that negative build a prerequisite and show it fail.

Validation performed here: static producer/consumer audit and `git apply --check` only. No build, test execution, VM, production edit, or passing implementation claim. The parent owns navigation_base; the layout agent owns current projection work. Rebase and review this patch with that agent before applying.


## Integration correction

The read-only status above is retained as the original handoff. Root subsequently
applied the patch after the layout owner froze its changes. The connected
`test-transparent-box-hit` passes 119/119; its prerequisite old-behavior control
fails 51 checks, including blank native/CSSOM hits in atomic wrap/vertical-align,
flex/grid/absolute/table-cell boxes, transparent overlays and scrolling clips.
`pointer-events:none` is separately marked UNSUPPORTED: both CSS.supports forms
return false, and the original three failed hit observations remain recorded.
It is not counted as a passing capability.

The assembled guest clicked the transparent overlay and the canceling link,
then the unpainted bottom-right of a real link. Native listener markers and the
actual destination document prove both cancellation and default navigation.
See `build/site-general/assembled-guest/results.json` and its serial/screenshots;
input image hashes were unchanged across the run. Bilibili's latest display-list
snapshot is 693 items (dynamic peak 1085 of 16384), with visible titles intact.
These are capacity observations, not a controlled CPU/memory benchmark.
