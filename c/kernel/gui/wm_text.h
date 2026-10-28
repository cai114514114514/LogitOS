/* wm_text.h -- kernel chrome text helpers (design 3.6 type, 5.2 shadow ink).
 * The kernel twin of aui's text API for the window manager's own chrome: a
 * CSS weight on the same entry SYS_GUI_TEXT_RUN2 uses (text_draw_run_a +
 * text_face_for_weight), tabular digits, metric vertical centring, a two-pass
 * shadowed ink and a U+2026 tail ellipsis. Nothing calls it yet (no visual
 * change); the shell items adopt it.
 *
 * UNITS. Positions, sizes and rects are LOGICAL POINTS (ints, like fb_pt's
 * input); the helper converts with the WM scale (fb_scale(), percent) using
 * wm2_pt's floor rule, so one run lands where a legacy whole-point run would.
 * Every function that draws or measures returns a width in DEVICE PIXELS (what
 * text_measure answers), because the caller's next placement is a device pixel
 * too. Fallback chain is text.c's own (ui.ttf then text.ttf: the command key
 * and friends), identical to fb_text.
 *
 * The *_pct twins take the scale explicitly (host tests, and callers that
 * already hold one scale for a whole pass).
 *
 * LOCKING. All of these run on the compositor/WM thread under the WM lock
 * (they are chrome painters). wm_text_tnum keeps an 8-entry cache of the
 * tabular cell width per (px, weight, scale) in file-scope state that the WM
 * lock protects; it is not safe from another thread. The text engine's own
 * text_lock is taken inside text_measure / text_draw_run_a and is never held
 * across the calls here. Colour is 0xRRGGBB (the high byte is dropped). */
#ifndef LOGIT_WM_TEXT_H
#define LOGIT_WM_TEXT_H

struct ol_display;

enum { WM_TEXT_LEFT = 0, WM_TEXT_CENTER = 1, WM_TEXT_RIGHT = 2 };

struct wm_text_rect { int x, y, w, h; };   /* logical points */

/* A run with its TOP at (x, y), px points, CSS weight 100..900 (0 = regular). */
int wm_text_w(struct ol_display *d, int x, int y, int px, int weight,
              unsigned rgb, const char *s);
int wm_text_measure_w(int px, int weight, const char *s);

/* Tabular digits: every 0-9 occupies the widest digit's cell so a changing
 * clock never jitters. x is the left edge, the centre or the right edge
 * (the last digit's right side) per `align`. */
int wm_text_tnum(struct ol_display *d, int x, int y, int px, int weight,
                 unsigned rgb, const char *s, int align);

/* baseline = top + (h - 1.448 px) / 2 + 1.160 px (logit_tokens metrics),
 * in whole points (rounded). */
int wm_text_baseline(int top, int h, int px);

/* One line centred in `r` by font metrics, aligned horizontally within it. */
int wm_text_vc(struct ol_display *d, struct wm_text_rect r, const char *s,
               int px, int weight, unsigned rgb, int align);

/* Two passes: the shadow (argb: alpha in the high byte) one point lower, then
 * the ink on top. */
int wm_text_shadowed(struct ol_display *d, int x, int y, int px, int weight,
                     unsigned ink, unsigned shadow_argb, const char *s);

/* wm_text_vc, left aligned, cut at a code point boundary with U+2026 when it
 * does not fit r.w. */
int wm_text_ellipsis(struct ol_display *d, struct wm_text_rect r, const char *s,
                     int px, int weight, unsigned rgb);

/* Explicit-scale twins. */
int wm_text_w_pct(int pct, struct ol_display *d, int x, int y, int px, int weight,
                  unsigned rgb, const char *s);
int wm_text_measure_w_pct(int pct, int px, int weight, const char *s);
int wm_text_tnum_pct(int pct, struct ol_display *d, int x, int y, int px, int weight,
                     unsigned rgb, const char *s, int align);
int wm_text_vc_pct(int pct, struct ol_display *d, struct wm_text_rect r, const char *s,
                   int px, int weight, unsigned rgb, int align);
int wm_text_shadowed_pct(int pct, struct ol_display *d, int x, int y, int px, int weight,
                         unsigned ink, unsigned shadow_argb, const char *s);
int wm_text_ellipsis_pct(int pct, struct ol_display *d, struct wm_text_rect r, const char *s,
                         int px, int weight, unsigned rgb);

#endif /* LOGIT_WM_TEXT_H */
