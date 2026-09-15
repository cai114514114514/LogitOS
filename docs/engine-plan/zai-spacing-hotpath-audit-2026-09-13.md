# Z.ai spacing hot-path audit: bounded normal input

The complete saved Z.ai 1.1.93 stylesheet does **not** reproduce a large
spacing-related slowdown on the small ordinary DOM tested here. Both actual
pre-spacing sources and current sources complete cold styling, warm restyling,
and a final layout-only pass. This does not clear the real guest's missing
main content or establish its cause.

The input is the same 436,038-byte public stylesheet, SHA-256
`d96de011b7f3a18b57de65bd0b022b120ad6ba75daec826c0669a3a01e2d3427`.
Both versions expand it to 381,255 bytes. A hand-written 29-node document uses
ordinary flex/grid, five category buttons, three colored cards, and logical
composer padding classes. It contains no site script, challenge, module,
image, or request. The host fixture's resource stub cannot fetch resources;
font metrics are deterministic and there is no active JavaScript/animation
owner. Each binary runs once, with one cold, one warm, and one layout-only pass.

| Native host phase | Before, ms | Current, ms |
| --- | ---: | ---: |
| Variable expansion | 5.980 | 4.140 |
| Cold LibCSS style | 11.398 | 14.260 |
| Cold extension style | 14.308 | 14.025 |
| Cold layout | 0.153 | 0.162 |
| Warm LibCSS style | 0.168 | 0.217 |
| Warm extension style | 0.041 | 0.045 |
| Warm layout | 0.019 | 0.018 |
| Layout only | 0.014 | 0.011 |

These are single observations, not a statistical performance benchmark or
guest-time estimate. Each process exits 0 within the harness's 30-second bound.
The extension stylesheet compiles exactly once in both versions, so this input
does not trigger repeated cache invalidation. Recognized extension rules rise
from 452 to 552 because additional spacing declarations are retained. Existing
selector refusals rise from 127 to 155; this test does not repair their unsupported
syntax/pseudo targets. Actual synthetic geometry changes as intended: card gap
0 → 12 px, logical padding x offset 0 → 12 px, and the y offset including
existing alignment 2 → 12 px.

The source audit agrees with this limited runtime result:

- `css_spacing_math.inc::sm_capture` advances a scanner and permits at most
  two components; per-element application scans at most two gap axes and four
  padding edges. It does not request another cascade or layout.
- The shared `css_inset_math.inc` parser bounds expression nesting and numeric
  leaves. No new parser recursion or retry loop was added by spacing.
- `css_extra.c:1785` reuses the compiled sheet when bytes, viewport, screen and
  color scheme match. The same pre-existing cache condition is exercised by
  the warm pass.
- The normal block padding correction resolves once against the incoming
  containing width before width calculation; it does not iterate to a fixed
  point. The existing flex resolution loop freezes items each round; its
  rounding repair decreases a finite deficit or exits when no candidate remains
  (`layout_flex.c:375`, `:464`, `:510`).
- CSS and layout are still synchronous work on the UI call stack.
  `browser.c::restyle` directly calls scoped/full style then `layout_page`
  (`:3165`–`:3202`); `browser_resize` does likewise (`:3666`), and animation
  processing can call layout (`:6412`). These calls do not carry a native
  time budget or process UI input internally. Task/event budgets only help
  after the call returns.

The real page has a different DOM, live animation, fonts, scripts, transport and
guest kernel behavior. A topology-specific expensive layout or another long
native callback is therefore still possible; this experiment only rules out
the measured full-sheet/small-DOM reproduction. It does not justify changing
product code or declaring Z.ai usable.

Artifacts are `build-zai-spacing-fix/hotpath/{bench.c,bench.mk,before.log,current.log,run.json,summary.json}`.
The before binary uses the saved original spacing-related sources and matching
header; both use the same vendor CSS archive and normal host harness. Production
hashes still match `build-zai-spacing-fix/production-hashes.json`. No production
file, VM, site state, or network request was changed by this audit.
