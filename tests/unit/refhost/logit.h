/* Host stand-in for c/apps/logit.h that RASTERIZES, for the WPT reftest harness.
 *
 * WHY A SECOND SHIM (tests/unit/painthost already exists). painthost turns the
 * five GUI syscalls into a list of draw ops, which is what an assertion about
 * "did the painter choose alpha 128" wants. A reftest asks a different question
 * -- are these two documents the SAME PICTURE -- and that only has an answer
 * once the ops have become pixels. So this header is the same include-shadowing
 * trick with a different sink: `#include "logit.h"` resolves here (via
 * -Itests/unit/refhost, which MUST come first on the include path) and the real
 * c/apps/browser/browser_paint.c is linked and run unmodified.
 *
 * ============================================================================
 * WHAT IS SHARED WITH browser.aex AND WHAT IS NOT.  Read this before believing
 * any number this harness prints.  It is the credibility of every one of them.
 * ============================================================================
 *
 * The first draft of this file (commit 2b96998) MODELLED the bottom of the
 * stack: it transcribed fb.c's four fill routines and substituted an analytic
 * "Ahem model" for text, on the premise that neither the kernel framebuffer nor
 * a font could be reached from a host process.  Both halves of that premise
 * turned out to be false, and this file is the rewrite:
 *
 *   - c/kernel/gui/fb.c COMPILES AND LINKS ON THE HOST unmodified.  Its only
 *     undefined symbols are kmalloc/kfree, text_*, vmm_map_range and the
 *     virtio_gpu_* probe -- and drawing into a `struct surface` via fb_target()
 *     never touches the last two.  So the harness links the REAL painter
 *     primitives instead of a transcription of them.
 *
 *   - c/kernel/gui/text.c AND c/lib/text/* LINK TOO, against a vfs_read() that
 *     reads host files.  So glyph rasterization, shaping, bidi, the glyph cache
 *     and -- the part layout actually depends on -- text_measure() are the real
 *     code, not a stub.  Every other host test in this tree substitutes
 *     `len * (px/2)` here (tests/unit/paint_test.c:30); this one does not, and
 *     it must not, because a reftest is a claim about where text ENDED UP.
 *
 * SHARED (the real files, linked, byte for byte the same code browser.aex runs):
 *     the pipeline   dom.c html_tokenizer.c html_tree.c dom_serialize.c
 *                    css_engine.c css_vars.c css_extra.c layout.c
 *                    browser_paint.c, LibCSS, c/lib/gfx
 *     the raster     c/kernel/gui/fb.c      (fill/round-rect/blit/clip/glyph)
 *                    c/kernel/gui/raster.c  (the AA coverage rasterizer)
 *                    c/lib/text/*           (ttf, cff, shape, script, bidi)
 *                    c/kernel/gui/text.c    (text_measure + text_draw_run)
 *     the ABI        include/abi/logit_pack.h -- the SAME generated unpack
 *                    macros c/kernel/gui/wm.c uses, so the 16-bit coordinate
 *                    truncation below is not an imitation of the syscall
 *                    convention, it IS the syscall convention.
 *
 * NOT SHARED (and this is now the whole list):
 *   1. c/kernel/gui/wm.c's syscall CASES -- the ~6 lines per call that unpack,
 *      scale and clamp before calling fb_*.  Each gui_* below is a transcription
 *      of one of them; the case it mirrors is named above it.  A divergence in
 *      wm.c would not be caught here.  They are short and they are quoted.
 *   2. THE WINDOW.  On the machine the browser paints into a window surface
 *      inside a desktop, and browser.c reserves a chrome strip (address bar,
 *      status bar) that this harness does not draw.  reftest.c paints at
 *      scroll 0 into a bare viewport, so document coordinates ARE device
 *      coordinates.  Both sides of a comparison get the same treatment.
 *   3. THE FONT FILE, and this is the one real substitution.  On the machine
 *      /fonts/ui.ttf is a Noto Sans SC subset; the WPT corpus is written
 *      against Ahem, and cstyle carries no font-family name at all (LibCSS's
 *      family list is dropped in css_engine.c -- only the monospace BIT
 *      survives), so the engine physically cannot select a family.  The
 *      harness therefore MAPS the font files: --ahem loads Ahem.ttf as the UI
 *      font.  The rasterizer, the shaper and the metrics are unchanged real
 *      code; only which file they are pointed at differs.  reftest.c reports
 *      the mode on every run, and `make test-reftest-realfont` measures the
 *      same corpus through the shipping fonts so the size of the substitution
 *      is a number rather than a footnote.
 */
#ifndef REFHOST_LOGIT_H
#define REFHOST_LOGIT_H

#include <stdint.h>
#include "logit_pack.h"          /* the kernel's own generated unpack macros */

/* The real primitives, forward-declared rather than via fb.h/text.h so that
 * this header can be included by browser_paint.c without dragging the kernel's
 * include tree into the app's translation unit. Signatures are copied from
 * c/kernel/gui/fb.h and c/kernel/gui/text.h; a mismatch is a link error, not a
 * silent divergence. */
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_round_rect(int x, int y, int w, int h, int radius, uint32_t color);
void fb_blit_rgba(int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh);
void fb_set_clip(int x, int y, int w, int h);
void fb_clear_clip(void);
int  text_draw_run(int x, int y, const char *s, int len, int px, int mono, uint32_t color);

/* The surface reftest.c is painting into. Declared here (not fb.h's full
 * struct) so the shim can clamp to it exactly as wm.c clamps to w->surf. */
extern int refhost_surf_w, refhost_surf_h;

