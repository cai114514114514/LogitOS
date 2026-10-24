# CSS expansion: visible layout and painting, 2026-10-01

The earlier HTML/API batch passed its own guest checks, but that did not prove
that ordinary pages gained useful CSS layout or painting. This batch follows
the declarations through stylesheet parsing, real layout, resource loading and
guest scanout. It also rebuilds the default disk: an isolated successful build
does not change the browser launched by `make run`.

## What changed

| Area | Result in the browser | Implementation |
| --- | --- | --- |
| CSS nesting | `&`, implicit descendants, selector lists and declarations mixed with child rules preserve source order and parent-list specificity | `css/nesting.c/h`, shared normalization in native CSS, extension properties and variables; LibCSS alternative matching |
| Conditional styles | Supported flat and nested `@supports` conditions use the same value-aware evaluator as support queries | `css/engine.c`, `css/sizing.c/h`, LibCSS parser |
| Preferred ratio | Ordinary blocks, atomic/flex/grid items and images use `aspect-ratio`; padding, border-box sizing, definite dimensions and min/max constraints are accounted for | `css/sizing.c/h`, `layout/ratio.h`, block/flex/grid consumers |
| Column flex | Auto-height columns honor their bases; bounded columns wrap into distinct columns | `layout/block.c`, `layout/flex.c/h` |
| Grid sizing | Explicit `start` retains intrinsic tracks rather than acting like `normal`; empty min-sized items contribute to tracks and actual item sizes | `layout/block.c`, `layout/grid.c/h` |
| Bitmap backgrounds | A single URL background actually loads, decodes and paints; size, position and repeat affect the pixels | `css/background.c/h`, `app/background.c`, `paint/background.c` |
| Rounded boxes | Four distinct corner radii, percentage ellipses, unequal solid border edges and rounded ancestor clips reach scanout | `paint/box.c/h`, `paint/renderer.c` |
| Composition | Transparent gradients retain their background color; solid border and background apply local box opacity once | `paint/box.c` |

The background grammar includes `auto`, `cover`, `contain`, one/two size
lengths, one/two position keywords or lengths, and repeat/repeat-x/repeat-y/
no-repeat. Supported units include percentages, px, em, rem, vw and vh. Plain
URL/none shorthand and the corresponding longhands share the same cascade
slots. Successful native color/gradient shorthands reset the relevant slots.
Inherited em geometry keeps the parent's computed font basis; percentages
still resolve against the child's positioning area.

Background resource discovery uses the existing image queue, cache and decoder
budget. Painting only borrows cached pixels and never performs network IO or
allocates an image. Discovery keeps pending background requests alive instead
of allowing the image cancellation pass to discard them immediately. Imports
decode escaped URL tokens and rebase them before losing the response URL of
the stylesheet that owns them. A masked decoration still refuses painting and
IO because there is no mask compositor to make its output correct.

The common stylesheet producer preserves nesting specificity using the parent
selector list as an alternative selector. Complex alternatives backtrack over
the entire selector chain. Resource exhaustion propagates as a selection error,
not `false`: converting it to a boolean could silently turn `:not(...)` into a
successful match. Normalization has depth 32 and output 32 MiB ceilings;
alternative matching has a depth 64 ceiling and a shared per-node 65,536-visit
budget. These are refusal boundaries, not claims of unlimited conformance.

During sanitizer validation a genuine double-free was observed: selection
failure cleanup freed a borrowed parent bloom. Cleanup now distinguishes owned
storage. The stylesheet adapter also checks fatal append/finalize results and
refuses the sheet rather than selecting from a partial parse. Cache-key
allocation failure refuses publication rather than leaking a second parsed
sheet. These corrections retain ordinary invalid-declaration recovery.

The final guest run found one additional general defect: an empty Grid child
with `min-width:20px` had width zero under explicit `start`. Both intrinsic
contributions and final item bounds now include min/max constraints on both
axes. Percentage bounds wait for the actual grid area. Overflow from a child
does not enlarge a definite row or move the following sibling.

## Guest evidence

Both versions used the same authored component HTML, stylesheet and original
80 by 40 BMP. Their three source SHA-256 values match in both result files.
The before browser is the preserved HTML-priority build; the after browser is
the final CSS build. The ISO is identical, so this comparison does not mix a
kernel change into the CSS result.

