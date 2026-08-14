---
title: Open Logit 2 — from a fill engine to a 2D engine
status: design
date: 2026-08-14
---

# Open Logit 2 — Stroke, Clip, and the Fourth Rasterizer

This is a **schedule**, not an implementation spec. It fixes what "actually
usable" means as a testable claim, names the consumers that are blocked today
with the line that blocks them, and slices the work into stages (L1–L6) that each
get their own spec before they are built. Nothing here is scheduled until its
stage spec is written.

## 0. Honest inventory: what phase 1 is

`c/lib/gfx/` is 1,428 lines across six files. It does **fill only**: paths
(move/line/quad/cubic/close), nonzero and evenodd, a scanline coverage
rasterizer, four paints (solid / linear gradient / radial gradient / image),
Porter-Duff src-over, an affine transform applied to **paths**, and a **rectangle**
clip. Integer only — 24.8 coordinates, 16.16 matrices — with no libc and no
allocator. Its accuracy is a measured number against an independently written
oracle (worst pixel error: circles 0.091, rounded rects 0.047, triangles 0.119),
and it has a negative control (`-DGFX_NO_AA`) that makes 67 assertions fail.

It is already the shared engine: `GFX_OBJ` links into every GUI app through
`aui.o`, plus browser, greeter, studio, terminal, gallery and monitor.

**Its own header already writes down what phase 2 costs**, including the trap
(`gfx.h`, bottom block): a stroked corner's inner ellipse shares the outer's
**centre** and differs only in radii; insetting the centre — the mistake that
looks right — pinches the arc to nothing before it meets the straight edges, and
`test-aui-mask` caught exactly that. This document does not restate that
analysis; it schedules it and adds what the header does not cover.

## 1. The criterion

Open Logit was built to end a specific problem: three coverage/paint paths, and
every new app starting from `gui_rect`. Its stated bar was that **an engine which
coexists with what it replaced is a fourth path**. That is still the right test,
so this document adopts it as the criterion for "usable":

> **A stage earns its place only if it lets a real in-tree consumer delete code,
> or paint something it currently paints wrongly and provably.**

Not "the engine should have strokes because engines have strokes." The
corollary, which is the operational definition of done:

> **When the last stage lands there is exactly ONE rasterizer in this tree, one
> path type at each boundary, and no consumer hand-rolls coverage.**

## 2. The demand: who is blocked, and on what line

| Consumer | Blocked on | Evidence |
|---|---|---|
| `browser_paint.c` borders | stroke + dash | `border_edge()` hand-draws each edge; its own comment concedes "dashes are a fixed 3:2 approximation" (`browser_paint.c:193-202`) |
| CSS `overflow:hidden` + `border-radius` | **path clipping** | the clip is a rect in both ring 3 and the kernel (`browser_paint.c:118-128`); a rounded box cannot clip its content |
| `c/lib/image/svg.c` | the whole engine | 901 lines carrying a **third scanline filler** ("sorted-crossing winding walk") and its own `dsqrt`/`dsin` Newton loops |
| SVG `stroke` | stroke | `grep -c stroke c/lib/image/svg.c` → **0**. Every stroked icon on every real page is missing its strokes, not mis-drawn — absent |
| SVG `transform` | nothing; the engine has it | svg.c's header lists `transform` under "skipped, never fatal" |
| gradient / stroked / transformed **text** | glyph outlines as paths | `ttf_glyph_path()` already returns them (`ttf.h:68`), but text reaches the screen only through the kernel's `SYS_GUI_TEXT_RUN` |
| `box-shadow` on a non-rounded-rect | real blur | `gfx_corner_shadow` is an **analytic distance ramp**, correct for a rounded rect and inapplicable to an arbitrary shape (`gfx.h`, mask-cache block) |
| `aui` rings | a general stroker | `gfx_corner_ring` is a hard-coded mask kind, not a stroke |

### 2.1 The fourth rasterizer is real, and it is in ring 0