/* ------------------------------------------------------------------------ *
 * The five calls. Each is `pack exactly as c/apps/logit.h packs` followed by
 * `unpack and clamp exactly as the wm.c case unpacks and clamps`. The round
 * trip is deliberate and it is not ceremony: LOGIT_GUI_RECT_A_X masks to 16
 * UNSIGNED bits, so a painter that hands the kernel a negative coordinate --
 * which the browser does, for anything scrolled above the viewport -- gets it
 * back as ~65500 and draws nothing. Reproducing that is the difference between
 * measuring the browser and measuring an idealisation of it.
 *
 * S() is the UI scale; on the host ui_scale() is 100, so S(v) == v and the
 * scaling terms below collapse. They are written out anyway, in the same form
 * wm.c writes them (the DIFFERENCE of two converted edges, never the converted
 * width), so that turning the harness up to a 1.5x display later is a one-line
 * change and not a re-derivation.
 * ------------------------------------------------------------------------ */
#define REFHOST_S(v) (v)                 /* ui_scale() == 100 */

/* --- wm.c case SYS_GUI_RECT --- */
static inline void gui_rect(int x, int y, int w, int h, unsigned color)
{
    long a = ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF);
    long b = ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF);
    int rx = REFHOST_S(LOGIT_GUI_RECT_A_X(a)), ry = REFHOST_S(LOGIT_GUI_RECT_A_Y(a));
    int rw = REFHOST_S(LOGIT_GUI_RECT_A_X(a) + LOGIT_GUI_RECT_B_W(b)) - rx;
    int rh = REFHOST_S(LOGIT_GUI_RECT_A_Y(a) + LOGIT_GUI_RECT_B_H(b)) - ry;
    if (rw > refhost_surf_w - rx) rw = refhost_surf_w - rx;
    if (rh > refhost_surf_h - ry) rh = refhost_surf_h - ry;
    fb_fill_rect(rx, ry, rw, rh, (uint32_t)color);
}

/* --- wm.c case SYS_GUI_RRECT --- */
static inline void gui_rrect(int x, int y, int w, int h, int radius, unsigned color)
{
    long a = ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF);
    long b = ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF);
    long c = ((long)(radius & 0xFF) << 24) | (color & 0xFFFFFF);
    int rx = REFHOST_S(LOGIT_GUI_RRECT_A_X(a)), ry = REFHOST_S(LOGIT_GUI_RRECT_A_Y(a));
    int rw = REFHOST_S(LOGIT_GUI_RRECT_A_X(a) + LOGIT_GUI_RRECT_B_W(b)) - rx;
    int rh = REFHOST_S(LOGIT_GUI_RRECT_A_Y(a) + LOGIT_GUI_RRECT_B_H(b)) - ry;
    int rr = REFHOST_S(LOGIT_GUI_RRECT_C_RADIUS(c));
    if (rw > refhost_surf_w - rx) rw = refhost_surf_w - rx;
    if (rh > refhost_surf_h - ry) rh = refhost_surf_h - ry;
    fb_round_rect(rx, ry, rw, rh, rr, (uint32_t)LOGIT_GUI_RRECT_C_COLOR(c));
}

/* --- wm.c case SYS_GUI_CLIP ---
 * The kernel treats a clip covering the whole surface as "no clip" (that is
 * what fb_clear_clip is for); a zero/negative size clips everything away. */
static inline void gui_clip(int x, int y, int w, int h)
{
    long a = ((long)(x & 0xFFFF) << 16) | (y & 0xFFFF);
    long b = ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF);
    int cx = REFHOST_S(LOGIT_GUI_CLIP_A_X(a)), cy = REFHOST_S(LOGIT_GUI_CLIP_A_Y(a));
    int cw = LOGIT_GUI_CLIP_B_W(b), ch = LOGIT_GUI_CLIP_B_H(b);
    if (cw <= 0 || ch <= 0) { fb_set_clip(0, 0, 0, 0); return; }
    if (cx <= 0 && cy <= 0 && cx + REFHOST_S(cw) >= refhost_surf_w &&
        cy + REFHOST_S(ch) >= refhost_surf_h) { fb_clear_clip(); return; }
    fb_set_clip(cx, cy, REFHOST_S(cx + cw) - cx, REFHOST_S(cy + ch) - cy);
}

/* --- wm.c case SYS_GUI_TEXT_RUN ---
 * USER_TEXT_MAX is the kernel's bounce-buffer size; a longer run is TRUNCATED
 * on the machine, so it is truncated here. */
#define REFHOST_USER_TEXT_MAX 4096
static inline void gui_text_run(int x, int y, int px, int mono, unsigned color,
                                const char *s, int len)
{
    if (px < 1 || px > 512) return;
    if (len < 0) len = 0;
    if (len > REFHOST_USER_TEXT_MAX - 1) len = REFHOST_USER_TEXT_MAX - 1;
    text_draw_run(REFHOST_S(x), REFHOST_S(y), s, len, REFHOST_S(px), mono, color);
}

/* --- wm.c case SYS_GUI_BLIT --- */
static inline void gui_blit(int x, int y, int w, int h, const unsigned char *rgba,
                            int sw, int sh)
{
    if (!rgba || sw <= 0 || sh <= 0 || sw > 4096 || sh > 4096) return;
    if (w <= 0 || h <= 0) return;
    int bx = REFHOST_S(x), by = REFHOST_S(y);
    fb_blit_rgba(bx, by, REFHOST_S(x + w) - bx, REFHOST_S(y + h) - by, rgba, sw, sh);
}

/* The painter asks the compositor for the display's backing scale. 100 makes
 * points and device pixels the same thing, which is what a reftest compares. */
static inline int ui_scale(void) { return 100; }

struct logit_run  { int x, y, px, mono; unsigned color; const char *s; int len; };
struct logit_blit { int x, y, w, h; const unsigned char *rgba; int sw, sh; };

#endif /* REFHOST_LOGIT_H */
