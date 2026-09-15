# Absolute descendants of auto-height containing blocks, 2026-09-09

The actual card reduction had correct 320x180 normal-flow padding, but its
absolute picture/image was 320x320; bottom:0 statistics started at y=180 rather
than y=142. The title's own normal-flow y=188 was already correct. This is a
separate defect from the navigation's inflated grid tracks.

A positioned auto-height container with local absolute descendants now measures
normal flow without any absolute subtree, rolls back owned trial items/boxes,
then formats normally with its used padding-box height. Nested measurement
passes are suppressed during a measurement, avoiding exponential recursion.
The regular final pass preserves display order, clip state and subtree ranges;
a postponed paint queue would have invalidated those ranges. The height lives
only on the current formatting stack, so no previous frame can set its basis.
Only absolute percentage resolution sees this used auto height: normal-flow
percentages still stop at an auto ancestor. Fixed descendants retain the
viewport basis. Auto overflow clipping reads the same measured height.

`make BUILD=build test-absolute-auto-height` checks the original card reduction,
normal-flow percentage control, padding/border basis, nested independent
positioning, fixed percent chains, image item count, clipping, maximum height,
stacking and repeat layout. The legacy control restores the premature absolute
layout and must fail. All image sources in this gate remain unfetched; this is
reserved geometry, not a decoded-image or real-site visual claim.

The prepass applies only when a positioned auto-height box has an absolute
subtree for which it establishes the containing block. It does not invent
flex/grid post-stretch definiteness, transformed fixed containing blocks, or a
new static-position algorithm. Those pre-existing boundaries remain separate.
Root agent owns the disk build and guest comparison; the fixture is
`tests/fixtures/engine-expansion/absolute-auto-height.html`.

Final host result: 39 checks pass, legacy control 12 failures. Regressions pass:
grid parser/placement/sizing/alignment 318, CSSOM 149, percentage height 41,
display contents 32 plus its CSSOM companion 9, opacity 12, form controls 60,
intrinsic 65, flex 176, max-height 18, generated content 54, grid font units 10,
and grid item percentages 18. `test-mk-wired` reports 216 fragments, 215 reachable
and the declared recursive wrapper. Detailed logs are in
`build/site-general/layout/auto-height-regressions.log` and
`auto-height-additional.log`; prerequisite controls intentionally print red.
