/* Host stand-in for c/apps/logit.h that RASTERIZES instead of recording.
 * Used only by tests/unit/reftest.c (the WPT reftest harness).
 *
 * WHY A SECOND SHIM. tests/unit/painthost/logit.h turns the five GUI syscalls
 * into a list of draw ops, which is what an assertion about "did the painter
 * choose alpha 128" wants. A reftest asks a different question -- are these two
 * documents the SAME PICTURE -- and that question only has an answer once the
 * ops have become pixels. So this header is the same trick with a different
 * sink: `#include "logit.h"` resolves here (via -Itests/unit/refhost, which must
 * come first on the include path) and the real c/apps/browser/browser_paint.c
 * is linked and run unmodified.
 *
 * WHAT IS SHARED WITH browser.aex AND WHAT IS NOT -- read this before believing
 * any number the harness prints:
 *
 *   SHARED (the real files, linked):  dom.c html_tokenizer.c html_tree.c
 *   dom_serialize.c css_engine.c css_vars.c css_extra.c layout.c
 *   browser_paint.c, LibCSS, and c/lib/gfx.
 *
 *   NOT SHARED (this header):  the last step, ops -> pixels. On the machine
 *   that step is the kernel's c/kernel/gui/fb.c under c/kernel/gui/wm.c's
 *   syscall cases. The four routines below are transcribed from those two
 *   files -- fb_fill_rect, fb_round_rect's boolean dx*dx+dy*dy <= r*r corner,
 *   and fb_blit_rgba's nearest-neighbour scale with src-over -- so the geometry
 *   rule is the same rule. They are a TRANSCRIPTION and not the code, and a
 *   divergence in fb.c would not be caught here.
 *
 *   NOT SHARED (and this is the big one):  TEXT. On the machine a glyph comes
 *   from c/lib/text's TrueType rasterizer against /fonts/{ui,mono}.ttf, and
 *   text_measure is a syscall into the same metrics. Ring 3 cannot be linked to
 *   that here, and every existing host test in this tree already substitutes a
 *   metric stub (`len * (px/2)` in paint_test.c). This harness substitutes the
 *   AHEM model instead -- see ref_glyph below -- because that is what the WPT
 *   corpus is written against, not because it is what the browser does.
 */
#ifndef REFHOST_LOGIT_H
#define REFHOST_LOGIT_H

#include <stdint.h>

/* ------------------------------------------------------------- the canvas -- */
struct refcanvas {
    int w, h;
    uint32_t *px;                    /* 0x00RRGGBB, row-major */
    int clip_on, clx0, cly0, clx1, cly1;
};

extern struct refcanvas ref_cv;      /* defined in tests/unit/reftest.c */

/* Ahem is content-insensitive by design: nearly every glyph is the same filled
 * em box, so "PASS" and "FAIL" draw identically. That is right for the corpus
 * and wrong as a general renderer, so the alternative is selectable and the
 * harness measures the difference rather than asserting it does not matter.
 * 0 = Ahem model (default), 1 = a codepoint-dependent notch pattern. */
extern int ref_glyph_mode;

static inline void ref_put(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= ref_cv.w || y >= ref_cv.h) return;
    if (ref_cv.clip_on && (x < ref_cv.clx0 || x >= ref_cv.clx1 ||
                           y < ref_cv.cly0 || y >= ref_cv.cly1)) return;
    ref_cv.px[(long)y * ref_cv.w + x] = c & 0xFFFFFFu;
}

static inline uint32_t ref_get(int x, int y)
{
    if (x < 0 || y < 0 || x >= ref_cv.w || y >= ref_cv.h) return 0;
    return ref_cv.px[(long)y * ref_cv.w + x] & 0xFFFFFFu;
}

/* fb_fill_rect */
static inline void ref_fill(int x, int y, int w, int h, uint32_t c)
{
    if (w <= 0 || h <= 0) return;
    if (w > 1 << 20) w = 1 << 20;
    if (h > 1 << 20) h = 1 << 20;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) ref_put(x + i, y + j, c);
}

/* ------------------------------------------------------- the five syscalls -- */
static inline void gui_rect(int x, int y, int w, int h, unsigned color)
{ ref_fill(x, y, w, h, (uint32_t)color); }

/* fb_round_rect: the corner test is boolean, exactly as the kernel's is -- the
 * antialiased path a page's border-radius takes now goes through gfx and
 * arrives here as a BLIT, not as this call. */
static inline void gui_rrect(int x, int y, int w, int h, int r, unsigned color)
{
    if (w <= 0 || h <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = 0, dy = 0;
            if (i < r) dx = r - i; else if (i >= w - r) dx = i - (w - r - 1);
            if (j < r) dy = r - j; else if (j >= h - r) dy = j - (h - r - 1);
            if (dx && dy && dx * dx + dy * dy > r * r) continue;
            ref_put(x + i, y + j, (uint32_t)color);
        }
    }
}

static inline void gui_clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) { ref_cv.clip_on = 1; ref_cv.clx0 = ref_cv.cly0 = 0;
                            ref_cv.clx1 = ref_cv.cly1 = 0; return; }
    if (x <= 0 && y <= 0 && x + w >= ref_cv.w && y + h >= ref_cv.h) {
        ref_cv.clip_on = 0; return;                 /* full-surface clip = none */
    }
    ref_cv.clip_on = 1;
    ref_cv.clx0 = x; ref_cv.cly0 = y; ref_cv.clx1 = x + w; ref_cv.cly1 = y + h;
}

