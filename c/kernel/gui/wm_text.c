/* wm_text.c -- kernel chrome text helpers; see wm_text.h for the contract.
 * Design 3.6 (windowTitle 13/600, menuBar 14/400, app name 14/600, tabular
 * clock, metric centring) and 5.2 (two-pass shadow ink).
 *
 * text.c gains nothing: everything here goes through its public entry points
 * (text_draw_run_a, text_measure, text_measure_run_metrics) and the same
 * weight -> face resolver the TEXT_RUN2 handler uses, so the glyph cache, the
 * fallback chain and the quarter-pixel pen are shared. No fb_* call except
 * fb_scale() in the wrappers that take no explicit scale.
 *
 * -DUI_KTEXT_NEGCTL: wm_text_tnum places digits by their NATURAL advances
 * instead of the shared cell (the control for tests/ui-kernel-text.mk). */
#include "wm_text.h"
#include "wm2_scale.h"
#include "text.h"
#include "logit_abi.h"
#include "ui/logit_tokens.h"

unsigned text_face_for_weight(unsigned face, int weight);   /* text.c */
int fb_scale(void);                                         /* fb.c */

#define WT_BUF 256                 /* ellipsis staging: bytes incl. the tail */
static const char WT_ELL[] = "\xE2\x80\xA6";       /* U+2026 */

static unsigned wt_face(int weight)
{
    return weight > 0 ? text_face_for_weight(0, weight) : 0;
}
static int wt_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static int wt_px(int px, int pct)
{
    int v = wm2_pt(px, pct);
    return v < 1 ? 1 : v;
}

static int wt_advance(const char *s, int len, int pxd, unsigned face)
{
    return len > 0 ? text_measure(s, len, pxd, (int)face) : 0;
}

/* Distance from a run's top to its baseline in the face text.c picks, device
 * px. The metrics query is the engine's own ascent_px, so the baseline we
 * compute is the one it draws on. */
static int wt_ascent(int pxd, unsigned face)
{
    struct logit_text_metrics m;
    if (text_measure_run_metrics("", 0, pxd, (int)face, &m) > 0) return m.ascent;
    return (pxd * LOGIT_FONT_ASCENT) / LOGIT_FONT_UPEM;
}

static int wt_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int wt_align_x(int x, int w, int align)
{
    return align == WM_TEXT_RIGHT ? x - w : align == WM_TEXT_CENTER ? x - w / 2 : x;
}

int wm_text_measure_w_pct(int pct, int px, int weight, const char *s)
{
    return wt_advance(s, wt_len(s), wt_px(px, pct), wt_face(weight));
}

int wm_text_w_pct(int pct, struct ol_display *d, int x, int y, int px, int weight,
                  unsigned rgb, const char *s)
{
    int len = wt_len(s);
    if (len <= 0) return 0;
    int pxd = wt_px(px, pct), dx = wm2_pt(x, pct);
    int end = text_draw_run_a(d, dx, wm2_pt(y, pct), s, len, pxd, wt_face(weight),
                              rgb & 0xFFFFFFu, 255);
    return end - dx;
}

/* ---- tabular digits ----------------------------------------------------
 * cache[]: the widest digit advance for (px, weight, scale). WM-lock
 * protected (wm_text.h). Round-robin replacement; a scale change simply keys
 * a different entry, so nothing is ever stale. */
struct wt_cell { int pxd, weight, pct, cell; };
static struct wt_cell wt_cache[8];
static int wt_cache_n, wt_cache_next;

static int wt_digit_cell(int pxd, int weight, int pct)
{
    for (int i = 0; i < wt_cache_n; i++)
        if (wt_cache[i].pxd == pxd && wt_cache[i].weight == weight && wt_cache[i].pct == pct)
            return wt_cache[i].cell;
    unsigned face = wt_face(weight);
    int cell = 0;
    for (char c = '0'; c <= '9'; c++) {
        int w = text_measure(&c, 1, pxd, (int)face);
        if (w > cell) cell = w;
    }
    int slot = wt_cache_n < 8 ? wt_cache_n++ : wt_cache_next++ & 7;
    wt_cache[slot] = (struct wt_cell){ pxd, weight, pct, cell };
    return cell;
}

/* Lay the string out: digits advance by `cell` (the pen steps by it, so the
 * Nth glyph sits at the same x whatever digits precede it), every
 * other run its natural advance. emit != 0 draws at x0. Returns the width. */
static int wt_tnum_pass(struct ol_display *d, int x0, int y, int pxd, unsigned face,
                        int cell, unsigned rgb, const char *s, int len, int emit)
{
    int pen = 0;
    for (int i = 0; i < len;) {
        int isd = s[i] >= '0' && s[i] <= '9';
        int j = i + 1;
        if (!isd) while (j < len && !(s[j] >= '0' && s[j] <= '9')) j++;
        int w = wt_advance(s + i, j - i, pxd, face);
#ifdef UI_KTEXT_NEGCTL
        int adv = w;
        int off = 0;
        (void)cell;
#else
        int adv = isd ? cell : w;
        int off = 0;
#endif
        if (emit) text_draw_run_a(d, x0 + pen + off, y, s + i, j - i, pxd, face, rgb, 255);
        pen += adv;
        i = j;
    }
    return pen;
}

int wm_text_tnum_pct(int pct, struct ol_display *d, int x, int y, int px, int weight,
                     unsigned rgb, const char *s, int align)
{
    int len = wt_len(s);
    if (len <= 0) return 0;
    int pxd = wt_px(px, pct);
    unsigned face = wt_face(weight);
    int cell = wt_digit_cell(pxd, weight, pct);
    int w = wt_tnum_pass(0, 0, 0, pxd, face, cell, 0, s, len, 0);
    wt_tnum_pass(d, wt_align_x(wm2_pt(x, pct), w, align), wm2_pt(y, pct), pxd, face,
                 cell, rgb & 0xFFFFFFu, s, len, 1);
    return w;
}