| Guest gate | Before | Final after | What was observed |
| --- | --- | --- | --- |
| Components | 19 checks, 13 failures | 19 checks, 0 failures | Public element rectangles, HTTP resource requests and exact RGB samples from scanout |
| Background/corners | 26 checks, 15 failures | 26 checks, 0 failures | Border geometry plus exact pixels for gradient bases, unequal corners/edges, rounded clips and local opacity |
| Existing HTML priority | Previous batch 457/0 | 457 checks, 0 failures | Window/Worker checks and trusted native form submission/edited entry-list wire behavior |

Concrete component changes include a 240 by 0 card becoming 240 by 135, column
children occupying 60 and 90 pixels of height, three 40 by 60 items forming
three columns, explicit-start Grid items retaining width 20 rather than 90,
and nested styling producing an 80 by 16 leaf rather than 12 by 8. The external
stylesheet now causes the actual `/assets/stripes.bmp` request. Cover, contain
and repeat then show distinct, checked RGB samples instead of the base color.

Artifacts, relative to the repository root:

- Before: [`result.json`](../../build-css-verify/guest-before-final/result.json), [`screen.png`](../../build-css-verify/guest-before-final/screen.png).
- Final components: [`result.json`](../../build-css-verify/css-guest/components-d7aca7f9961b4d76a4b43437064e7b32/result.json), [`screen.png`](../../build-css-verify/css-guest/components-d7aca7f9961b4d76a4b43437064e7b32/screen.png).
- Final background pixels: [`result.json`](../../build-css-verify/css-guest/background-a23090905971496b8bc14b6aabf08d26/result.json), [`frame-01.png`](../../build-css-verify/css-guest/background-a23090905971496b8bc14b6aabf08d26/frame-01.png) converted losslessly from the captured PPM in the same directory.
- HTML regression: [`result.json`](../../build-css-verify/html-regression/result.json); the run used `--with-navigation`.

`tests/qmp/qmp_css_components.py` measures the HTTP-served page and actual WM
client geometry. `tests/qmp/qmp_css_background.py` consumes the same expectation
CSV as the host painter test. Its samples come from the framebuffer; counting
changed pixels or looking only for a ready marker would not establish these
behaviors. Outputs have unique run directories, QMP sockets and HTTP ports.

### Saved MDN page

The MDN flex reference page was also served with its original HTML and all 17
saved stylesheets. Before and after source fingerprints and the manifest match.
Both guests report 17 successful external sheets, no empty/HTTP/transport
failures, and 124,957 bytes reaching the parser (50,436 inline + 74,521 external).
The captured heading typography and breadcrumb spacing visibly change.

- Before: [`viewport.png`](../../build-css-verify/mdn-before-capture/viewport.png), [`result.json`](../../build-css-verify/mdn-before-capture/result.json).
- After: [`viewport.png`](../../build-css-verify/mdn-after-complete/viewport.png), [`result.json`](../../build-css-verify/mdn-after-complete/result.json).

The native parser's selector drops change from 121 to 1; declarations kept
change from 1,129 to 1,335. These are diagnostic counters, not a site pass:
native unknown-property counts include properties handled by extension
consumers, and the extension selector gate still refuses unsupported cases.
The page remains visually incomplete. Four unsaved module scripts return 404
in both versions; the new background path additionally requests an unsaved
`low-dark` SVG, also recorded as a real 404. No source rewriting, script removal,
replacement asset or browser hostname special case was used.

The first snapshot attempt exposed a missing closing brace in the read-only
geometry probe: both browsers correctly reported SyntaxError. Its failed runs
remain in `mdn-before-final` and `mdn-after-final`. After fixing and checking the
JavaScript syntax, one paired run lost a stylesheet in guest transport despite
successful responses from the local server. That incomplete run remains in
`mdn-after-capture`; it is excluded from the comparison. The final driver
requires the complete stylesheet set both at the server and in the guest load
summary, and detects probe exceptions before waiting out the report timeout.

## Host gates and controls

The final host validation passed with address/undefined-behavior instrumentation:

| Gate | Positive checks | Fault observed failing |
| --- | --- | --- |
| Layout | 164/0 | Ratio disabled: 76 checks/25 failures; old column solver: 20/8; start treated as normal: 6/1; Grid bounds omitted: 62/19 |
| Nesting/sizing producer | 72/0 | Normalization bypass: 17/12; ratio producer disabled: 12/8; exhausted budget treated as false: 5/1 |
| Bitmap background pipeline | 43/0 | Actual cache/pixel assertions fail when loading or sampling is disabled |
| Background/corner painting | 26/0 | Gradient base removed: 26/3; old rounded box: 26/15 |