`c/lib/image/svg.c` survived Open Logit's consolidation for a mechanical reason:
`C_SRC` (`Makefile:236`) filters out `c/lib/video`, `c/lib/audio`, `c/lib/media`,
`inflate.c` and `png.c` — **and not `svg.c`**. So it compiles into the kernel,
where it cannot call the ring-3 engine, and it is *also* compiled into the browser
(`Makefile:624`, `:831`). One file, two builds, its own rasterizer.

That is the exact thing this engine exists to prevent, and it is also a ring-0
argument identical to the one CLAUDE.md already makes for the H.264 decoder:
**an SVG's path data is attacker-shaped geometry**, and filling it in ring 0 under
the BKL serves no ring-0 caller that could not be served from ring 3.

## 3. The constraints that shape every stage

These are not preferences; each one closes off an obvious design.

- **No allocator.** Six GUI apps link crt0 + aui + gfx and nothing else; path
  storage is the caller's. **Clipping, groups and blur each need a buffer**, and
  where that buffer comes from is the central design question of L2, L5 and L6 —
  not an implementation detail to be discovered later. An engine that quietly
  grows a malloc dependency breaks its own linkage story for six apps.
- **`gfx_path` applies the CTM as points are recorded** (`gfx.h:126`). A recorded
  path is therefore already in device space and **cannot be re-transformed**. That
  is right for a one-shot fill and wrong for a glyph outline reused at several
  sizes, so L4 must decide whether to re-record per use or to carry an untransformed
  form. Naming it now stops it being discovered at the end of L4.
- **Two path types.** `struct fp_path` (`c/lib/text/fontpath.h:36`) carries font
  commands (FP_QUAD from glyf, FP_CUBIC from CFF) in font units, y-up. `struct
  gfx_path` carries flattened 24.8 device points, y-down. Both are correct for
  their side; the bridge between them is a real, small, schedulable item and it
  belongs to L0, not to whichever stage trips over it first.
- **`GFX_MASK_MAX` is 72 device px, and exceeding it fails SILENTLY.** A 90pt
  clock dial asked `aui_round` for a mask, was refused, and drew a **square** —
  no error anywhere. This tree's standing rule is that a thing which cannot work
  says so; a silent geometric fallback is the worst available outcome, and L0
  fixes the reporting before any stage adds more mask kinds.
- **The oracle discipline is the deliverable, not the code.** Phase 1's real
  achievement was that every filled shape is checked against a 16×16 supersampled
  evaluation of its own analytic predicate. Every stage below must name its
  oracle *before* it is built, and a stage without one is not scheduled.

## 4. The stages

### L0 — the seams (small, unblocks the rest)

`fp_path` → `gfx_path` bridge; make the `GFX_MASK_MAX` refusal observable instead
of silent; give callers a documented path-storage discipline for geometry too big
to cache. No new drawing capability. **Gate:** the clock dial's failure mode
becomes an assertion; a glyph outline round-trips into a filled path with the same
worst-pixel-error bound phase 1 already meets.

### L1 — STROKE

The largest single unlock. Flatten, then emit the offset outline as two
oppositely-wound subpaths and fill **nonzero** — the road `gfx.h` already says is
open because this engine flattens to device-space polylines. The work is joins
(miter with limit, round, bevel), caps (butt/round/square), dashing, and the
degenerate cases: a zero-length subpath with round caps is a dot; a cusp inside a
curve reverses the offset direction.

**Oracle:** a stroke's analytic predicate is *distance from the source path ≤
w/2*, which is brute-force computable by supersampling — the same trick phase 1
used, applied to a different predicate. Miter joins need their own predicate
(the miter tip is exactly computable) or they are checked by construction.
**Gate:** worst pixel error against that reference within phase 1's envelope;
`gfx_corner_ring` deleted and aui's rings served by the stroker at equal or better
error; `border_edge`'s 3:2 dash approximation deleted.
**Negative control:** `-DGFX_NO_JOINS` (butt-join everything) must fail.

### L2 — PATH CLIPPING