/* fb_blit_rgba: nearest-neighbour scale of the source into the dest rect, with
 * per-pixel src-over. This is the primitive the painter uses for EVERY blended
 * fill (a 1x1 RGBA source stretched) and for every gfx coverage mask, so its
 * rounding is load-bearing: `(s*a + d*(255-a))/255`, truncating, same as fb.c. */
static inline void gui_blit(int x, int y, int w, int h, const unsigned char *rgba,
                            int sw, int sh)
{
    if (!rgba || w <= 0 || h <= 0 || sw <= 0 || sh <= 0) return;
    if (w > 1 << 20 || h > 1 << 20) return;
    for (long j = 0; j < h; j++) {
        int sy = (int)(j * sh / h);
        for (long i = 0; i < w; i++) {
            int sx = (int)(i * sw / w);
            const unsigned char *p = rgba + ((long)(sy * sw + sx) * 4);
            int a = p[3];
            if (!a) continue;
            int px = (int)(x + i), py = (int)(y + j);
            if (a >= 255) { ref_put(px, py, ((uint32_t)p[0] << 16) |
                                            ((uint32_t)p[1] << 8) | p[2]); continue; }
            uint32_t d = ref_get(px, py);
            int br = (int)((d >> 16) & 0xFF), bg = (int)((d >> 8) & 0xFF), bb = (int)(d & 0xFF);
            int nr = (p[0] * a + br * (255 - a)) / 255;
            int ng = (p[1] * a + bg * (255 - a)) / 255;
            int nb = (p[2] * a + bb * (255 - a)) / 255;
            ref_put(px, py, ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb);
        }
    }
}

/* ------------------------------------------------------------------ text -- */
/* THE AHEM MODEL, and why the harness ships it rather than a font.
 *
 * A large part of the WPT layout corpus sets `font: 25px/1 Ahem` precisely so
 * that font rendering cancels out of the comparison: Ahem's glyphs are exactly
 * specified filled rectangles, one em wide, from 0.8em above the baseline to
 * 0.2em below. A test that draws Ahem text and a reference that draws a plain
 * <div> of the same colour are then the same picture in every conforming
 * engine. third_party/wpt/fonts/ahem.ttf IS NOT IN THIS CHECKOUT (see the
 * report), so the choice was: render no glyph at all, or model the glyph.
 * Modelling it is the only option under which that whole class of reftest is
 * judgeable.
 *
 * The classes, from Ahem's own README:
 *   space, tab, newline           blank
 *   'p'                           the descender only  (0.8em .. 1.0em)
 *   0xC9 (E-acute)                the ascender only   (0.0em .. 0.8em)
 *   everything else               the full em box
 * gui_text_run's y is the TOP of the em box (browser_paint.c:476 says so), so
 * the full box is exactly (x, y, adv, px).
 *
 * Advance is one em per CODEPOINT, which is also what ref_text_measure returns
 * -- the two must agree or a line breaks in one place and paints in another. */
static inline int ref_utf8_next(const char *s, int len, int *i)
{
    unsigned char c = (unsigned char)s[*i];
    int cp, n;
    if (c < 0x80)        { cp = c;          n = 1; }
    else if (c < 0xE0)   { cp = c & 0x1F;   n = 2; }
    else if (c < 0xF0)   { cp = c & 0x0F;   n = 3; }
    else                 { cp = c & 0x07;   n = 4; }
    if (*i + n > len) { (*i)++; return 0xFFFD; }
    for (int k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)s[*i + k] & 0x3F);
    *i += n;
    return cp;
}

static inline void gui_text_run(int x, int y, int px, int mono, unsigned color,
                                const char *s, int len)
{
    (void)mono;
    if (px < 1 || !s || len <= 0) return;
    int i = 0;
    while (i < len) {
        int cp = ref_utf8_next(s, len, &i);
        int adv = px;
        int gy = y, gh = px;
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') { x += adv; continue; }
        if (cp == 'p') { gy = y + (px * 4) / 5; gh = px - (px * 4) / 5; }
        else if (cp == 0xC9) { gh = (px * 4) / 5; }
        ref_fill(x, gy, adv, gh, (uint32_t)color);
        if (ref_glyph_mode) {
            /* The control: punch a codepoint-dependent hole so that two runs of
             * the same length but different text are different pictures. */
            int q = px / 4; if (q < 1) q = 1;
            for (int b = 0; b < 4; b++)
                if ((cp >> b) & 1)
                    ref_fill(x + (b & 1) * q * 2 + q / 2, gy + (b >> 1) * q + q / 4,
                             q, q > gh ? gh : q, 0xFFFFFFu ^ (uint32_t)color);
        }
        x += adv;
    }
}

/* The metric half of the same model. reftest.c defines text_measure (which
 * layout.c calls) on top of this, so the two cannot drift. */
static inline int ref_text_measure(const char *s, int len, int px)
{
    int i = 0, n = 0;
    while (i < len) { ref_utf8_next(s, len, &i); n++; }
    return n * px;
}

static inline int ui_scale(void) { return 100; }

struct logit_run  { int x, y, px, mono; unsigned color; const char *s; int len; };
struct logit_blit { int x, y, w, h; const unsigned char *rgba; int sw, sh; };

#endif /* REFHOST_LOGIT_H */