Controls assert named failures and are prerequisites of the positive gates.
For example, the Grid-bounds fault prints
`FAIL: grid: auto track includes minimum-only empty item`; the ratio fault
prints `FAIL: ratio: card aspect height`. Compiler failure is not accepted as
the control result. The guest positives depend on the same host controls.

Relevant existing import and spacing gates also passed: imports 40/0,
spacing 144/0 with literal cases 12/12. Their controls failed at 40/17 and
144/36 or 144/9 respectively. Existing Grid, font-unit, percentage item, image
intrinsic, percentage-height and used Flex height regressions were rerun.
The selected regression log is `/private/tmp/logitos-css-grid-bounds-regression.log`;
the final host log is `/private/tmp/logitos-css-ready-host.log`.

Sanitizer scope is the host adapter/layout/painter sources linked to the
established host LibCSS archive. This is not full-library sanitizer or leak
coverage: the archive is not rebuilt with instrumentation and Darwin runs use
`detect_leaks=0`. No full WPT or entire CI pass is claimed.

`make test-css-guest BUILD=build-css-verify` exited zero. Its captured log is
`/private/tmp/logitos-css-final-guest-gate.log`. `make test-mk-wired` passed with
419 fragments, 418 reachable and the existing one declared exception.
`git diff --check` passed.

## Shipping artifact

`make -j6 BUILD=build all build/disk.img` completed and retained 20 user-state
inodes through the existing profile snapshot/repack mechanism. Reading the
filesystem payload proved that `/browser.aex` inside the default disk equals
the freshly linked `build/browser.aex` byte for byte. That binary also equals
the final isolated browser used by the guest gates.

- Browser size: 7,200,832 bytes.
- Browser SHA-256: `6162edcc2ebbd9e78aa737e0ff257337ed0d3de6e1c044b2b2de5fd886b6f4f3`.
- ISO SHA-256: `7bc78d8d616a27d00b503a4fa87e426f4db7aa28bdaf9fd4b95f304a478bcbeb`.
- Package proof: [`build/css-package.json`](../../build/css-package.json).

The Makefile now derives stylesheet/layout/painter dependencies from the
browser inventory. Textually included C modules invalidate their owning
objects without also being linked as duplicate translation units. This avoids
a source-list agreement that accidentally excludes the implementation.

## Remaining scope

This is a substantial batch with measured visible effects, not complete CSS
conformance. Multiple background layers, advanced mixed URL shorthands,
radial/conic/repeating gradients, alternate background origin/clip/attachment
and a mask compositor remain outside this batch. Bitmap sampling handles
positive axis scales; arbitrary affine or mirrored bitmap transforms have not
been completed. Local solid-box opacity is covered; full descendant-group
compositing and patterned rounded-border corrections are separate work.

Top-level `&`, nested scope/container and namespace cases, explicit child
rules below pseudo-element parents, calculated ratio support conditions and
full nested CSSOM remain bounded/refused. Existing global approximations for
variables and cascade layers are not presented as fixed. Orthogonal layout,
fragmentation and every replaced-element subtype are not established by these
ratio checks.

The saved real-page snapshot driver preserves the original HTML/CSS bytes and
scripts. Unsaved assets return actual 404s and are recorded. Its screenshots
are supplementary static evidence, not a verdict that the live site's layout
or JavaScript interactions are correct.

## Standards consulted

- [CSS Nesting: declaration/child-rule order](https://drafts.csswg.org/css-nesting-1/#mixing) and [nesting-selector specificity](https://drafts.csswg.org/css-nesting-1/#nest-selector).
- [CSS Sizing: preferred aspect ratio](https://drafts.csswg.org/css-sizing-4/#aspect-ratio).
- [Background positioning](https://www.w3.org/TR/css-backgrounds-3/#background-position) and [background sizing](https://www.w3.org/TR/css-backgrounds-3/#background-size).
- [Selectors: alternatives](https://www.w3.org/TR/selectors-4/#matches) and [CSS Syntax: numeric token consumption](https://www.w3.org/TR/css-syntax-3/#consume-a-number).
