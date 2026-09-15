# Transparent ancestor ink leak — 2026-09-09

DeepSeek sample CSS `.ds-mobile-menu` is `display:flex;opacity:0;pointer-events:none;transition:opacity .2s`. It is hidden by opacity, not a desktop media query. The painter only tested each leaf item's hidden/opacity; CSS opacity is not inherited, so descendants stayed opaque. A reduced ordinary parent, fixed descendant under transition, and explicitly opacity:1 child all painted through their opacity:0 group (3 failed assertions / 6 initial checks).

`browser_paint.c` now skips ink from items with any live opacity:0 ancestor. This is exact for a fully transparent group. Fractional opacity is deliberately not implemented by multiplying alpha per descendant because overlap requires real offscreen group compositing. Layout and pointer targeting are preserved; visibility:hidden is not substituted for opacity and a child may still override inherited visibility.

`make BUILD=build test-opacity-group`: **11/11 pass**. Its prerequisite `BROWSER_NO_ZERO_OPACITY_ANCESTOR` produces four named failures, including the return-to-zero CSSOM mutation. Host gate executes actual browser painter and checks paint operations, transparent descendant hit target, unchanged flow geometry, CSSOM 0→1→0 restoration, and visibility override. Logs: `build/site-general/layout/opacity-baseline.log`, `opacity-final.log`.

Guest fixture: `tests/fixtures/engine-expansion/opacity-group.html`; red content initially invisible, Show red/Hide red changes visibility while green reference bar stays in place. Actual guest pixels/input are pending root's shared disk build.

The fetched DeepSeek SSR has an outer div with `style="opacity:0"`. Correct ancestor culling can therefore expose a still-failing bootstrap as a blank page. Do not defeat opacity to preserve the old erroneous pixels; JS runtime/reveal completion requires independent verification. `pointer-events:none` is an additional independent interaction requirement, not implemented by this paint-only change.

## Final checks and dirty-rectangle audit

The final gate adds interaction with display:contents: a boxless wrapper does not establish an opacity group. Final **12/12 pass**, control still4 named failures (`layout-final-suite.log`).

Read-only follow-up plus private host probe (`opacity-dirty-probe.c` / `.log`) checked a transparent height:0 ancestor with a child at y200/h50. Unchanged transparency gives an empty dirty rectangle; 0→1 and1→0 both report a real0,200,100,50 rectangle. The cull occurs before pd_record, so appearance/disappearance adds/removes the child record and pd_finish unions its own extent, regardless of parent box size. No production or official gate change was made for this audit. The private probe passes15/15 including the12 base checks. This rules out this specific host dirty-diff hypothesis, not every guest GUI flush or screenshot timing path.