Coverage × coverage. `gfx.h` sketches both roads — a rasterized clip **mask**
(small change to the span consumer, large change to who owns the buffer) versus
**span-list intersection** (exact, stateless, needs the clip's spans for every row
of every fill). The choice is dictated by the no-allocator constraint above and
must be argued in L2's spec, not assumed.
**Gate:** `overflow:hidden` on a `border-radius` box clips its content, proven by
a reftest, not a screenshot. **Negative control:** clip disabled must fail it.

### L3 — SVG onto the engine, in ring 3

Delete `c/lib/image/svg.c`'s rasterizer and its `dsqrt`/`dsin`; keep its parser.
Move the result out of `C_SRC`. This is the stage that satisfies §1's corollary,
and it pays three times: one rasterizer instead of two, `transform` and `stroke`
arrive for free from L1/L0, and a media parser leaves ring 0.
**Gate:** the octicon corpus svg.c was written against renders identically or
better, per-pixel; `test-svg` stays green; the kernel no longer links a filler.

### L4 — TEXT AS PATHS (selective)

`ttf_glyph_path()` already exists. This is **not** a replacement for the kernel's
glyph rasterizer and cache — that is what makes text fast, and Open Logit's
construction was deliberately copied from it so a shape and the type on it are
antialiased by the same rule at the same size. L4 adds the cases the cache cannot
serve: display-size type, gradient-filled or stroked text, transformed text, and
SVG `<text>`.
**Gate:** a glyph filled through the engine and the same glyph through
`raster.c` agree within a stated bound at the sizes where both are valid — which
is also the first independent check the kernel rasterizer has ever had.

### L5 — GROUPS, OPACITY, BLEND

CSS `opacity` on a subtree and `mix-blend-mode` need an off-screen layer, which
needs the buffer question answered. Scheduled after L2 because L2 answers it.
**Gate:** Porter-Duff and the separable blend modes recomputed in double, the
same oracle shape phase 1 used for src-over (175 combinations, 1/255).

### L6 — BLUR, and a real shadow

Replace the analytic ramp for the arbitrary-shape case with a real blur (three
box passes approximate a Gaussian to within the error budget and are integer).
Keeps the ramp for rounded rects, where it is cheaper and exact enough.
**Gate:** against a double-precision Gaussian; the existing corner-tile shadow
path unchanged and still cheap.

## 5. Schedule

| Stage | Unlocks | Deletes | Gate |
|---|---|---|---|
| **L0** | everything after it | — | mask refusal observable; glyph outline round-trip |
| **L1** | CSS borders, SVG stroke, aui rings | `gfx_corner_ring`, `border_edge`'s dash approximation | distance-field oracle + `-DGFX_NO_JOINS` control |
| **L2** | `overflow:hidden` + radius, `clip-path` | ring-3/kernel dual rect clip | reftest + disabled-clip control |
| **L3** | SVG transform & stroke | **`svg.c`'s rasterizer, `dsqrt`, `dsin`** | octicon corpus per-pixel; kernel links no filler |
| **L4** | gradient/stroked/transformed text, SVG `<text>` | — | agreement with `raster.c` within a stated bound |
| **L5** | `opacity` subtrees, `mix-blend-mode` | — | blend modes vs double |
| **L6** | arbitrary-shape shadow, `filter: blur()` | — | vs double-precision Gaussian |

Order is by unblocking, not by difficulty. L1 and L2 are independent of each
other and both precede L3, which is the stage that makes the §1 claim true.

## 6. Non-goals (locked)

- **No allocator inside the engine.** If a stage needs a buffer, the caller owns
  it. The six-app linkage story is a feature.
- **No floating point.** Integer 24.8 / 16.16 throughout; the kernel is
  `-mno-sse` for glyphs and the engine's arithmetic must stay comparable.
- **No replacement of `raster.c`.** L4 adds cases; it does not take the glyph
  cache's job.
- **No GPU path.** virtio-gpu presents a RAM-backed scanout; there is no 3D
  pipeline to target and inventing one is a different project.
- **No SVG features the parser does not already reach** (`defs`, filters,
  markers, animation). L3 moves the rasterizer; growing the parser is separate.
- **No new mask kinds before L0.** Every one added today inherits the silent
  72-pixel failure.
