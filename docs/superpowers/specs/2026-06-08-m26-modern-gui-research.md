# M26 — Modern GUI overhaul: research findings

Research note (not yet a spec). The user asked to "search M26 carefully." Repo
`M26` (the spec) was TCP robustness — done; here **M26 = the next milestone = the
modern GUI overhaul**, the "too ugly to look at" GUI ([[modern-gui-overhaul]]).
The libc expansion just done is its groundwork. Findings from a 4-thread research
sweep (codebase foundation + SVG/vector + software blur + modern UI), with the key
claims verified against the code.

## What M26 is
Take Aether's GUI from "2010s Win7"-dated to macOS-grade modern: real-time
backdrop **blur/vibrancy**, **vector (SVG-grade) icons + paths**, soft multi-layer
shadows, **rounded everything**, semantic **color tokens + dark mode**, an 8px
**spacing/type scale**, and (optional) subtle **motion**.

## We are NOT starting from scratch (verified foundation)
Aether's GUI is already unusually modern for a from-scratch OS:
- `fb.c`: `fb_fill_circle`, `fb_round_rect`, `fb_blend_rect`, `fb_blend_round_rect`
  ("frosted-glass surfaces", alpha 0–255) — rounded + alpha already exist.
- `wm.c`: a `glass_blur()` (box blur) + a 3-layer `shadow_band()` drop shadow —
  the dock + menu bar are ALREADY frosted glass + shadowed. Full-frame compositor
  (my recent change composites the whole screen + cursor each frame).
- `raster.c`/`text.c`: an integer AA glyph rasterizer (4× oversample + per-scanline
  winding coverage → 0–255 alpha) + a 2048-glyph cache. A general path rasterizer
  can reuse this coverage engine.
- `virtio_gpu.c`: RAM scanout + DMA present (no 3D accel exposed).

So the "ugly" is mostly the **aui toolkit + the apps** (flat, hardcoded colors,
ASCII dock icons, cramped spacing), not a missing engine.

## Gaps = M26 scope
No vector **path** rasterization (only TTF outlines) · no **stroking** · no
**gradients** · blur is **baked once at init, not real-time** (and O(r²)) · no
**icon system** (ASCII glyphs in the dock) · no semantic **color tokens / dark
mode** · no **motion** · hardcoded spacing/type.

## Approach (the chosen techniques)
1. **Vector graphics — EXTEND, don't port.** Reuse the glyph rasterizer's coverage
   model for arbitrary paths: cubic Bézier flattening (de Casteljau) feeding the
   existing scanline winding fill; stroking via offset-the-centerline + caps/joins;
   integer 8.8 fixed-point (no FPU → kernel-safe). NOT NanoSVG/PlutoVG.
2. **Real-time backdrop blur — separable box blur.** Replace the O(r²) baked
   `glass_blur` with an O(r) separable (horizontal + vertical moving-sum) integer
   blur as `fb_blur_rect(x,y,w,h,radius)`; temp buffers allocated once. Est. ~2–4 ms
   for full 1280×800 (radius-independent). Vibrancy = blur the backdrop in the WM
   (apps are opaque, so the compositor must do it) + translucent tint + shadow.
3. **Color** — semantic tokens (`AUI_SUCCESS/WARNING/ERROR`, focus ring), light/dark
   sets, an HSL accent function. ~100 lines, integer LUTs.
4. **Type/spacing** — replace `aui.c`'s hardcoded `PX=15` with `TEXT_BODY/LABEL/
   HEADING`; 8px spacing scale; `aui_vstack/hstack` layout helpers.
5. **Icons** — procedural monochrome vector icons, rasterized + cached like glyphs.
   MVP: ~20–30 filled-shape icons (folder/file/gear/x/min/max); full stroking later.
6. **Motion (optional)** — 150–250 ms ease transitions (focus glow, press, raise).

## Honest hard parts / cost
- **Stroking** (offset geometry, joins/caps) is the algebraically fiddly bit —
  MVP can use filled shapes and defer real stroking.
- **Real-time blur perf** must be the separable version; verify the per-frame cost
  on real TCG timing first (the synthesis estimate is ~5–7 ms total overhead in a
  16 ms budget, but measure).
- **Vibrancy must live in the WM/kernel** (apps can't see behind their opaque
  surface) — a per-window `vibrancy` flag → the compositor blurs the backdrop.
- Kernel stays `-mno-sse` (integer blur/raster); userland could SSE2-accelerate if
  a ring-3 compositor layer ever appears.

## Suggested phase order (each ships a visible win, low risk)
P1 **vector icons** (extend raster + procedural icons → dock/buttons stop using
ASCII) → P2 **real-time blur/vibrancy** (fb_blur_rect + per-window backdrop) → P3
**color tokens + dark mode** → P4 **type/spacing scale + layout helpers** → P5
**gradients** → P6 **motion**. Start: measure baseline frame time, add cubic Bézier
to the rasterizer, sketch the icon DSL with ~5 icons in the dock.

Next: turn this into a real M26 spec (brainstorm → spec → plan) and prototype P1.
