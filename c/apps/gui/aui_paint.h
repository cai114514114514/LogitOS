/* aui_paint.h -- shape, gradient, shadow, glass and card drawing declarations.
 * Included by aui.h (apps include only aui.h); it relies on logit.h and
 * struct aui_rect from there. Declarations moved verbatim. */
#ifndef AUI_PAINT_H
#define AUI_PAINT_H

void aui_fill(int x, int y, int w, int h, unsigned color);
/* Uniform ALPHA fill. There is no alpha rectangle syscall; this is one 1x1 RGBA
 * pixel blitted over the whole rect, so the kernel's blit loop does the blend
 * and it costs exactly what an opaque fill costs. */
void aui_fill_a(int x, int y, int w, int h, unsigned color, int alpha);
void aui_round(int x, int y, int w, int h, int radius, unsigned color);
void aui_round_a(int x, int y, int w, int h, int radius, unsigned color, int alpha);
void aui_stroke(int x, int y, int w, int h, int radius, int thick, unsigned color);
void aui_circle(int cx, int cy, int radius, unsigned color);
void aui_ring(int cx, int cy, int radius, int thick, unsigned color);
/* Vertical / horizontal gradients. One blit of a 1-pixel-wide (or 1-tall)
 * source strip: the kernel's nearest-neighbour rescale replicates it along the
 * constant axis exactly, so a gradient is the same cost as a flat fill. */
void aui_vgrad(int x, int y, int w, int h, unsigned top, unsigned bottom);
void aui_hgrad(int x, int y, int w, int h, unsigned left, unsigned right);
void aui_vgrad_round(int x, int y, int w, int h, int radius, unsigned top, unsigned bottom);
/* Drop shadow UNDER the rect (x,y,w,h). `elev` is AUI_ELEV_1/2/3, drawn as the
 * design's E1/E2/E3 (aui_elev); aui_shadow_ex takes a recipe directly (8 tile
 * slices around the rect without SHADOW2, so a big card's shadow is no dearer
 * than a small one's). */
void aui_shadow(int x, int y, int w, int h, int radius, int elev);
void aui_shadow_ex(int x, int y, int w, int h, int radius, int dy, int blur, int alpha);
/* ---- GUI ABI v2 shadows (SYS_GUI_SHADOW2) ----
 * One ol_window_shadow2 each when the kernel reports LOGIT_GUI2_F_SHADOW2 (no
 * blur ceiling, coloured, offset, spread, inset); older kernels get the
 * mask-tile fallback documented per function. aui_shadow and aui_shadow_ex take
 * the v2 path too (blur extent -> sigma = blur / 2). The damage set is widened
 * by 3 sigma + 2 px + spread + the offset (ad_halo_shadow).
 *
 * aui_shadow2: dx, dy, spread and sigma (the gaussian standard deviation, 0..64)
 * are points; argb's alpha is the peak opacity; inset shades the INSIDE of the
 * rect (tile fallback: outer form only, inset draws nothing). */
void aui_shadow2(struct aui_rect rect, int radius, int dx, int dy, int sigma, int spread,
                 unsigned argb, int inset);
/* level 0..4 (0 draws nothing, above 4 is 4): the design's elevation ladder
 * (3.3), 1 -> E1, 2 -> E2, 3 -> E3, 4 -> E6, through aui_elev (a key, then an
 * ambient SHADOW2 in the shadow ink with the mode's alphas). */
void aui_elevation(struct aui_rect rect, int radius, int level);
/* A soft halo of argb around the rect (sigma 6 pt, spread 1 pt, no offset). */
void aui_glow(struct aui_rect rect, int radius, unsigned argb);
/* ---- GUI ABI v2 shapes (SYS_GUI_PATH) ----
 * ARGB colours: alpha is the high byte, 0 draws nothing. Each lowers to ONE
 * ol_window_path2 when the kernel reports LOGIT_GUI2_F_PATH and has a
 * documented best-effort fallback otherwise (see aui_paint.inc). aui_round,
 * aui_round_a, aui_stroke, aui_circle, aui_ring and aui_fill_a keep their
 * signatures and take the same lowering. */
void aui_round4(struct aui_rect rect, const int radius[4], unsigned argb);   /* TL, TR, BR, BL; points */
void aui_stroke_a(struct aui_rect rect, int radius, int width_q8, unsigned argb);   /* width in 24.8 points */
/* Exactly ONE device pixel wide at any backing scale; axis-aligned lines are
 * pixel-snapped and half-open ([x0,x1) or [y0,y1)). */
