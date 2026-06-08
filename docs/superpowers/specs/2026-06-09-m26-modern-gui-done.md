# M26 — Modern GUI overhaul: COMPLETE (P1–P6)

Built from scratch (no ported library — no NanoSVG/PlutoVG/etc.), each phase
QEMU-verified (`-smp 4`, screenshots) and committed on `m13-html-css`. Takes the
desktop from "2010s-flat" to macOS-grade. Research → `2026-06-08-m26-modern-gui-research.md`.

## What shipped

**P1 — vector icons** (`f81153d`). Extended the integer AA glyph rasterizer into a
general vector path rasterizer: factored the 4× scanline nonzero-winding fill into
`fill_edges()`, added a cubic-Bézier flattener, and `vg_render_path()` (move/line/
quad/cubic/close in a 0..unit box → coverage bitmap), all integer/fixed-point
(kernel is `-mno-sse`). New `vg.h`. `icons.c`: 9 procedural monochrome icons
(folder/doc/terminal/grid/globe/code/chart/clock/image) as `vg_cmd` paths with
holes via reverse winding; rasterized once per (id,px) and cached. The Dock draws
these per app (`icon_for_app`) instead of single ASCII letters.

**P2 — real-time blur / vibrancy** (`467be99`). Replaced the baked-once O(r²)
`glass_blur` with `fb_blur_rect(x,y,w,h,radius,corner)` — a separable box blur
(horizontal moving-sum → scratch → vertical pass), O(w·h), radius-independent,
integer-only; `corner>0` rounds the written region. The menu bar + dock are now
composited **per-frame on top of the windows** and blur the **live** backdrop, so
a window slid under them reads as true frosted glass, and the dock floats above
windows (fixed the old "window covers the dock" occlusion bug). `bg` holds only
the wallpaper now.

**P3 — color tokens + dark mode** (`a4a1a3a`). aui's 5 hardcoded color #defines →
a runtime `struct aui_theme aui_t` with semantic tokens (bg/surface/face/text/
muted/border/hi/accent/accent_text + success/warning/error/focus). `aui_set_dark()`
swaps the palette; `aui_hsl()` is integer HSL→rgb; `aui_set_accent()` recolors.
Widget edges use the tokens so dark renders correctly. Init fix: each token
lazy-inits via `(aui_ensure(), aui_t.x)` (a token's value is evaluated at the call
site, before `aui_begin`'s body — a plain read saw zeroed/black fields frame 1).
`widgets.c` has a Dark-mode toggle + success/warning/error swatches.

**P4 — type + spacing scale + layout** (`2560fd0`). Type scale (`AUI_FS_CAPTION/
LABEL/BODY/TITLE/HEADING`), 4px spacing scale (`AUI_SP(n)`, `AUI_GAP`, `AUI_PAD`),
`aui_heading()`/`aui_text_sz()`/`aui_text_w()`, and a linear stack layout
(`aui_vstack`/`aui_hstack` + `aui_next(w,h,&x,&y)`). widgets.c uses a heading-size
title and an hstack swatch row.

**P5 — gradients** (`378de05`). `fb_fill_vgrad`/`fb_round_rect_vgrad` (one color
lerp per row) + `fb_shade` (lighten/darken a packed color). Dock tiles are now
glossy vertical gradients (shade +38 → base → −26); titlebars get a subtle
top→bottom gradient.

**P6 — motion** (`7b4d177`). Live dock hover magnification: the icon under the
cursor grows 1.3× in place (kept inside the panel + gap, no overlap) with a name
tooltip above — the signature macOS dock feel. `dock_tile(i,cx,cy,sz)` helper
(radius + icon scale track sz). Recomputed each frame from the cursor; the WM
already repaints on every mouse move, so it animates with no animation timer — the
delicate BKL/idle render loop is untouched. Launch hit-test stays on the base grid
(the magnified icon stays centred on its cell).

## Beyond the six phases (same session, all QEMU-verified)

**Dock launch bounce** (`b746a26`). On launch the app's dock icon bounces (two
decaying parabolic hops ~0.55s); `wm_launch` stamps `reg_bounce[i]`, `draw_dock`
lifts the icon and keeps `dirty=1` while it runs (~100fps off the existing 100Hz
timer wake -- no new timer).

**Window open "pop"** (`6ba69a6`). New windows scale 0.84->1.0 over ~0.16s
(easeOutCubic): `wm_render` renders the whole window into a scratch surface and
`fb_blit_surface_scaled`-blits it while the pop runs (`draw_frame` split into
`draw_frame_body` at an explicit rect). Verified via a forced-50% build (half-size
render) + a serial trace of the per-frame elapsed (e=1,2,3,5,... ticks).

**System-wide light/dark mode** (`d55560a`). The flagship: P3's per-app palette
becomes a real system setting. `SYS_UI_DARK` (70) get/set + a menu-bar toggle
switch; `wm_set_dark` flips `g_ui_dark`, broadcasts `EV_THEME` (#5) to all windows,
marks dirty. The kernel chrome (menu bar, dock glass, every window frame) reads
`g_ui_dark` and switches instantly; aui apps follow (each `aui_begin` queries
SYS_UI_DARK and reloads on change, clearing with the fresh bg so the EV_THEME
repaint is consistent). The whole desktop -- chrome + the aui apps (Finder /
Monitor / Clock / Widgets) -- flips together.

## New kernel fb primitives (reusable)
`fb_blur_rect`, `fb_fill_vgrad`, `fb_round_rect_vgrad`, `fb_shade`,
`fb_blit_surface_scaled`, plus `vg_render_path` (raster.c) +
`icon_draw`/`icon_for_app` (icons.c). New ABI: `SYS_UI_DARK`, `EV_THEME`.

## Deferred / not done
- **Path stroking** (offset geometry, joins/caps) — icons use filled silhouettes.
  The one explicitly-user-named capability ("SVG等") still open; needs a clean
  consumer before it's worth adding.
- **aui gradient buttons** — no `gui_*` gradient syscall yet (gradients are
  kernel-side WM chrome only).
- **Dark mode for the non-aui apps** — textedit / terminal / preview / studio /
  browser draw with custom code, so only their WM frame darkens, not their content.

## Resolved since the first pass
- ~~Time-based easing~~ → DONE (launch bounce + window open pop), no animation
  timer: the WM keeps `dirty=1` during an animation and steps off the 100Hz wake,
  so the delicate BKL/idle loop is untouched.
- ~~WM frame dark theming~~ → DONE (system-wide dark mode darkens all chrome).
