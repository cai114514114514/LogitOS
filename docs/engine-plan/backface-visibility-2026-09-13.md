# Backface visibility: bounded affine paint and hit support

Date: 2026-09-13. Product source is stable; this is host validation, not a new
Z.ai or other live-site acceptance result. No VM, user input, login data, SMS,
or real-site request was used for this implementation or its gates.

## Problem and behavior

An ordinary element using `backface-visibility:hidden` and `rotateX(180deg)` or
`rotateY(180deg)` still contributed text, backgrounds, and native hit targets.
Both faces could therefore be drawn during a flip. This was independent of
damage invalidation: the ordinary full-paint fixture reproduced it.

The computed keyword now reaches a shared face decision in normal paint,
backdrop sampling, native hit testing, and the embedded-page opening hit path.
Turning a hidden face away removes its ink and hit targets without removing its
layout geometry. The default and explicit `visible` behavior remain visible;
`visibility:hidden` retains its independent behavior. Ordinary descendants
share their plane's exclusion. A top-layer element starts at its own boundary.

The helper retains the affine 3D linear matrix for facing and uses its inverse
normal. A 2D mirror is consequently distinct from turning a face away. Exact
quarter turns are treated as edge-on for hidden faces. Parent transforms
accumulate across represented `preserve-3d` boundaries; flat, opacity, and
overflow grouping boundaries stop that accumulation. Independently transformed
3D child planes can remain visible when a parent's plane is hidden, including
two nested half turns. `transform:none` and plain 2D transforms do not create
that escape. This follows the plane distinction described in
[CSS Transforms 2](https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts).

## Product ownership

Six files constitute this change:

- `css.h`: computed fields, property identifiers, and shared keyword parser declaration.
- `css_extra.c`: keyword parsing, declaration application, and explicit inheritance resolution.
- `css_extra_cascade.inc`: per-declaration cascade merging.
- `css_engine.c`: computed serialization, paint-only differences, value-aware `CSS.supports`, and the existing registered post-pass during full synchronous CSSOM flushes.
- `browser_backface.inc`: allocation-free affine face and plane-boundary decisions.
- `browser_paint.c`: shared paint, backdrop, frame-hit, and native-hit consumers.

The synchronous CSSOM hook is necessary: the full base CSS flush previously
could overwrite extension state before a same-turn computed read. Full fresh
and dirty fallbacks now call the same registered extension post-pass that the
scoped path already used. No new JS-visible API or layout algorithm was added.

## Reproducible validation

Permanent targets are in `tests/backface.mk`, included from the main Makefile.
`tests/unit/backface_test.c` uses the existing HTML/CSS/layout/paint host stack
and its drawing-operation recorder. It checks normal paint output and the real
native hit path rather than asserting helper return values. The negative
control is a prerequisite of the positive target.

| Gate | Observed result |
| --- | --- |
| `test-backface` | 124 checks, 0 failures |
| `test-backface-san` | ASan/UBSan, 124 checks, 0 failures; leak detection disabled |
| `PAINT_BACKFACE_VISIBLE_LEGACY` | 124 checks, exactly 36 expected failures |
| Negative output checker | Exact failure names and counts matched; all other checks preserved |
| CSS extra cascade | 22 passed; its existing negative control also passed |
| CSS inline extensions | 4 passed; its existing negative control also passed |
| Computed style | 35 passed; its existing negative controls also passed |
| Stacking | 46 passed; its existing 24-failure negative control matched |
| Modal paint | Passed, including native interaction and top-layer controls |
| `test-mk-wired` | 309 fragments, 308 reachable, 1 declared wrapper |
| Cross compiler | `browser_paint.o`, `css_extra.o`, `css_engine.o` compiled for x86_64-elf |

The ordinary cases include X/Y rotation at 0/90/180 degrees, default/explicit
visibility, 2D mirroring and rotation, subtree backgrounds/text, clipped hits,
flat versus preserve boundaries, nested half turns, non-inheritance and
explicit inheritance, invalid declaration preservation, specificity and
important, and first/same-turn computed reads after normal style mutations.

An initial synchronous-read test was incorrectly put in a passive CSS context,
which intentionally ignores page JS dirty state. Its single failure was a
fixture error. The permanent test uses the default page context and the normal
registered post-pass; the repeated mutation reads now pass without manual CSS
application before the getter. Independent review also caught and corrected
the original unconditional ancestor-plane cull and a raw-pointer `none`
misclassification before this final run.

Final command, exit status 0:

```sh
make -j4 BUILD=build-backface-fix test-backface-san \
  test-css-extra-cascade test-css-inline-extensions test-csstyle \
  test-layout-stacking test-mk-wired build-backface-fix/modal_paint_test \
  build-backface-fix/browserobj/c/apps/browser/browser_paint.o \
  build-backface-fix/browserobj/c/apps/browser/css_extra.o \
  build-backface-fix/cssobj/c/apps/browser/css_engine.o
build-backface-fix/modal_paint_test
```

## Evidence retention

Several repository `build-*` directories disappeared during parallel work.
The agent and root did not delete or archive them. The gates above were rerun
once against the current source, then the actual fresh logs, exact negative
output, test sources, six product sources, and SHA-256 manifest were copied to:

`/Users/wangzhe/.codex/artifacts/logitos-ai-sites-20260913/backface/`

Use `final-gates.log`, `modal.log`, `old.log`, and `product.sha256` there. The
manifest paths are relative to the repository; retained source copies are in
that directory's `product/`. Earlier missing build paths are not evidence for
the final result. The manifest fixes the six product sources used for these
results; no subsequent product edit was made for this report.

## Explicit limits

This is affine facing support, not full 3D rendering. A non-affine perspective
matrix declines the new cull; projected glyph rasterization, intersecting
planes, and depth sorting remain outside this change. Existing paint still
projects individual ancestor transforms to 2D, so general nested 3D geometry
does not become correct merely because facing now retains Z. The helper uses
the represented opacity/overflow grouping values; it does not add missing
filter, mask, or containment semantics. Broader `transform-style` containing
block and stacking rules are not completed by these facing fields.

The tested CSS surface is ordinary exact keywords and the listed cascade and
computed-read cases. This does not establish complete escaped-keyword,
`revert-layer`, or stylesheet conditional-rule conformance. Pure inline
absolute layout behavior and the exact vertical offset seen in the earlier
site screenshot were not changed. The host recorder demonstrates ordinary
paint commands and hit ownership, not a final guest screenshot or live-site
end-to-end success; root owns the subsequent packaged-page verification.