void aui_hairline_dev(int x0, int y0, int x1, int y1, unsigned argb);
void aui_squircle(struct aui_rect rect, int radius, unsigned argb);
/* Degrees, 0 at 3 o'clock, positive clockwise; |sweep| <= 360; width in points. */
void aui_arc(int cx, int cy, int radius, int start_deg, int sweep_deg, int width, unsigned argb);
/* Passthrough to SYS_GUI_PATH: verbs/points (24.8) are in the current origin's
 * coordinates; returns the kernel's status (OL_UNSUPPORTED without PATH). */
long aui_path(const struct logit_path2 *path, const struct logit_paint2 *paint,
              const struct logit_stroke2 *stroke);
void aui_hairline(int x, int y, int w);           /* 1px horizontal rule */
void aui_vhairline(int x, int y, int h);
/* Liquid-glass a region over the content drawn behind it: aui_material(rect,
 * radius, LOGIT_MAT_REGULAR) (design P4), whose Reduce Transparency solid is
 * drawn where glass cannot be. Draw the content first, then glass. */
void aui_glass(int x, int y, int w, int h, int radius);
void aui_panel(int x, int y, int w, int h, unsigned color);
/* A card (design 3.2, 3.3): bg.card, radius 12 (AUI_R_LG). Light: the
 * elevation shadow (AUI_ELEV_1 = E1) and the 1 dev-px outer outline #141428 @ 20.
 * Dark: the dark rim, a 1 dev-px inner stroke of ONE SYS_GUI_PATH with a LINEAR
 * paint (#FFFFFF @ 38 at the top, @ 10 at 40 %, @ 18 at the bottom); a dark
 * card raised above E1 (AUI_ELEV_2/3) keeps its shadow too. */
void aui_card(int x, int y, int w, int h, int elev);

/* ---- 「澄」 materials, backdrops, images and elevation (design P4, 3.2, 3.3;
 * added by ui-aui-core) ----
 * Rects and radii are points in the current drawing coordinates. A radius
 * r < 0 takes the material's own (design 3.2 "Radius": a capsule is h / 2);
 * all four corners are equal (GLASS2 needs four equal circular radii), except
 * that a material with cut sides (the menu bar) squares them. Each widens the
 * frame's damage through the aui_damage.h halo API: a material or backdrop by
 * its tier's reach (THIN 3, REGULAR 6, THICK 12, ULTRA 20 device px; a
 * GLASS2 panel also re-flushes when anything in its sample halo changes), an
 * elevation by offset + 3 sigma + spread.
 *
 * aui_material: ONE SYS_GUI_GLASS2 of the material's tier and per-mode tint
 *   (alpha = opacity), SYMMETRIC rim, default tone. Reduce Transparency, a
 *   kernel without GLASS2 or a refused call draw ONE solid rounded rect of
 *   the material's RT colour instead. Glass blurs the window's own content:
 *   draw what it floats over first.
 * aui_backdrop: ONE SYS_GUI_BACKDROP region `id` (0..7) showing the DESKTOP
 *   behind the window through the material; its fill_rgb (what the WM shows
 *   while it drags the window, and under Reduce Transparency) is the RT solid.
 *   Draw the region's ink AFTER it. RT or a kernel without BACKDROP: the solid
 *   rect, and a region this process registered earlier is cleared. A region
 *   stays registered until aui_backdrop_clear(id) (-1 = all of them).
 * aui_image: ONE SYS_GUI_BLIT2 of the whole sw x sh straight-RGBA image into
 *   dst with filter LOGIT_FILTER_NEAREST / _BILINEAR / _BOX; without BLIT2
 *   (or refused) the legacy nearest-neighbour gui_blit.
 * aui_elev: the elevation ladder, EXACTLY two SHADOW2 (key, then ambient) in
 *   shadow ink with the table's numbers; E0 draws nothing. Without SHADOW2,
 *   the tile shadow per layer. */
void aui_material(struct aui_rect rect, int r, enum logit_mat m);
void aui_backdrop(int id, struct aui_rect rect, int r, enum logit_mat m);
void aui_backdrop_clear(int id);
void aui_image(const unsigned char *rgba, int sw, int sh, struct aui_rect dst, int filter);
void aui_elev(struct aui_rect rect, int r, enum logit_elev e);

#endif /* AUI_PAINT_H */
