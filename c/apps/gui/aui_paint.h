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
/* Drop shadow UNDER the rect (x,y,w,h): 8 slices around it, nothing inside, so
 * the cost is O(perimeter * blur) and a big card's shadow is no dearer than a
 * small one's. `elev` is AUI_ELEV_*; aui_shadow_ex takes the recipe directly. */
void aui_shadow(int x, int y, int w, int h, int radius, int elev);
void aui_shadow_ex(int x, int y, int w, int h, int radius, int dy, int blur, int alpha);
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
/* Liquid-glass a region over the content drawn behind it (theme-tinted). For
 * app chrome (sidebars/toolbars/cards) -- draw the content first, then glass. */
void aui_glass(int x, int y, int w, int h, int radius);
void aui_panel(int x, int y, int w, int h, unsigned color);
/* A raised surface with a radius, a hairline and an elevation shadow. */
void aui_card(int x, int y, int w, int h, int elev);

#endif /* AUI_PAINT_H */