/* ---- vertical centring ------------------------------------------------- */
int wm_text_baseline(int top, int h, int px)
{
    return (logit_text_baseline24(top * 256, h * 256, px) + 128) >> 8;
}

int wm_text_vc_pct(int pct, struct ol_display *d, struct wm_text_rect r, const char *s,
                   int px, int weight, unsigned rgb, int align)
{
    int len = wt_len(s);
    if (len <= 0) return 0;
    int pxd = wt_px(px, pct);
    unsigned face = wt_face(weight);
    /* Edges convert separately so abutting rects never open a seam. */
    int x0 = wm2_pt(r.x, pct), x1 = wm2_pt(r.x + r.w, pct);
    int t = wm2_pt(r.y, pct), b = wm2_pt(r.y + r.h, pct);
    int w = wt_advance(s, len, pxd, face);
    int base = (logit_text_baseline24(t * 256, (b - t) * 256, pxd) + 128) >> 8;
    int dx = align == WM_TEXT_RIGHT ? x1 - w : align == WM_TEXT_CENTER ? x0 + (x1 - x0 - w) / 2 : x0;
    text_draw_run_a(d, dx, base - wt_ascent(pxd, face), s, len, pxd, face, rgb & 0xFFFFFFu, 255);
    return w;
}

/* ---- shadowed ink (5.2) ------------------------------------------------ */
int wm_text_shadowed_pct(int pct, struct ol_display *d, int x, int y, int px, int weight,
                         unsigned ink, unsigned shadow_argb, const char *s)
{
    int len = wt_len(s);
    if (len <= 0) return 0;
    int pxd = wt_px(px, pct), dx = wm2_pt(x, pct), dy = wm2_pt(y, pct);
    unsigned face = wt_face(weight);
    int a = (int)(shadow_argb >> 24) & 255;
    int off = wm2_pt(1, pct);
    if (off < 1) off = 1;
    if (a) text_draw_run_a(d, dx, dy + off, s, len, pxd, face, shadow_argb & 0xFFFFFFu, a);
    return text_draw_run_a(d, dx, dy, s, len, pxd, face, ink & 0xFFFFFFu, 255) - dx;
}

/* ---- ellipsis ---------------------------------------------------------- */
int wm_text_ellipsis_pct(int pct, struct ol_display *d, struct wm_text_rect r, const char *s,
                         int px, int weight, unsigned rgb)
{
    int len = wt_len(s);
    if (len <= 0) return 0;
    int pxd = wt_px(px, pct);
    unsigned face = wt_face(weight);
    int avail = wm2_pt(r.x + r.w, pct) - wm2_pt(r.x, pct);
    if (wt_advance(s, len, pxd, face) <= avail)
        return wm_text_vc_pct(pct, d, r, s, px, weight, rgb, WM_TEXT_LEFT);
    char buf[WT_BUF];
    int ell = wt_advance(WT_ELL, 3, pxd, face);
    /* Largest code-point-aligned prefix whose width plus the tail fits. lo and
     * hi are always boundaries, so the search never splits a sequence. */
    int lo = 0, hi = len < WT_BUF - 4 ? len : WT_BUF - 4;
    while (hi > 0 && hi < len && wt_cont(s[hi])) hi--;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        while (mid < hi && wt_cont(s[mid])) mid++;
        if (wt_advance(s, mid, pxd, face) + ell <= avail) lo = mid;
        else {
            hi = mid - 1;
            while (hi > lo && wt_cont(s[hi])) hi--;
        }
    }
    int n = lo;
    while (n > 0 && s[n - 1] == ' ') n--;
    for (int i = 0; i < n; i++) buf[i] = s[i];
    for (int i = 0; i < 3; i++) buf[n + i] = WT_ELL[i];
    buf[n + 3] = 0;
    return wm_text_vc_pct(pct, d, r, buf, px, weight, rgb, WM_TEXT_LEFT);
}

/* ---- WM-scale wrappers ------------------------------------------------- */
int wm_text_measure_w(int px, int weight, const char *s)
{ return wm_text_measure_w_pct(fb_scale(), px, weight, s); }
int wm_text_w(struct ol_display *d, int x, int y, int px, int weight, unsigned rgb, const char *s)
{ return wm_text_w_pct(fb_scale(), d, x, y, px, weight, rgb, s); }
int wm_text_tnum(struct ol_display *d, int x, int y, int px, int weight, unsigned rgb,
                 const char *s, int align)
{ return wm_text_tnum_pct(fb_scale(), d, x, y, px, weight, rgb, s, align); }
int wm_text_vc(struct ol_display *d, struct wm_text_rect r, const char *s, int px, int weight,
               unsigned rgb, int align)
{ return wm_text_vc_pct(fb_scale(), d, r, s, px, weight, rgb, align); }
int wm_text_shadowed(struct ol_display *d, int x, int y, int px, int weight, unsigned ink,
                     unsigned shadow_argb, const char *s)
{ return wm_text_shadowed_pct(fb_scale(), d, x, y, px, weight, ink, shadow_argb, s); }
int wm_text_ellipsis(struct ol_display *d, struct wm_text_rect r, const char *s, int px,
                     int weight, unsigned rgb)
{ return wm_text_ellipsis_pct(fb_scale(), d, r, s, px, weight, rgb); }
