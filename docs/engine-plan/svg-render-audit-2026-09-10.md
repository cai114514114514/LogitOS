# SVG and icon rendering audit — 2026-09-10

Read-only production audit, paused for the higher-priority browser-profile
rebuild defect. Eleven synthetic native cases exercise the shipping layout and
SVG rasterizer: the static red rectangle control passes; ten cases expose the
general compatibility gaps below. No production rendering code changed in this
audit, and the user's authenticated QEMU was not navigated or inspected for
private content.

| Finding | Concrete evidence |
|---|---|
| Dynamically created SVG has no drawable source | `createElementNS`/native DOM creation produces `rawlen=0`; layout emits zero images. The same static 16×16 SVG emits one image with 256 red pixels. |
| SVG DOM attribute changes keep the old source | Changing a static rectangle's fill from red to green still emits 256 red pixels. Layout decodes the parser's original `node.raw`; mutations do not regenerate it. |
| `currentColor` does not resolve the element/inherited color | Green element color and green inherited HTML CSS color each yield 256 black pixels. `svg.c` explicitly converts `currentColor` to black. |
| CSS fill is not applied to SVG descendants | A CSS rule setting the rectangle fill green leaves 256 red pixels from its original presentation attribute. |
| SVG transforms are ignored | A 4×4 rectangle under `translate(10,0)` remains in the left half: 16 left pixels, zero right; the fixture expects the reverse. |
| `<defs>`/`<use>` does not instantiate visible geometry | A referenced red rectangle yields an image with zero opaque pixels. |
| `clipPath` is ignored | A 16×16 rectangle clipped to a four-pixel-wide strip should have 64 opaque pixels; it retains all 256, including the right half. |
| Native button content sizing overrides zero CSS padding | A 20px icon inside a 24px button draws at 12×12; a 12px icon inside a 12px button draws at 1×1. The layout's fixed six-pixel padding on each side is applied despite the fixture's `padding:0;border:0`. |

The decoder declares unsupported `transform`, `style`, and `defs` near its
header. Current-color handling is in `c/lib/image/svg.c` near line 369; SVG raw
source and native-button sizing are in `c/apps/browser/layout.c`. Exact line
numbers may shift with concurrent layout work. The test artifact records the
measurements and expected geometry independently of screenshots.

Native evidence: `build-cookie-persistence-fix/svg-audit/{probe.c,probe.mk,probe.log,results.json}`.
The fixture is `build-cookie-persistence-fix/svg-audit/fixture.html`.

A separate headless guest, using the frozen `snapshot-referrer` image and a
local synthetic server, executed the actual JavaScript mutation/creation path.
Its marker was `SVG-AUDIT READY dynamic=true mutated=true`. The screenshot
retains the static control, blank dynamic SVG, stale red mutated SVG, black
currentColor, stale red CSS fill, untranslated rectangle, blank use reference,
and unclipped rectangle. Evidence:
`build-cookie-persistence-fix/svg-audit/guest/{results.json,page.png}`.

An initial fixture used unquoted numeric SVG attributes, which the standalone
SVG decoder does not accept; its static control therefore failed. That run is
retained separately under `guest-unquoted`, and the final fixture uses quoted
attributes so its passing control validates the ten comparisons. It is not
counted as evidence for the dynamic-SVG failure.

The public DS stylesheet captured without account credentials was
`https://fe-static.deepseek.com/chat/static/main.fdf56932b4.css` (281,634 bytes,
SHA-256 `3b1adbfba0691413cfcae882af6b42a053e05956a40a9805ae495a82d32eeef2`).
It defines `.ds-icon` using inline-flex and `1em` SVG dimensions, and includes
rules such as `svg path[fill]{fill:currentColor}` and corresponding stroke
coloring. These dependencies make the observed general gaps relevant to icon
rendering. They do not establish that every blank icon in the user's page has
the same cause, nor explain the entire blank chat area without live layout and
runtime evidence. No authenticated text or identity was inspected.

The expected behavior follows SVG's [paint/currentColor specification](https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint)
and [use-element instance model](https://www.w3.org/TR/SVG2/struct.html#UseElement).

## Decoder implementation follow-up — 2026-09-11

The original audit above remains the record of the old behavior. The generic
image decoder now resolves inherited `currentColor` independently for each
shape/use instance, accepts the affine SVG transform list, registers local
`defs` references (including forward references), instantiates `use`/`symbol`,
and composites `clipPath` in `userSpaceOnUse` and `objectBoundingBox` units.
Nested clips intersect and clip children combine their silhouettes; their
fill color/opacity does not change the clipping geometry. Group opacity is
applied once to the composited subtree. `use` x/y also moves its user-space
clip, and auto dimensions retain a referenced viewport's explicit size.

`c/lib/image/svg_scene.inc` is part of `svg.c`, so the existing `img_decode`
ABI remains unchanged. The browser's separate DOM/CSS bridge can serialize
winning color/fill/stroke values as standard presentation attributes. Inherited
`currentColor` stays a semantic value until a shape is painted, allowing each
use instance to inherit a different color. Standalone SVG also accepts simple
inline declarations, including ordinary importance/order precedence. No private
serialized attribute or hostname-specific branch is used.

The existing integer path and elliptical-arc builders are reused in user space;
flattening tolerance is tightened using the composed matrix scale. Fill points
and the already-built stroke outline are then transformed, preserving a
nonuniformly scaled stroke. Matrix coefficients have 16.16 precision, while
path coordinates retain the existing 24.8 precision. Decimal exponents are
applied before fixed-point rounding, so ordinary `scale(.001)` and `.016e3`
inputs no longer disappear through premature truncation. Two signed negative
left shifts in the shared stroke unit-vector calculation were replaced by
exact multiplication after the ordinary stroke regression exposed them under
UBSan.

The image decoder has explicit bounds: 4 MiB for the complete input, 2,048
nodes, depth 48, 8,192 scene visits, 8 MiB cumulative geometry attribute work,
64 MiB of simultaneous offscreen layers, and 256 Mi pixels of full-surface
work. Clears, empty-group compositing, and viewport masks consume the same
work budget as geometry. Exhausted budgets return decode failure and release
all owned storage. Reference cycles contribute no geometry. Local references
are XML-unescaped before identity comparison; external resources never load.
The original 2,048 × 2,048 output cap remains.

Permanent validation is `make BUILD=build-ds-svg-fix test-svg-scene test-svg-scene-asan`. The scene suite
has 91 checks; its feature-disabled control fails exactly 29 pixel checks.
The five-check resource suite runs with a small work budget and its unmetered
layer control fails exactly one targeted assertion. Both current suites pass
ASan and UBSan. `test-svg` also runs the scene gate before the legacy SVG path,
curve, sniffing, and truncation checks. The shared GFX stroke suite passes
76 checks and GFX paint passes 25 checks, including linear/radial gradients.
The real CSS-to-browser-paint gate `test-paint-gfx` also passes, including its
negative control and the shared `img_css_color` dependency. Evidence is in
`build-ds-svg-fix/svg-scene/{gate.log,existing-gates.log,css-paint-gate.log}`.

The permanent guest driver is `tests/unit/svg_scene_guest.py`; its make gate
requires explicit `SVG_SCENE_OLD_SNAPSHOT` and `SVG_SCENE_SNAPSHOT` frozen
artifact directories and always uses QEMU `-snapshot`. It serves only eight
local ordinary SVG samples, locates a known page anchor, and compares interior
pixels. The frozen old image passes the static red-square control and fails
all seven feature samples: `build-ds-svg-fix/svg-scene/guest-old/results.json`.
The frozen current image passes all eight pixel cases:
`build-ds-svg-fix/svg-scene/guest-current/results.json` and `page.png`. Both runs
consume identical local HTML/SVG bytes; the current image is
`build-ds-render-fixed/snapshot` (browser SHA-256
`ab4caaa03e8210d7dd0ba2c3f5f955410b341b3e02504b4eb396f80918c669ad`).
The JSON records ISO/disk hashes and sampled pixels. This proves the decoder
changes are present in an actual independent browser guest.

This remains an SVG image subset, not all of SVG 2. Text shaping, external use,
SVG paint servers/filters/masks, stylesheet selector matching inside an external
SVG, and the full CSS transform/length grammar are outside this change. The
old decoder did not implement SVG gradients: it skipped `defs` wholesale.
The shared CSS color parser and existing browser/GFX gradient path remain in
place. Object bounding boxes for curves use the flattened geometry's bounded
precision. This follow-up does not itself establish dynamic DOM/CSS bridge
behavior or explain every blank area in a live authenticated page.
