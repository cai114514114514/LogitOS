#include "aui.h"
#include "gfx.h"

/* ============================================================================
 * aui -- immediate-mode widgets over the gui_* syscalls.
 *
 * Layout of this file:
 *   1. small utilities (no libc: these apps link nothing but crt0 + gfx)
 *   2. theme tokens
 *   3. scale, and the bridge from Open Logit's masks to SYS_GUI_BLIT
 *   4. drawing primitives built on it
 *   5. frame state: input, focus, clipping, translation, deferred popups
 *   6. layout stacks
 *   7. widgets
 *
 * THE ONE STRUCTURAL IDEA. The kernel's shape calls are hard-edged; its ONE
 * per-pixel-alpha entry point is SYS_GUI_BLIT (fb_blit_rgba). So every smooth
 * thing in here -- rounded corners, rings, shadows, gradients, translucency --
 * is produced by rasterizing a small coverage mask in ring 3 and handing it to
 * that blit.
 *
 * WHERE THE RASTERIZER WENT. It used to be in this file. It is now
 * c/lib/gfx -- Open Logit, the engine -- and this file is one of its clients,
 * along with the browser's painter. There were three coverage/paint paths in
 * this tree and a toolkit that could not be reused outside a window; the point
 * of moving it was that there is now ONE, and that an app which needs a shape
 * aui does not have can build a path and fill it instead of starting a fourth.
 * The three properties the toolkit was built on are unchanged, because they are
 * why it is cheap, and the engine inherited all three:
 *
 *   - Masks are rasterized in DEVICE pixels and blitted into a POINT rect whose
 *     device size is the same number, so the kernel's nearest-neighbour rescale
 *     is the identity. Generate at 1x and let it stretch and you get a blurry
 *     mask of a sharp shape, which looks worse than the staircase it replaced.
 *   - Only the parts that actually curve are rasterized. A rounded rect is a
 *     9-slice: three opaque gui_rect calls plus four r x r corner tiles. A
 *     shadow is 8 slices around a hole. Cost is O(r^2) and O(perimeter*blur),
 *     never O(area).
 *   - Masks are cached by their exact device geometry, so a screen full of
 *     controls that share a radius rasterizes one corner and reuses it.
 *
 * NO LIBC, still. Nothing here or in gfx may call memset/memcpy -- clock.aex
 * links crt0 + this file + gfx and nothing else. Bulk clears go through
 * gfx_zero(), which writes through a volatile pointer specifically so -O2's
 * loop-idiom pass cannot rewrite it into a memset call that would then fail to
 * link.
 * ========================================================================== */

/* ------------------------------------------------- 0. cost instrumentation --
 * -DAUI_COST (make bench-gfx-frame) puts a CLOCK_MONOTONIC bracket around every
 * drawing syscall and sorts the time into three buckets, because the honest
 * question about a rendering engine is not "what does a frame cost" but "what
 * of the frame is the engine". The toolkit line already measured 24-27 ms for a
 * full-window repaint and found the dominant cost was aui_begin()'s
 * unconditional gui_clear plus text -- NOT the rasterized primitives. A number
 * that does not separate those credits the engine with a cost it does not pay
 * and hides one it does.
 *
 * The instrumentation itself costs a syscall per drawing call, so the buckets
 * are to be read as a RATIO and the uninstrumented total comes from bench-aui.
 * A build without AUI_COST compiles to exactly what it did before: the macros
 * are absent, not empty. */
#ifdef AUI_COST
static unsigned long long ck_clear, ck_text, ck_shape, ck_other, ck_frames;
static unsigned long long ck_t0, ck_fstart, ck_wall;
static int ck_miss0;
static unsigned ck_last;
static void t0_(void) { ck_t0 = monotonic_ns(); }
static void t1_(unsigned long long *b) { *b += monotonic_ns() - ck_t0; }
/* The parenthesised callee suppresses the macro, so each of these wraps the
 * real inline syscall rather than recursing. */
static int tm_(const char *s, int n, int px, int mono)
{ t0_(); int r = (text_measure_px)(s, n, px, mono); t1_(&ck_text); return r; }
#define gui_clear(a)                 (t0_(), (gui_clear)(a), t1_(&ck_clear))
#define gui_rect(a,b,c,d,e)          (t0_(), (gui_rect)(a,b,c,d,e), t1_(&ck_shape))
#define gui_rrect(a,b,c,d,e,f)       (t0_(), (gui_rrect)(a,b,c,d,e,f), t1_(&ck_shape))
#define gui_blit(a,b,c,d,e,f,g)      (t0_(), (gui_blit)(a,b,c,d,e,f,g), t1_(&ck_shape))
#define gui_glass(a,b,c,d,e,f,g,h,i) (t0_(), (gui_glass)(a,b,c,d,e,f,g,h,i), t1_(&ck_shape))
#define gui_icon(a,b,c,d,e)          (t0_(), (gui_icon)(a,b,c,d,e), t1_(&ck_shape))
#define gui_text_run(a,b,c,d,e,f,g)  (t0_(), (gui_text_run)(a,b,c,d,e,f,g), t1_(&ck_text))
#define gui_clip(a,b,c,d)            (t0_(), (gui_clip)(a,b,c,d), t1_(&ck_other))
#define gui_flush()                  (t0_(), (gui_flush)(), t1_(&ck_other))
#define text_measure_px(a,b,c,d)     tm_(a,b,c,d)
#endif

/* ------------------------------------------------------------ 1. utilities */

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---------------------------------------------------------------- 2. theme */

static int theme_dark, theme_inited;
struct aui_theme aui_t;

/* The channel lerp lives in the engine (gfx_mix): a gradient strip built by
 * gfx and a two-tone token blended here have to land on the same byte, or a
 * card's fill and the top of its own gradient differ by one and show a seam. */
unsigned aui_mix(unsigned a, unsigned b, int t) { return gfx_mix(a, b, t); }

unsigned aui_shade(unsigned c, int d)
{
    int r = iclamp((int)((c >> 16) & 255) + d, 0, 255);
    int g = iclamp((int)((c >> 8) & 255) + d, 0, 255);
    int b = iclamp((int)(c & 255) + d, 0, 255);
    return rgb(r, g, b);
}

static void load_light(void)
{
    aui_t.bg          = rgb(244, 245, 248);
    aui_t.surface     = rgb(255, 255, 255);
    aui_t.face        = rgb(232, 234, 240);
    aui_t.text        = rgb(40, 42, 50);
    aui_t.muted       = rgb(140, 144, 154);
    aui_t.border      = rgb(206, 208, 216);
    aui_t.hi          = rgb(255, 255, 255);
    aui_t.accent      = rgb(64, 130, 246);
    aui_t.accent_text = rgb(255, 255, 255);
    aui_t.success     = rgb(52, 199, 89);
    aui_t.warning     = rgb(255, 179, 64);
    aui_t.error       = rgb(255, 69, 58);
    aui_t.focus       = rgb(64, 130, 246);
    aui_t.surface_2   = rgb(255, 255, 255);
    aui_t.face_hover  = rgb(222, 225, 233);
    aui_t.face_active = rgb(206, 210, 220);
    aui_t.track       = rgb(219, 222, 230);
    aui_t.thumb       = rgb(255, 255, 255);
    aui_t.disabled    = rgb(236, 237, 241);
    aui_t.disabled_tx = rgb(178, 181, 190);
    aui_t.scrim       = rgb(18, 20, 28);
    aui_t.shadow      = rgb(28, 32, 48);
    aui_t.selection   = rgb(180, 208, 255);
}

static void load_dark(void)
{
    aui_t.bg          = rgb(28, 28, 32);
    aui_t.surface     = rgb(44, 44, 52);
    aui_t.face        = rgb(58, 58, 68);
    aui_t.text        = rgb(236, 237, 242);
    aui_t.muted       = rgb(146, 148, 160);
    aui_t.border      = rgb(72, 74, 86);
    aui_t.hi          = rgb(84, 86, 98);
    aui_t.accent      = rgb(94, 150, 255);
    aui_t.accent_text = rgb(255, 255, 255);
    aui_t.success     = rgb(48, 209, 88);
    aui_t.warning     = rgb(255, 190, 84);
    aui_t.error       = rgb(255, 92, 82);
    aui_t.focus       = rgb(94, 150, 255);
    aui_t.surface_2   = rgb(56, 57, 66);
    aui_t.face_hover  = rgb(72, 73, 85);
    aui_t.face_active = rgb(88, 90, 104);
    aui_t.track       = rgb(70, 72, 84);
    aui_t.thumb       = rgb(226, 228, 236);
    aui_t.disabled    = rgb(48, 49, 57);
    aui_t.disabled_tx = rgb(104, 106, 118);
    aui_t.scrim       = rgb(0, 0, 0);
    aui_t.shadow      = rgb(0, 0, 0);
    aui_t.selection   = rgb(46, 86, 152);
}

/* First use: adopt the current SYSTEM theme (SYS_UI_DARK) so even the first token
 * read / window clear matches a desktop that is already dark. */
void aui_ensure(void)
{
    if (!theme_inited) {
        theme_dark = sys_ui_dark(-1) > 0;
        theme_dark ? load_dark() : load_light();
        theme_inited = 1;
    }
}

void aui_set_dark(int on)
{
    theme_dark = on; theme_inited = 1;
    if (on) load_dark(); else load_light();
}
int aui_is_dark(void) { return theme_dark; }

/* HSL -> packed rgb, integer-only. h:0..359, s,l:0..100. Channels are carried in
 * "percent" (0..100) through the standard piecewise formula, then scaled to 8-bit:
 *   C = (1-|2L-1|)*S,  X = C*(triangular wave over the sextant),  m = L - C/2. */
unsigned aui_hsl(int h, int s, int l)
{
    h = iclamp(h, 0, 359); s = iclamp(s, 0, 100); l = iclamp(l, 0, 100);
    int a = (l >= 50) ? (2 * l - 100) : (100 - 2 * l);   /* |2L-1| in % */
    int C = (100 - a) * s / 100;                          /* chroma, 0..100 */
    int seg = h / 60, frac = h % 60;                      /* sextant + position */
    int X = (seg & 1) ? C * (60 - frac) / 60 : C * frac / 60;
    int r1 = 0, g1 = 0, b1 = 0;
    switch (seg) {
        case 0: r1 = C; g1 = X; break;
        case 1: r1 = X; g1 = C; break;
        case 2: g1 = C; b1 = X; break;
        case 3: g1 = X; b1 = C; break;
        case 4: r1 = X; b1 = C; break;
        default: r1 = C; b1 = X; break;
    }
    int m = l - C / 2;                                    /* lightness offset, % */
    return rgb(iclamp((r1 + m) * 255 / 100, 0, 255),
               iclamp((g1 + m) * 255 / 100, 0, 255),
               iclamp((b1 + m) * 255 / 100, 0, 255));
}

void aui_set_accent(unsigned color) { aui_ensure(); aui_t.accent = color; aui_t.focus = color; }

/* ------------------------------------------------- 3. scale + rasterizer */

static int sc_pct;                    /* cached backing scale, percent */
int aui_scale(void) { if (!sc_pct) { sc_pct = ui_scale(); if (sc_pct < 100) sc_pct = 100; } return sc_pct; }
int aui_dev(int p)
{
    int s = aui_scale();
    return p >= 0 ? p * s / 100 : -(((-p) * s + 99) / 100);
}
/* The DEVICE extent of a logical span, computed as the difference of two
 * converted edges. Converting the length on its own instead is the classic
 * scaled-UI seam bug: two abutting 5-point columns become 7 and 7 device pixels
 * at 150% instead of 7 and 8, and the missing column shows as a moving hairline. */
static int devlen(int a, int len) { return aui_dev(a + len) - aui_dev(a); }

/* ---- the engine's masks, and how they reach the screen ----
 *
 * gfx_mask_corner() rasterizes and caches a corner tile keyed by its exact
 * DEVICE geometry; the names below are the toolkit's local vocabulary for the
 * engine's kinds. tests/unit/aui_mask_test.c reaches these directly -- it has
 * its own, independently written 16x supersampled reference, so keeping it
 * pointed at the toolkit's entry points means the engine is now checked against
 * TWO references that share no code. */
#define MASK_MAX             GFX_MASK_MAX
#define MK_FILL              GFX_MASK_FILL
#define MK_STROKE            GFX_MASK_RING
#define MK_SHADOW            GFX_MASK_SHADOW
#define raster_fill_corner   gfx_corner_fill
#define raster_stroke_corner gfx_corner_ring
#define raster_shadow_corner gfx_corner_shadow
#define mask_get             gfx_mask_corner

/* ---- getting a mask onto the screen ---- */
static unsigned char rgba_buf[MASK_MAX * MASK_MAX * 4];
#define GRAD_MAX 1024
static unsigned char grad_buf[GRAD_MAX * 4];

/* Blit a coverage tile into the point rect (x,y,w,h), optionally mirrored. The
 * source is generated at the rect's exact device size, so the kernel's rescale
 * is a no-op and the anti-aliasing survives at any backing scale. One
 * rasterized quadrant serves all four corners, which is what the mirroring is
 * for. */
static void blit_mask(int x, int y, int w, int h, const unsigned char *cov,
                      int cw, int ch, unsigned color, int alpha, int fx, int fy)
{
    if (!cov || cw <= 0 || ch <= 0 || w <= 0 || h <= 0) return;
    if ((long)cw * ch * 4 > (long)sizeof rgba_buf) return;
    gfx_mask_to_rgba(rgba_buf, cov, cw, ch, color, alpha, fx, fy);
    gui_blit(x, y, w, h, rgba_buf, cw, ch);
}

/* --------------------------------------------- 5a. frame state (globals) */

static int win_w = 640, win_h = 480;
static int id_ctr;
static int focus_id, focus_vis;
static int ox_, oy_;                          /* current translation, points */
static unsigned frame_ms;

/* clip stack (the kernel has one clip rect per surface, so aui keeps the stack
 * and pushes the intersection) */
static struct aui_rect clipst[8];
static int clipn;

static void clip_apply(void)
{
    if (!clipn) { gui_clip(0, 0, 0, 0); return; }
    struct aui_rect r = clipst[clipn - 1];
    if (r.w <= 0 || r.h <= 0) { gui_clip(0, 0, 1, 1); return; }
    gui_clip(r.x, r.y, r.w, r.h);
}
static void clip_push(struct aui_rect r)
{
    if (clipn > 0) {
        struct aui_rect p = clipst[clipn - 1];
        int x0 = imax(r.x, p.x), y0 = imax(r.y, p.y);
        int x1 = imin(r.x + r.w, p.x + p.w), y1 = imin(r.y + r.h, p.y + p.h);
        r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    }
    if (clipn < 8) clipst[clipn++] = r;
    clip_apply();
}
static void clip_pop(void) { if (clipn) clipn--; clip_apply(); }
static int clip_has(int x, int y)
{
    if (!clipn) return 1;
    struct aui_rect r = clipst[clipn - 1];
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* input */
static struct {
    int ev, a, b, mods, button, wheel;
    int mx, my;               /* pointer, window-local points */
    int down;                 /* left button held */
    int active;               /* widget id owning the press */
    int hot;                  /* widget under the pointer */
    unsigned hot_t0;
    int repaint;
    int key_used;             /* the frame's key was consumed by the toolkit */
} in;

/* What motion can change, without re-running the frame to find out.
 *
 * `hot_rect` is the hovered widget's box and `wbb` the union of every box polled
 * last frame. Motion inside the hovered widget changes nothing; motion outside
 * `wbb` cannot enter one. Everything else has to repaint. Both are last frame's
 * geometry, which is the only geometry that exists when the event arrives -- and
 * it is right, because an immediate-mode frame is a pure function of state that
 * a mouse move does not alter. */
static struct aui_rect hot_rect, wbb, wbb_next;
static int wbb_any;

/* modal / popup gating: both are decided from the PREVIOUS frame's geometry,
 * because a click has to be refused by things drawn before the popup exists. */
static struct aui_rect pop_prev;
static int modal_prev, in_dialog, in_popup;
static int dlg_open_now;

static struct {
    int kind;                 /* 0 none, 1 list popup */
    int owner, x, y, w, itemh, n, hi;
    const char *const *items;
    int *sel;
} pop;
static int pop_changed_id;

static const char *tip_text;
static int tip_x, tip_y;

/* focus order = call order; the list is rebuilt every frame and read by
 * aui_feed() one frame later, which is exactly when Tab needs it. */
static int foc_ids[128], foc_n;

/* A radio GROUP's value range, learned by watching the calls.
 *
 * Arrows have to move the selection to the neighbouring button, and aui is never
 * told what the group's legal values are -- so an unguarded `*group = value + d`
 * walks off the end and leaves the group with NOTHING selected, which looks
 * exactly like the widget breaking. The buttons of a group are drawn
 * consecutively, so the range can simply be observed: accumulate while the frame
 * runs, commit at aui_end(), clamp against the committed range. */
static int *rg_ptr, rg_lo, rg_hi, rg_lo_a, rg_hi_a;

/* ----------------------------------------------- 4. drawing primitives */

#define X_(v) ((v) + ox_)
#define Y_(v) ((v) + oy_)

void aui_fill(int x, int y, int w, int h, unsigned color)
{
    if (w <= 0 || h <= 0) return;
    gui_rect(X_(x), Y_(y), w, h, color);
}

void aui_fill_a(int x, int y, int w, int h, unsigned color, int alpha)
{
    if (w <= 0 || h <= 0) return;
    if (alpha >= 255) { aui_fill(x, y, w, h, color); return; }
    if (alpha <= 0) return;
    /* One RGBA pixel stretched over the rect: the kernel's blit does the blend,
     * so an alpha fill costs exactly what an opaque fill costs. There is no
     * alpha-rectangle syscall, and this is why none is needed. */
    rgba_buf[0] = (unsigned char)((color >> 16) & 255);
    rgba_buf[1] = (unsigned char)((color >> 8) & 255);
    rgba_buf[2] = (unsigned char)(color & 255);
    rgba_buf[3] = (unsigned char)alpha;
    gui_blit(X_(x), Y_(y), w, h, rgba_buf, 1, 1);
}

void aui_hairline(int x, int y, int w) { aui_fill(x, y, w, 1, AUI_BORDER); }
void aui_vhairline(int x, int y, int h) { aui_fill(x, y, 1, h, AUI_BORDER); }
void aui_separator(int x, int y, int w) { aui_hairline(x, y, w); }

static int clamp_radius(int w, int h, int r)
{
    int m = imin(w, h) / 2;
    if (r > m) r = m;
    return r < 0 ? 0 : r;
}

static void round_impl(int x, int y, int w, int h, int r, unsigned c, int alpha)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);
    if (r == 0) { aui_fill_a(x, y, w, h, c, alpha); return; }
#ifdef AUI_NO_AA
    /* NEGATIVE CONTROL (make test-aui-negctl). Route every rounded shape back
     * through the kernel's SYS_GUI_RRECT, whose corner test is the boolean
     * dx*dx + dy*dy <= r*r -- i.e. the hard-edged staircase this file exists to
     * replace. The gallery still builds, still runs and still looks broadly
     * right, and tests/qmp/qmp_gallery.py's anti-aliasing assertions fail. That
     * is the demonstration that they are measuring the rasterizer and not the
     * theme. */
    (void)alpha;
    gui_rrect(X_(x), Y_(y), w, h, r, c);
    return;
#else
    int cw = devlen(X_(x), r), ch = devlen(Y_(y), r);
    const unsigned char *m = mask_get(MK_FILL, cw, ch, 0);
    if (!m) { aui_fill_a(x, y, w, h, c, alpha); return; }   /* radius past the cache: still correct, just square */
    /* interior: three opaque bands, the kernel's fast path */
    aui_fill_a(x + r, y,         w - 2 * r, r,         c, alpha);
    aui_fill_a(x,     y + r,     w,         h - 2 * r, c, alpha);
    aui_fill_a(x + r, y + h - r, w - 2 * r, r,         c, alpha);
    /* four corners, one rasterized quadrant mirrored into place */
    blit_mask(X_(x),         Y_(y),         r, r, m, cw, ch, c, alpha, 0, 0);
    blit_mask(X_(x + w - r), Y_(y),         r, r, m, cw, ch, c, alpha, 1, 0);
    blit_mask(X_(x),         Y_(y + h - r), r, r, m, cw, ch, c, alpha, 0, 1);
    blit_mask(X_(x + w - r), Y_(y + h - r), r, r, m, cw, ch, c, alpha, 1, 1);
#endif
}

void aui_round(int x, int y, int w, int h, int r, unsigned c) { round_impl(x, y, w, h, r, c, 255); }
void aui_round_a(int x, int y, int w, int h, int r, unsigned c, int a) { round_impl(x, y, w, h, r, c, a); }

void aui_stroke(int x, int y, int w, int h, int r, int t, unsigned c)
{
    if (w <= 0 || h <= 0 || t <= 0) return;
    r = clamp_radius(w, h, r);
    if (r == 0) {
        aui_fill(x, y, w, t, c); aui_fill(x, y + h - t, w, t, c);
        aui_fill(x, y + t, t, h - 2 * t, c); aui_fill(x + w - t, y + t, t, h - 2 * t, c);
        return;
    }
#ifdef AUI_NO_AA
    gui_rrect(X_(x), Y_(y), w, h, r, c);                       /* see round_impl */
    gui_rrect(X_(x + t), Y_(y + t), w - 2 * t, h - 2 * t, r - t, AUI_BG);
    return;
#else
    int cw = devlen(X_(x), r), ch = devlen(Y_(y), r), td = imax(1, aui_dev(t));
    const unsigned char *m = mask_get(MK_STROKE, cw, ch, td);
    aui_fill(x + r, y,         w - 2 * r, t, c);
    aui_fill(x + r, y + h - t, w - 2 * r, t, c);
    aui_fill(x,         y + r, t, h - 2 * r, c);
    aui_fill(x + w - t, y + r, t, h - 2 * r, c);
    blit_mask(X_(x),         Y_(y),         r, r, m, cw, ch, c, 255, 0, 0);
    blit_mask(X_(x + w - r), Y_(y),         r, r, m, cw, ch, c, 255, 1, 0);
    blit_mask(X_(x),         Y_(y + h - r), r, r, m, cw, ch, c, 255, 0, 1);
    blit_mask(X_(x + w - r), Y_(y + h - r), r, r, m, cw, ch, c, 255, 1, 1);
#endif
}

void aui_circle(int cx, int cy, int r, unsigned c) { aui_round(cx - r, cy - r, 2 * r, 2 * r, r, c); }
void aui_ring(int cx, int cy, int r, int t, unsigned c) { aui_stroke(cx - r, cy - r, 2 * r, 2 * r, r, t, c); }

void aui_vgrad(int x, int y, int w, int h, unsigned top, unsigned bot)
{
    if (w <= 0 || h <= 0) return;
    int n = devlen(Y_(y), h);
    if (n <= 0) return;
    if (n > GRAD_MAX) n = GRAD_MAX;       /* subsample; the kernel rescales it back */
    gfx_gradient_strip(grad_buf, n, top, bot, 255);
    gui_blit(X_(x), Y_(y), w, h, grad_buf, 1, n);   /* sw = 1: replicated across x */
}

void aui_hgrad(int x, int y, int w, int h, unsigned l, unsigned r)
{
    if (w <= 0 || h <= 0) return;
    int n = devlen(X_(x), w);
    if (n <= 0) return;
    if (n > GRAD_MAX) n = GRAD_MAX;
    gfx_gradient_strip(grad_buf, n, l, r, 255);
    gui_blit(X_(x), Y_(y), w, h, grad_buf, n, 1);
}

void aui_vgrad_round(int x, int y, int w, int h, int r, unsigned top, unsigned bot)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);
    if (r == 0) { aui_vgrad(x, y, w, h, top, bot); return; }
    aui_vgrad(x + r, y,         w - 2 * r, r,         top, aui_mix(top, bot, r * 255 / h));
    aui_vgrad(x,     y + r,     w,         h - 2 * r, aui_mix(top, bot, r * 255 / h),
                                                      aui_mix(top, bot, (h - r) * 255 / h));
    aui_vgrad(x + r, y + h - r, w - 2 * r, r,         aui_mix(top, bot, (h - r) * 255 / h), bot);
    /* The corners take the gradient's colour at their own row, so the curve does
     * not shear away from the band it abuts. */
    int cw = devlen(X_(x), r), ch = devlen(Y_(y), r);
    const unsigned char *m = mask_get(MK_FILL, cw, ch, 0);
    if (!m) return;
    int hd = imax(1, devlen(Y_(y), h));
    for (int corner = 0; corner < 4; corner++) {
        int fx = corner & 1, fy = corner >> 1;
        int px = fx ? x + w - r : x, py = fy ? y + h - r : y;
        for (int j = 0; j < ch; j++) {
            int sj = fy ? ch - 1 - j : j;
            int grow = fy ? hd - ch + j : j;
            unsigned c = aui_mix(top, bot, iclamp(grow * 255 / hd, 0, 255));
            unsigned char *d = rgba_buf + (long)j * cw * 4;
            const unsigned char *s = m + (long)sj * cw;
            for (int i = 0; i < cw; i++) {
                d[i * 4 + 0] = (unsigned char)((c >> 16) & 255);
                d[i * 4 + 1] = (unsigned char)((c >> 8) & 255);
                d[i * 4 + 2] = (unsigned char)(c & 255);
                d[i * 4 + 3] = s[fx ? cw - 1 - i : i];
            }
        }
        gui_blit(X_(px), Y_(py), r, r, rgba_buf, cw, ch);
    }
}

/* An edge strip: one device pixel across the constant axis, `n` along the
 * falloff, stretched by the kernel. This is what makes a shadow's cost depend on
 * the blur radius and not on the size of the thing casting it. */
static void shadow_edge(int x, int y, int w, int h, int n, int vertical,
                        int reverse, unsigned color, int alpha)
{
    if (w <= 0 || h <= 0 || n <= 0 || n > GRAD_MAX) return;
    long blur = (long)n * 256;
    for (int k = 0; k < n; k++) {
        int kk = reverse ? k : n - 1 - k;            /* distance from the box edge */
        long d = (long)kk * 256 + 128;
        int a = gfx_shadow_falloff(d, blur) * alpha / 255;
        grad_buf[k * 4 + 0] = (unsigned char)((color >> 16) & 255);
        grad_buf[k * 4 + 1] = (unsigned char)((color >> 8) & 255);
        grad_buf[k * 4 + 2] = (unsigned char)(color & 255);
        grad_buf[k * 4 + 3] = (unsigned char)a;
    }
    if (vertical) gui_blit(x, y, w, h, grad_buf, 1, n);
    else          gui_blit(x, y, w, h, grad_buf, n, 1);
}

void aui_shadow_ex(int x, int y, int w, int h, int r, int dy, int blur, int alpha)
{
    if (w <= 0 || h <= 0 || blur <= 0 || alpha <= 0) return;
#ifdef AUI_NO_AA
    (void)r; (void)dy;                    /* no shadows without alpha compositing */
    return;
#else
    unsigned col = AUI_SHADOW;
    r = clamp_radius(w, h, r);
    int sx = X_(x), sy = Y_(y) + dy;
    int T = blur + r;                                  /* corner tile edge, points */
    int cw = devlen(sx - blur, T), ch = devlen(sy - blur, T);
    int rd = imin(aui_dev(r), cw - 1);
    if (rd < 0) rd = 0;
    const unsigned char *m = mask_get(MK_SHADOW, cw, ch, rd);
    if (m) {
        blit_mask(sx - blur,     sy - blur,     T, T, m, cw, ch, col, alpha, 0, 0);
        blit_mask(sx + w - r,    sy - blur,     T, T, m, cw, ch, col, alpha, 1, 0);
        blit_mask(sx - blur,     sy + h - r,    T, T, m, cw, ch, col, alpha, 0, 1);
        blit_mask(sx + w - r,    sy + h - r,    T, T, m, cw, ch, col, alpha, 1, 1);
    }
    /* The offset exposes a sliver of the shadow box's INTERIOR below the caster,
     * and the 8 slices deliberately do not paint the interior. Left out, every
     * elevated card in the system shows a `dy`-pixel gap of clean background
     * between itself and its own shadow -- which is exactly what a shadow never
     * does. One flat band closes it, still O(1). (Caught by the "falls off with
     * distance" assertion in tests/qmp/qmp_gallery.py, which read background
     * where the darkest part of the shadow should have been.) */
    if (dy > 0) aui_fill_a(x + r, y + h, w - 2 * r, dy, col, alpha);
    int bd = devlen(sy - blur, blur);
    shadow_edge(sx + r, sy - blur, w - 2 * r, blur, bd, 1, 0, col, alpha);
    shadow_edge(sx + r, sy + h,    w - 2 * r, blur, bd, 1, 1, col, alpha);
    bd = devlen(sx - blur, blur);
    shadow_edge(sx - blur, sy + r, blur, h - 2 * r, bd, 0, 0, col, alpha);
    shadow_edge(sx + w,    sy + r, blur, h - 2 * r, bd, 0, 1, col, alpha);
#endif
}

void aui_shadow(int x, int y, int w, int h, int r, int elev)
{
    switch (elev) {
        case AUI_ELEV_1: aui_shadow_ex(x, y, w, h, r, 1, 4,  aui_is_dark() ? 90  : 40); break;
        case AUI_ELEV_2: aui_shadow_ex(x, y, w, h, r, 3, 10, aui_is_dark() ? 120 : 55); break;
        case AUI_ELEV_3: aui_shadow_ex(x, y, w, h, r, 8, 22, aui_is_dark() ? 150 : 70); break;
        default: break;
    }
}

void aui_panel(int x, int y, int w, int h, unsigned color) { aui_fill(x, y, w, h, color); }

void aui_card(int x, int y, int w, int h, int elev)
{
    aui_shadow(x, y, w, h, AUI_R_LG, elev);
    aui_round(x, y, w, h, AUI_R_LG, AUI_SURFACE);
    aui_stroke(x, y, w, h, AUI_R_LG, 1, AUI_BORDER);
}

void aui_glass(int x, int y, int w, int h, int radius)
{
    aui_ensure();
    if (theme_dark) gui_glass(X_(x), Y_(y), w, h, radius, 34, 36, 46, 120);
    else            gui_glass(X_(x), Y_(y), w, h, radius, 255, 255, 255, 50);
}

/* ------------------------------------------------------------------ text */

#define PX AUI_FS_BODY

static int tw(const char *s) { return text_measure_px(s, slen(s), PX, 0); }
static int twn(const char *s, int n) { return n <= 0 ? 0 : text_measure_px(s, n, PX, 0); }
static void txt(int x, int y, unsigned c, const char *s)
{ gui_text_run(X_(x), Y_(y), PX, 0, c, s, slen(s)); }

int  aui_text_w(const char *s, int px) { return text_measure_px(s, slen(s), px, 0); }
void aui_text_sz(int x, int y, const char *s, unsigned color, int px)
{ gui_text_run(X_(x), Y_(y), px, 0, color, s, slen(s)); }
void aui_heading(int x, int y, const char *s, unsigned color) { aui_text_sz(x, y, s, color, AUI_FS_TITLE); }
void aui_label(int x, int y, const char *s, unsigned color) { txt(x, y, color, s); }

void aui_text_in(struct aui_rect r, const char *s, unsigned color, int px, int align)
{
    int w = aui_text_w(s, px);
    int x = r.x;
    if (align == AUI_ALIGN_CENTER) x = r.x + (r.w - w) / 2;
    else if (align == AUI_ALIGN_RIGHT) x = r.x + r.w - w;
    aui_text_sz(x, r.y + (r.h - px) / 2 - 1, s, color, px);
}

void aui_text_ellipsis(int x, int y, int maxw, const char *s, unsigned color, int px)
{
    int n = slen(s);
    if (text_measure_px(s, n, px, 0) <= maxw) { aui_text_sz(x, y, s, color, px); return; }
    int ew = text_measure_px("...", 3, px, 0);
    int lo = 0, hi = n;
    while (lo < hi) {                                  /* binary search, not a scan:
                                                        * each probe is a syscall */
        int mid = (lo + hi + 1) / 2;
        if (text_measure_px(s, mid, px, 0) + ew <= maxw) lo = mid; else hi = mid - 1;
    }
    gui_text_run(X_(x), Y_(y), px, 0, color, s, lo);
    gui_text_run(X_(x) + text_measure_px(s, lo, px, 0), Y_(y), px, 0, color, "...", 3);
}

/* -------------------------------------------------- 5b. frame + input */

unsigned aui_ms(void) { return frame_ms; }

void aui_set_size(int w, int h) { win_w = w; win_h = h; }
int  aui_width(void)  { return win_w; }
int  aui_height(void) { return win_h; }

int aui_focus_id(void) { return focus_id; }
void aui_set_focus(int id) { focus_id = id; }
int aui_focus_visible(void) { return focus_vis; }

void aui_focus_next(int dir)
{
    if (foc_n <= 0) return;
    focus_vis = 1;
    int at = -1;
    for (int i = 0; i < foc_n; i++) if (foc_ids[i] == focus_id) { at = i; break; }
    if (at < 0) { focus_id = foc_ids[dir >= 0 ? 0 : foc_n - 1]; return; }
    at = (at + (dir >= 0 ? 1 : foc_n - 1)) % foc_n;
    focus_id = foc_ids[at];
}

void aui_feed(const struct logit_event *e)
{
    in.ev = e->type; in.a = e->a; in.b = e->b;
    in.mods = e->mods; in.button = e->button; in.wheel = e->wheel;
    in.key_used = 0;
    in.repaint = 1;
    switch (e->type) {
    case EV_MOUSE:
    case EV_MOUSE_R:
    case EV_MOUSE_UP:
    case EV_MOUSE_MOVE:
    case EV_WHEEL:
        in.mx = e->a; in.my = e->b;
        break;
    default: break;
    }
    if (e->type == EV_MOUSE && e->button != EV_BTN_RIGHT) in.down = 1;
    /* Drop the press BEFORE the frame that handles the release, not after it:
     * clearing it in aui_feed_done() means the very frame drawn for the mouse-up
     * still paints the button pressed, and if nothing else happens (a click that
     * opened a modal, say) that is the last frame drawn and the button stays
     * pressed on screen indefinitely. */
    if (e->type == EV_MOUSE_UP) { in.down = 0; in.active = 0; }
    /* Motion only matters if it can change a highlight. Answering that here is
     * what lets an app feed every motion sample and still repaint only when the
     * picture would differ. */
    if (e->type == EV_MOUSE_MOVE) {
        int in_hot = in.hot && aui_hit(hot_rect, in.mx, in.my);
        int in_bb  = wbb_any && aui_hit(wbb, in.mx, in.my);
        in.repaint = in.down || tip_text != 0 || (in_bb && !in_hot) || (in.hot && !in_hot);
    }
    /* Tab is the toolkit's, not the app's. foc_ids still holds the PREVIOUS
     * frame's focusables here, which is exactly the list Tab should walk. */
    if (e->type == EV_KEY && e->a == '\t') {
        aui_focus_next((e->mods & EV_MOD_SHIFT) ? -1 : 1);
        in.ev = 0; in.key_used = 1; in.repaint = 1;
    }
    if (e->type == EV_KEY) focus_vis = 1;
    if (e->type == EV_THEME) { aui_set_dark(sys_ui_dark(-1) > 0); in.repaint = 1; }
}

void aui_feed_done(void)
{
    in.ev = 0; in.wheel = 0;
    if (!in.down) in.active = 0;
}

int aui_want_repaint(void) { return in.repaint; }

/* Hover has to survive a frame with no motion in it, so `hot` is recomputed by
 * every widget poll and only reset at aui_begin. */
static int pt_in(int x, int y, int w, int h) { return in.mx >= x && in.my >= y && in.mx < x + w && in.my < y + h; }

static int input_ok(int x, int y, int w, int h)
{
    if (modal_prev && !in_dialog) return 0;
    if (!in_popup && pop_prev.w > 0 &&
        in.mx >= pop_prev.x && in.my >= pop_prev.y &&
        in.mx < pop_prev.x + pop_prev.w && in.my < pop_prev.y + pop_prev.h) return 0;
    if (!clip_has(in.mx, in.my)) return 0;
    return pt_in(x, y, w, h);
}

struct wres { int st, clicked; };

/* One widget's interaction, in WINDOW coordinates. `enabled` 0 makes the widget
 * inert and reports AUI_OFF so the caller can draw it that way.
 *
 * Activation is on PRESS, not release. That is the semantics the six apps that
 * already link this file were written against (and that every QMP driver
 * clicks), so it is not something to modernise casually; AUI_ACTIVE gives the
 * pressed look, and the release is still tracked so drags work. */
static struct wres wpoll(int id, int x, int y, int w, int h, int enabled, int focusable)
{
    struct wres r; r.st = 0; r.clicked = 0;
    if (focusable && enabled && foc_n < 128) foc_ids[foc_n++] = id;
    if (!wbb_next.w) { wbb_next = aui_r(x, y, w, h); }
    else {
        int x0 = imin(wbb_next.x, x), y0 = imin(wbb_next.y, y);
        int x1 = imax(wbb_next.x + wbb_next.w, x + w), y1 = imax(wbb_next.y + wbb_next.h, y + h);
        wbb_next = aui_r(x0, y0, x1 - x0, y1 - y0);
    }
    if (!enabled) { r.st = AUI_OFF; return r; }
    int over = input_ok(x, y, w, h);
    if (over) {
        r.st |= AUI_HOVER;
        hot_rect = aui_r(x, y, w, h);
        if (in.hot != id) { in.hot = id; in.hot_t0 = frame_ms; }
    }
    if (in.active == id) r.st |= AUI_ACTIVE;
    if (focus_id == id) r.st |= AUI_FOCUSED;
    if (in.ev == EV_MOUSE && in.button != EV_BTN_RIGHT && over) {
        in.active = id; r.st |= AUI_ACTIVE;
        if (focusable) { focus_id = id; focus_vis = 0; }
        r.clicked = 1;
    }
    if (in.ev == EV_KEY && !in.key_used && focus_id == id && enabled &&
        (in.a == '\n' || in.a == ' ')) { r.clicked = 1; in.key_used = 1; }
    return r;
}

/* The colour a control's face should be in a given state. One place, so every
 * control in the system lights up by the same rule. */
static unsigned face_for(int st)
{
    if (st & AUI_OFF) return AUI_DISABLED;
    if (st & AUI_ACTIVE) return AUI_FACE_ACTIVE;
    if (st & AUI_HOVER) return AUI_FACE_HOVER;
    return AUI_FACE;
}

/* Two concentric RINGS, never a filled halo: a filled rounded rect behind the
 * control is drawn over it by the control itself, and drawn after it washes the
 * control blue. Rings also survive on any background, which is the point -- the
 * ring has to read over a card, over glass and over an image. */
static void focus_ring(int x, int y, int w, int h, int r)
{
    if (!focus_vis) return;
    aui_stroke(x - 4, y - 4, w + 8, h + 8, r + 4, 2, aui_mix(AUI_BG, AUI_FOCUS, 110));
    aui_stroke(x - 2, y - 2, w + 4, h + 4, r + 2, 2, AUI_FOCUS);
}

void aui_begin(unsigned bg)
{
    int s = sys_ui_dark(-1) > 0;            /* live-follow the system theme each frame */
    if (!theme_inited || s != theme_dark) { aui_set_dark(s); bg = aui_t.bg; }
    aui_ensure();
    frame_ms = (unsigned)monotonic_ms();
    id_ctr = 0; foc_n = 0; clipn = 0; ox_ = oy_ = 0;
    in_dialog = 0; in_popup = 0; dlg_open_now = 0;
    in.hot = 0;
    wbb_next = aui_r(0, 0, 0, 0);
    tip_text = 0;
    gui_clip(0, 0, 0, 0);
    gui_clear(bg);
#ifdef AUI_COST
    /* The frame's wall clock starts AFTER the theme probe so the residual below
     * is drawing, not startup. */
    ck_fstart = monotonic_ns();
#endif
}

/* Popups and tooltips are drawn LAST so they sit over everything, and they are
 * hit-tested here too -- which is why widgets drawn earlier consult the previous
 * frame's popup rect before accepting a click. */
static void draw_popup(void);
static void draw_tip(void);

#ifdef AUI_COST
/* Print the split on the serial console every two seconds, in the same shape
 * gallery.c uses for its own frame total so one harness can read both. */
static void ck_report(void)
{
    ck_frames++;
    unsigned now = (unsigned)monotonic_ms();
    if (!ck_last) { ck_last = now; return; }
    if (now - ck_last < 2000) return;
    ck_last = now;
    unsigned long long n = ck_frames ? ck_frames : 1;
    unsigned long long shape_us = ck_shape / 1000 / n;
    unsigned long long clear_us = ck_clear / 1000 / n;
    unsigned long long text_us = ck_text / 1000 / n;
    unsigned long long other_us = ck_other / 1000 / n;
    /* THE NUMBER THAT IS ACTUALLY THE ENGINE. The four buckets above are time
     * spent INSIDE syscalls -- the compositor filling pixels and the kernel
     * rasterizing glyphs -- and none of it is Open Logit. The engine runs in
     * ring 3 between those calls, so its cost is the residual: frame wall time
     * minus everything the kernel was doing. It also includes aui's own widget
     * and layout logic, which is why it is labelled `app`, not `raster`. The
     * mask-cache MISS count is the sharper instrument: a miss is one corner
     * tile actually rasterized, and it is what the residual is made of. */
    unsigned long long sys = ck_clear + ck_text + ck_shape + ck_other;
    unsigned long long app_us = (ck_wall > sys ? ck_wall - sys : 0) / 1000 / n;
    unsigned long long wall_us = ck_wall / 1000 / n;
    int hits = 0, misses = 0;
    gfx_mask_stats(&hits, &misses);
    int dmiss = misses - ck_miss0;
    ck_miss0 = misses;
    char b[160]; int q = 0;
    const char *k;
    char t[24];
    #define PUT(str) do { k = (str); while (*k) b[q++] = *k++; } while (0)
    #define NUM(v) do { unsigned long long _v = (v); int _i = 0; \
                        if (!_v) t[_i++] = '0'; \
                        while (_v) { t[_i++] = (char)('0' + _v % 10); _v /= 10; } \
                        while (_i) b[q++] = t[--_i]; } while (0)
    PUT("[gfx] w="); NUM((unsigned)win_w);
    PUT(" frames=");  NUM(ck_frames);
    PUT(" clear_us="); NUM(clear_us);
    PUT(" text_us=");  NUM(text_us);
    PUT(" shape_us="); NUM(shape_us);
    PUT(" other_us="); NUM(other_us);
    PUT(" app_us=");   NUM(app_us);
    PUT(" wall_us=");  NUM(wall_us);
    PUT(" tiles=");    NUM((unsigned)(dmiss < 0 ? 0 : dmiss));
    b[q++] = '\n';
    #undef PUT
    #undef NUM
    sys_write(1, b, q);
    ck_clear = ck_text = ck_shape = ck_other = ck_frames = ck_wall = 0;
}
#endif

void aui_end(void)
{
    ox_ = oy_ = 0; clipn = 0; gui_clip(0, 0, 0, 0);
    in_popup = 1;
    draw_popup();
    in_popup = 0;
    draw_tip();
    modal_prev = dlg_open_now;
    wbb = wbb_next; wbb_any = wbb.w > 0;
    rg_lo = rg_lo_a; rg_hi = rg_hi_a;      /* the radio range this frame observed */
    gui_flush();
#ifdef AUI_COST
    ck_wall += monotonic_ns() - ck_fstart;
    ck_report();
#endif
}

/* -------------------------------------------------------- 6. layout stacks */

struct stack { int x, y, x0, gap, horiz, cross; };
static struct stack stk[8];
static int stkn;

static void stack_open(int x, int y, int gap, int horiz, int cross)
{
    if (stkn >= 8) stkn = 7;
    stk[stkn].x = x; stk[stkn].y = y; stk[stkn].x0 = horiz ? y : x;
    stk[stkn].gap = gap; stk[stkn].horiz = horiz; stk[stkn].cross = cross;
    stkn++;
}
void aui_vstack(int x, int y, int gap) { stkn = 0; stack_open(x, y, gap, 0, 0); }
void aui_hstack(int x, int y, int gap) { stkn = 0; stack_open(x, y, gap, 1, 0); }
void aui_vstack_w(int x, int y, int w, int gap) { stack_open(x, y, gap, 0, w); }
void aui_hstack_h(int x, int y, int h, int gap) { stack_open(x, y, gap, 1, h); }
void aui_stack_end(void) { if (stkn) stkn--; }

void aui_next(int w, int h, int *x, int *y)
{
    if (!stkn) { *x = 0; *y = 0; return; }
    struct stack *s = &stk[stkn - 1];
    *x = s->x; *y = s->y;
    if (s->horiz) s->x += w + s->gap; else s->y += h + s->gap;
}

void aui_row(struct aui_rect *out, int w, int h)
{
    if (!stkn) { out->x = out->y = 0; out->w = w; out->h = h; return; }
    struct stack *s = &stk[stkn - 1];
    if (w == AUI_FILL) w = s->horiz ? 0 : (s->cross > 0 ? s->cross : 0);
    if (h == AUI_FILL) h = s->horiz ? (s->cross > 0 ? s->cross : 0) : 0;
    out->x = s->x; out->y = s->y; out->w = w; out->h = h;
    if (s->horiz) s->x += w + s->gap; else s->y += h + s->gap;
}

void aui_spacer(int n)
{
    if (!stkn) return;
    struct stack *s = &stk[stkn - 1];
    if (s->horiz) s->x += n; else s->y += n;
}

struct aui_rect aui_inset(struct aui_rect r, int dx, int dy)
{ r.x += dx; r.y += dy; r.w -= 2 * dx; r.h -= 2 * dy; return r; }
struct aui_rect aui_cut_top(struct aui_rect *r, int h)
{ struct aui_rect o = *r; o.h = h; r->y += h; r->h -= h; return o; }
struct aui_rect aui_cut_bottom(struct aui_rect *r, int h)
{ struct aui_rect o = *r; o.y = r->y + r->h - h; o.h = h; r->h -= h; return o; }
struct aui_rect aui_cut_left(struct aui_rect *r, int w)
{ struct aui_rect o = *r; o.w = w; r->x += w; r->w -= w; return o; }
struct aui_rect aui_cut_right(struct aui_rect *r, int w)
{ struct aui_rect o = *r; o.x = r->x + r->w - w; o.w = w; r->w -= w; return o; }
int aui_hit(struct aui_rect r, int x, int y)
{ return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* ------------------------------------------------------------- 7. widgets */

int aui_button_ex(int x, int y, int w, int h, const char *label, enum aui_variant v, int enabled)
{
    int id = ++id_ctr;
    int wx = X_(x), wy = Y_(y);
    struct wres r = wpoll(id, wx, wy, w, h, enabled, 1);
    int rad = imin(h / 2, AUI_R_MD + 3);
    unsigned fill, fg = AUI_TEXT;

    switch (v) {
    case AUI_V_PRIMARY:
        fill = AUI_ACCENT; fg = AUI_ACCENT_TEXT;
        if (r.st & AUI_ACTIVE) fill = aui_shade(fill, -26);
        else if (r.st & AUI_HOVER) fill = aui_shade(fill, 14);
        break;
    case AUI_V_DANGER:
        fill = AUI_ERROR; fg = rgb(255, 255, 255);
        if (r.st & AUI_ACTIVE) fill = aui_shade(fill, -26);
        else if (r.st & AUI_HOVER) fill = aui_shade(fill, 14);
        break;
    case AUI_V_GHOST:
        fill = 0; fg = AUI_ACCENT;
        break;
    case AUI_V_GLASS:
        fill = 0; break;
    default:
        fill = face_for(r.st); break;
    }
    if (r.st & AUI_OFF) { fill = AUI_DISABLED; fg = AUI_DISABLED_TX; }

    if (v == AUI_V_GLASS && !(r.st & AUI_OFF)) {
        int rr = imin(h / 2, 11);
        if (r.st & AUI_ACTIVE) {
            if (theme_dark) gui_glass(wx, wy, w, h, rr, 94, 150, 255, 180);
            else            gui_glass(wx, wy, w, h, rr, 64, 130, 246, 170);
            fg = AUI_ACCENT_TEXT;
        } else {
            aui_glass(x, y, w, h, rr);
            if (r.st & AUI_HOVER) aui_round_a(x, y, w, h, rr, AUI_ACCENT, 26);
        }
    } else if (v == AUI_V_GHOST) {
        if (r.st & AUI_ACTIVE)      aui_round_a(x, y, w, h, rad, AUI_ACCENT, 52);
        else if (r.st & AUI_HOVER)  aui_round_a(x, y, w, h, rad, AUI_ACCENT, 26);
        if (r.st & AUI_OFF) fg = AUI_DISABLED_TX;
    } else {
        /* A control face is a gradient, not a flat fill: a single flat tone with
         * a hard edge is most of what "1998" looks like. Two steps of tone plus
         * an anti-aliased corner is most of what fixes it. */
        aui_vgrad_round(x, y, w, h, rad, aui_shade(fill, 10), aui_shade(fill, -10));
        if (v == AUI_V_SECONDARY) aui_stroke(x, y, w, h, rad, 1, AUI_BORDER);
    }
    if (r.st & AUI_FOCUSED) focus_ring(x, y, w, h, rad);

    int lw = tw(label);
    txt(x + (w - lw) / 2, y + (h - PX) / 2 - 1, fg, label);
    return r.clicked;
}

int aui_button(int x, int y, int w, int h, const char *label)
{ return aui_button_ex(x, y, w, h, label, AUI_V_GLASS, 1); }

int aui_icon_button(int x, int y, int size, int icon, int enabled)
{
    int id = ++id_ctr;
    struct wres r = wpoll(id, X_(x), Y_(y), size, size, enabled, 1);
    int rad = AUI_R_MD;
    if (r.st & AUI_ACTIVE)     aui_round_a(x, y, size, size, rad, AUI_ACCENT, 60);
    else if (r.st & AUI_HOVER) aui_round_a(x, y, size, size, rad, AUI_TEXT, 22);
    if (r.st & AUI_FOCUSED) focus_ring(x, y, size, size, rad);
    unsigned c = (r.st & AUI_OFF) ? AUI_DISABLED_TX : AUI_TEXT;
    int ip = size * 3 / 5;
    gui_icon(icon, X_(x) + (size - ip) / 2, Y_(y) + (size - ip) / 2, ip, c);
    return r.clicked;
}

void aui_tooltip(const char *s)
{
    /* The widget just polled is the one this belongs to. A tooltip that appears
     * instantly is noise, so it waits for the pointer to settle. */
    if (!s || in.hot != id_ctr) return;
    if (frame_ms - in.hot_t0 < 450) return;
    tip_text = s; tip_x = in.mx; tip_y = in.my;
}

static void draw_tip(void)
{
    if (!tip_text) return;
    int pad = AUI_SP(2), px = AUI_FS_LABEL;
    int w = aui_text_w(tip_text, px) + 2 * pad, h = px + 2 * pad;
    int x = tip_x + 12, y = tip_y + 18;
    if (x + w > win_w) x = win_w - w - 2;
    if (y + h > win_h) y = tip_y - h - 6;
    if (x < 2) x = 2;
    if (y < 2) y = 2;
    aui_shadow(x, y, w, h, AUI_R_SM, AUI_ELEV_2);
    aui_round(x, y, w, h, AUI_R_SM, aui_is_dark() ? rgb(70, 72, 84) : rgb(48, 50, 60));
    aui_text_sz(x + pad, y + pad - 1, tip_text, rgb(250, 250, 252), px);
}

int aui_checkbox_ex(int x, int y, const char *label, int *state, int enabled)
{
    int id = ++id_ctr, box = 18, lw = label ? tw(label) : 0;
    int w = box + (label ? AUI_SP(2) + lw : 0);
    struct wres r = wpoll(id, X_(x), Y_(y), w, box, enabled, 1);
    unsigned fill = *state ? AUI_ACCENT : AUI_SURFACE;
    if (r.st & AUI_OFF) fill = *state ? AUI_DISABLED_TX : AUI_DISABLED;
    else if (r.st & AUI_HOVER) fill = *state ? aui_shade(fill, 14) : AUI_FACE_HOVER;

    if (r.st & AUI_FOCUSED) focus_ring(x, y, box, box, AUI_R_SM);
    aui_round(x, y, box, box, AUI_R_SM, fill);
    if (!*state) aui_stroke(x, y, box, box, AUI_R_SM, 1, (r.st & AUI_OFF) ? AUI_DISABLED : AUI_BORDER);
    if (*state) {
        /* The tick is three anti-aliased rounded bars, not a bitmap: it has to
         * stay sharp at 2x like the text next to it. */
        unsigned c = AUI_ACCENT_TEXT;
        aui_round(x + 4,  y + box / 2 - 1, 5, 3, 1, c);
        aui_round(x + 6,  y + box / 2 + 1, 3, 3, 1, c);
        aui_round(x + 8,  y + box / 2 - 1, 3, 3, 1, c);
        aui_round(x + 10, y + box / 2 - 3, 3, 3, 1, c);
        aui_round(x + 12, y + 5,           3, 3, 1, c);
    }
    if (label) txt(x + box + AUI_SP(2), y + (box - PX) / 2 - 1,
                   (r.st & AUI_OFF) ? AUI_DISABLED_TX : AUI_TEXT, label);
    if (r.clicked) { *state = !*state; return 1; }
    return 0;
}

int aui_checkbox(int x, int y, const char *label, int *state)
{ return aui_checkbox_ex(x, y, label, state, 1); }

int aui_radio(int x, int y, const char *label, int *group, int value)
{
    int id = ++id_ctr, box = 18, lw = label ? tw(label) : 0;
    int w = box + (label ? AUI_SP(2) + lw : 0);
    struct wres r = wpoll(id, X_(x), Y_(y), w, box, 1, 1);
    int on = (*group == value);
    if (rg_ptr != group) { rg_ptr = group; rg_lo_a = rg_hi_a = value; }
    else { if (value < rg_lo_a) rg_lo_a = value; if (value > rg_hi_a) rg_hi_a = value; }
    int cx = x + box / 2, cy = y + box / 2;
    if (r.st & AUI_FOCUSED) focus_ring(x, y, box, box, box / 2);
    aui_circle(cx, cy, box / 2, on ? AUI_ACCENT
                                   : ((r.st & AUI_HOVER) ? AUI_FACE_HOVER : AUI_SURFACE));
    if (!on) aui_ring(cx, cy, box / 2, 1, AUI_BORDER);
    else     aui_circle(cx, cy, box / 5, AUI_ACCENT_TEXT);
    if (label) txt(x + box + AUI_SP(2), y + (box - PX) / 2 - 1, AUI_TEXT, label);
    /* Arrows walk a radio group, which is what makes it a group and not three
     * unrelated buttons. */
    if ((r.st & AUI_FOCUSED) && in.ev == EV_KEY && !in.key_used &&
        (in.a == KEY_DOWN || in.a == KEY_RIGHT || in.a == KEY_UP || in.a == KEY_LEFT)) {
        int d = (in.a == KEY_DOWN || in.a == KEY_RIGHT) ? 1 : -1;
        int nv = (rg_hi > rg_lo) ? iclamp(value + d, rg_lo, rg_hi) : value + d;
        in.key_used = 1;
        if (nv != value) { *group = nv; aui_focus_next(d); return 1; }
        return 0;
    }
    if (r.clicked && !on) { *group = value; return 1; }
    return 0;
}

int aui_toggle(int x, int y, int *state, int enabled)
{
    int id = ++id_ctr, w = 44, h = 24;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, enabled, 1);
    unsigned track = *state ? AUI_ACCENT : AUI_TRACK;
    if (r.st & AUI_OFF) track = AUI_DISABLED;
    else if (r.st & AUI_HOVER) track = aui_shade(track, *state ? 14 : -8);
    if (r.st & AUI_FOCUSED) focus_ring(x, y, w, h, h / 2);
    aui_round(x, y, w, h, h / 2, track);
    int kx = *state ? x + w - h + 2 : x + 2;
    aui_shadow_ex(kx, y + 2, h - 4, h - 4, (h - 4) / 2, 1, 3, 70);
    aui_circle(kx + (h - 4) / 2, y + h / 2, (h - 4) / 2, (r.st & AUI_OFF) ? AUI_DISABLED_TX : rgb(255, 255, 255));
    if (r.clicked) { *state = !*state; return 1; }
    return 0;
}

int aui_slider(int x, int y, int w, int *value, int lo, int hi)
{
    int id = ++id_ctr, h = 22, kr = 9;
    int wx = X_(x), wy = Y_(y);
    struct wres r = wpoll(id, wx - kr, wy, w + 2 * kr, h, 1, 1);
    if (hi <= lo) hi = lo + 1;
    int v = iclamp(*value, lo, hi), changed = 0;

    /* Drag: the press captured the widget, and motion goes to the capture
     * target, so this keeps tracking after the pointer leaves the groove. */
    int dragging = (in.active == id && in.down);
    if (r.clicked || (dragging && in.ev == EV_MOUSE_MOVE)) {
        int nv = lo + (in.mx - wx) * (hi - lo) / (w > 0 ? w : 1);
        nv = iclamp(nv, lo, hi);
        if (nv != v) { v = nv; changed = 1; }
    }
    if ((r.st & AUI_FOCUSED) && in.ev == EV_KEY && !in.key_used) {
        int step = imax(1, (hi - lo) / 50), nv = v;
        if (in.a == KEY_LEFT || in.a == KEY_DOWN) nv = v - step;
        else if (in.a == KEY_RIGHT || in.a == KEY_UP) nv = v + step;
        else if (in.a == KEY_HOME) nv = lo;
        else if (in.a == KEY_END)  nv = hi;
        nv = iclamp(nv, lo, hi);
        if (nv != v) { v = nv; changed = 1; in.key_used = 1; }
    }
    *value = v;

    int ty = y + h / 2 - 2, fill = (v - lo) * w / (hi - lo);
    aui_round(x, ty, w, 4, 2, AUI_TRACK);
    aui_round(x, ty, fill, 4, 2, AUI_ACCENT);
    int kx = x + fill;
    if (r.st & AUI_FOCUSED) focus_ring(kx - kr, y + h / 2 - kr, 2 * kr, 2 * kr, kr);
    aui_shadow_ex(kx - kr, y + h / 2 - kr, 2 * kr, 2 * kr, kr, 1, 4, 70);
    aui_circle(kx, y + h / 2, kr, rgb(255, 255, 255));
    if (r.st & (AUI_HOVER | AUI_ACTIVE)) aui_ring(kx, y + h / 2, kr, 2, AUI_ACCENT);
    else aui_ring(kx, y + h / 2, kr, 1, AUI_BORDER);
    return changed;
}

void aui_progress(int x, int y, int w, int pct)
{
    int h = 8;
    aui_round(x, y, w, h, h / 2, AUI_TRACK);
    if (pct >= 0) {
        int fw = iclamp(pct, 0, 100) * w / 100;
        aui_hgrad(x, y, fw, h, AUI_ACCENT, aui_shade(AUI_ACCENT, 30));
        aui_round(x, y, fw, h, h / 2, AUI_ACCENT);
    } else {
        /* Indeterminate: a bar that travels. Driven off the monotonic clock, so
         * it moves at the same speed whatever the repaint rate is. */
        int seg = w / 3;
        int t = (int)((frame_ms / 6) % (unsigned)(w + seg));
        int bx = x + t - seg;
        int x0 = imax(bx, x), x1 = imin(bx + seg, x + w);
        if (x1 > x0) aui_round(x0, y, x1 - x0, h, h / 2, AUI_ACCENT);
    }
}

void aui_spinner(int cx, int cy, int r)
{
    aui_ring(cx, cy, r, 2, AUI_TRACK);
    /* Eight fading dots around the ring: no trigonometry, a fixed unit circle
     * in eighths scaled by r (integer only, like everything else here). */
    static const int ux[8] = { 0, 181, 256, 181, 0, -181, -256, -181 };
    static const int uy[8] = { -256, -181, 0, 181, 256, 181, 0, -181 };
    unsigned phase = (frame_ms / 100) % 8;
    for (int i = 0; i < 8; i++) {
        int a = 40 + (int)((i + 8 - phase) % 8) * 27;
        aui_round_a(cx + ux[i] * r / 256 - 2, cy + uy[i] * r / 256 - 2, 4, 4, 2, AUI_ACCENT, a);
    }
}

/* ---- text field ----
 * Caret and selection live here rather than in the app because only the FOCUSED
 * field has any, so a single set of globals is the whole state. They are reset
 * when focus moves, which is also what makes clicking from one field to another
 * do the obvious thing. */
static int tf_owner, tf_caret, tf_anchor;
/* When the caret was last MOVED. A caret that only blinks off a wall clock
 * vanishes for half a second in an app that repaints on input and nothing else
 * -- which is most of them, and which is why it must be solid right after a
 * keystroke and only start blinking once the field has been left alone. */
static unsigned tf_touch;

static int tf_index_at(const char *buf, int n, int px_off)
{
    int lo = 0, hi = n;
    while (lo < hi) {                       /* binary search: each probe is a syscall */
        int mid = (lo + hi + 1) / 2;
        if (twn(buf, mid) <= px_off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static void tf_erase_sel(char *buf, int *n)
{
    int a = imin(tf_caret, tf_anchor), b = imax(tf_caret, tf_anchor);
    if (a == b) return;
    for (int i = a; i + (b - a) <= *n; i++) buf[i] = buf[i + (b - a)];
    *n -= (b - a);
    buf[*n] = 0;
    tf_caret = tf_anchor = a;
}

int aui_textfield_ex(int x, int y, int w, char *buf, int cap, const char *placeholder, int enabled)
{
    int id = ++id_ctr, h = AUI_H_CTL, ret = 0;
    int wx = X_(x), wy = Y_(y);
    struct wres r = wpoll(id, wx, wy, w, h, enabled, 1);
    int foc = (focus_id == id) && enabled;
    int n = slen(buf);
    int pad = AUI_SP(2);

    if (foc && tf_owner != id) { tf_owner = id; tf_caret = tf_anchor = n; tf_touch = frame_ms; }
    if (foc) { tf_caret = iclamp(tf_caret, 0, n); tf_anchor = iclamp(tf_anchor, 0, n); }

    if (foc && (r.clicked || (in.active == id && in.down && in.ev == EV_MOUSE_MOVE))) {
        int idx = tf_index_at(buf, n, in.mx - wx - pad);
        tf_caret = idx;
        if (r.clicked && !(in.mods & EV_MOD_SHIFT)) tf_anchor = idx;
    }

    if (foc && (r.clicked || in.ev == EV_KEY)) tf_touch = frame_ms;
    if (foc && in.ev == EV_KEY && !in.key_used) {
        int k = in.a, shift = in.mods & EV_MOD_SHIFT;
        in.key_used = 1;
        if (k == '\n') ret = 1;
        else if (k == 1) { tf_anchor = 0; tf_caret = n; }            /* Ctrl+A */
        else if (k == '\b' || k == 127) {
            if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
            else if (k == '\b' && tf_caret > 0) {
                for (int i = tf_caret - 1; i < n; i++) buf[i] = buf[i + 1];
                n--; tf_caret--; tf_anchor = tf_caret;
            } else if (k == 127 && tf_caret < n) {
                for (int i = tf_caret; i < n; i++) buf[i] = buf[i + 1];
                n--;
            }
            buf[n] = 0;
        }
        else if (k == KEY_LEFT)  { if (tf_caret > 0) tf_caret--; if (!shift) tf_anchor = tf_caret; }
        else if (k == KEY_RIGHT) { if (tf_caret < n) tf_caret++; if (!shift) tf_anchor = tf_caret; }
        else if (k == KEY_HOME)  { tf_caret = 0; if (!shift) tf_anchor = 0; }
        else if (k == KEY_END)   { tf_caret = n; if (!shift) tf_anchor = n; }
        else if (k >= 32 && k < 127) {
            if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
            if (n < cap - 1) {
                for (int i = n; i > tf_caret; i--) buf[i] = buf[i - 1];
                buf[tf_caret++] = (char)k; n++; buf[n] = 0; tf_anchor = tf_caret;
            }
        }
        else in.key_used = 0;                                        /* not ours */
    }

    unsigned bg = (r.st & AUI_OFF) ? AUI_DISABLED : AUI_SURFACE;
    aui_round(x, y, w, h, AUI_R_MD, bg);
    if (foc) focus_ring(x, y, w, h, AUI_R_MD);
    aui_stroke(x, y, w, h, AUI_R_MD, 1, foc ? AUI_FOCUS
                                            : ((r.st & AUI_HOVER) ? AUI_MUTED : AUI_BORDER));

    struct aui_rect ir = aui_r(x + 1, y + 1, w - 2, h - 2);
    clip_push(aui_r(X_(ir.x), Y_(ir.y), ir.w, ir.h));
    int ty = y + (h - PX) / 2 - 1;
    if (!n && placeholder) txt(x + pad, ty, AUI_MUTED, placeholder);
    if (foc && tf_caret != tf_anchor) {
        int a = imin(tf_caret, tf_anchor), b = imax(tf_caret, tf_anchor);
        int ax = twn(buf, a), bx = twn(buf, b);
        aui_fill(x + pad + ax, y + 4, bx - ax, h - 8, AUI_SELECTION);
    }
    gui_text_run(X_(x) + pad, Y_(ty), PX, 0, (r.st & AUI_OFF) ? AUI_DISABLED_TX : AUI_TEXT, buf, n);
    if (foc && (frame_ms - tf_touch < 600 || ((frame_ms / 500) & 1) == 0))
        aui_fill(x + pad + twn(buf, tf_caret), y + 5, 2, h - 10, AUI_TEXT);
    clip_pop();
    return ret;
}

int aui_textfield(int x, int y, int w, char *buf, int cap)
{ return aui_textfield_ex(x, y, w, buf, cap, 0, 1); }

/* ---- scrolling ---- */

int aui_scrollbar(int x, int y, int h, int *off, int content, int view)
{
    int id = ++id_ctr, w = 10;
    int wx = X_(x), wy = Y_(y);
    if (content <= view) { *off = 0; return 0; }
    struct wres r = wpoll(id, wx - 3, wy, w + 6, h, 1, 0);
    int thumb_h = imax(24, h * view / content);
    int span = h - thumb_h;
    int changed = 0;
    int o = iclamp(*off, 0, content - view);

    if (r.clicked || (in.active == id && in.down && in.ev == EV_MOUSE_MOVE)) {
        int ty = in.my - wy - thumb_h / 2;
        int no = iclamp(span > 0 ? ty * (content - view) / span : 0, 0, content - view);
        if (no != o) { o = no; changed = 1; }
    }
    *off = o;
    int ty = span > 0 ? o * span / (content - view) : 0;
    aui_round(x, y, w, h, w / 2, AUI_TRACK);
    aui_round(x, y + ty, w, thumb_h, w / 2,
              (r.st & (AUI_HOVER | AUI_ACTIVE)) ? AUI_MUTED : aui_mix(AUI_TRACK, AUI_TEXT, 90));
    return changed;
}

static struct { int x, y, w, h, sx, sy; int *off; int content; } scr[4];
static int scrn;

void aui_scroll_begin(int x, int y, int w, int h, int *off, int content_h)
{
    if (scrn >= 4) return;
    int wx = X_(x), wy = Y_(y);
    int maxo = imax(0, content_h - h);
    *off = iclamp(*off, 0, maxo);
    if (in.ev == EV_WHEEL && input_ok(wx, wy, w, h) && maxo > 0) {
        *off = iclamp(*off + in.wheel * 48, 0, maxo);
    }
    scr[scrn].x = x; scr[scrn].y = y; scr[scrn].w = w; scr[scrn].h = h;
    scr[scrn].sx = ox_; scr[scrn].sy = oy_; scr[scrn].off = off; scr[scrn].content = content_h;
    scrn++;
    clip_push(aui_r(wx, wy, w, h));
    ox_ = wx; oy_ = wy - *off;
}

void aui_scroll_end(void)
{
    if (!scrn) return;
    scrn--;
    ox_ = scr[scrn].sx; oy_ = scr[scrn].sy;
    clip_pop();
    if (scr[scrn].content > scr[scrn].h)
        aui_scrollbar(scr[scrn].x + scr[scrn].w - 12, scr[scrn].y + 2, scr[scrn].h - 4,
                      scr[scrn].off, scr[scrn].content, scr[scrn].h);
}

/* ---- lists and tables ---- */

#define ROWH 26

static int list_body(int x, int y, int w, int h, const char *const *items, int n,
                     int *sel, int *scroll, const int *widths, int ncol)
{
    int id = ++id_ctr, activated = -1;
    int wx = X_(x), wy = Y_(y);
    /* The list itself is focusable; its rows are not, so Tab does not walk a
     * thousand-row table one row at a time. */
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);
    int nrow = ncol > 0 ? n / ncol : n;

    if (input_ok(wx, wy, w, h) && in.ev == EV_MOUSE) { focus_id = id; focus_vis = 0; }
    if (focused && in.ev == EV_KEY && !in.key_used) {
        if (in.a == KEY_DOWN)      { *sel = iclamp(*sel + 1, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == KEY_UP)   { *sel = iclamp(*sel - 1, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == KEY_HOME) { *sel = 0; in.key_used = 1; }
        else if (in.a == KEY_END)  { *sel = nrow - 1; in.key_used = 1; }
        else if (in.a == KEY_PGDN) { *sel = iclamp(*sel + h / ROWH, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == KEY_PGUP) { *sel = iclamp(*sel - h / ROWH, 0, nrow - 1); in.key_used = 1; }
        else if (in.a == '\n')     { activated = *sel; in.key_used = 1; }
        if (in.key_used) {                              /* keep the selection visible */
            int top = *sel * ROWH, bot = top + ROWH;
            if (top < *scroll) *scroll = top;
            if (bot > *scroll + h) *scroll = bot - h;
        }
    }

    aui_round(x, y, w, h, AUI_R_MD, AUI_SURFACE);
    if (focused) focus_ring(x, y, w, h, AUI_R_MD);
    aui_stroke(x, y, w, h, AUI_R_MD, 1, AUI_BORDER);

    aui_scroll_begin(x + 1, y + 1, w - 2, h - 2, scroll, nrow * ROWH);
    for (int i = 0; i < nrow; i++) {
        int ry = i * ROWH;
        if (ry + ROWH < *scroll || ry > *scroll + h) continue;     /* culled */
        int hovered = input_ok(X_(0), Y_(ry), w - 2, ROWH);
        if (i == *sel)      aui_fill(0, ry, w - 2, ROWH, AUI_ACCENT);
        else if (hovered)   aui_fill_a(0, ry, w - 2, ROWH, AUI_TEXT, 14);
        unsigned fg = (i == *sel) ? AUI_ACCENT_TEXT : AUI_TEXT;
        if (ncol > 0) {
            int cx = AUI_SP(2);
            for (int c = 0; c < ncol; c++) {
                aui_text_ellipsis(cx, ry + (ROWH - PX) / 2 - 1, widths[c] - AUI_SP(2),
                                  items[i * ncol + c], c ? (i == *sel ? fg : AUI_MUTED) : fg, PX);
                cx += widths[c];
            }
        } else {
            aui_text_ellipsis(AUI_SP(2), ry + (ROWH - PX) / 2 - 1, w - AUI_SP(6), items[i], fg, PX);
        }
        if (i + 1 < nrow) aui_fill(AUI_SP(2), ry + ROWH - 1, w - AUI_SP(4) - 12, 1,
                                   aui_mix(AUI_SURFACE, AUI_BORDER, 140));
        if (in.ev == EV_MOUSE && hovered) { *sel = i; activated = i; }
    }
    aui_scroll_end();
    return activated;
}

int aui_list(int x, int y, int w, int h, const char *const *items, int n, int *sel, int *scroll)
{ return list_body(x, y, w, h, items, n, sel, scroll, 0, 0); }

int aui_table(int x, int y, int w, int h, const char *const *cols, const int *widths,
              int ncol, const char *const *cells, int nrow, int *sel, int *scroll)
{
    int hh = 26;
    aui_round(x, y, w, hh + AUI_R_MD, AUI_R_MD, aui_mix(AUI_SURFACE, AUI_BG, 120));
    aui_fill(x, y + hh - 1, w, 1, AUI_BORDER);
    int cx = x + AUI_SP(2);
    for (int c = 0; c < ncol; c++) {
        aui_text_ellipsis(cx, y + (hh - AUI_FS_LABEL) / 2 - 1, widths[c] - AUI_SP(2),
                          cols[c], AUI_MUTED, AUI_FS_LABEL);
        cx += widths[c];
    }
    aui_stroke(x, y, w, hh, AUI_R_MD, 1, AUI_BORDER);
    return list_body(x, y + hh, w, h - hh, cells, nrow * ncol, sel, scroll, widths, ncol);
}

/* ---- tabs / segmented ---- */

int aui_tabs(int x, int y, int w, const char *const *items, int n, int *sel)
{
    int id = ++id_ctr, h = 34, changed = 0;
    if (n <= 0) return 0;
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);
    int cx = x, selx = x, selw = 0;
    aui_fill(x, y + h - 1, w, 1, AUI_BORDER);
    for (int i = 0; i < n; i++) {
        int tw_ = tw(items[i]) + AUI_SP(6);
        if (i == *sel) { selx = cx; selw = tw_; }
        int over = input_ok(X_(cx), Y_(y), tw_, h);
        if (over) in.hot = id;
        if (over && in.ev == EV_MOUSE) { if (*sel != i) { *sel = i; changed = 1; } focus_id = id; focus_vis = 0; }
        unsigned fg = (i == *sel) ? AUI_TEXT : AUI_MUTED;
        if (over && i != *sel) aui_fill_a(cx, y + 4, tw_, h - 5, AUI_TEXT, 12);
        aui_text_sz(cx + AUI_SP(3), y + (h - PX) / 2 - 1, items[i], fg, PX);
        if (i == *sel) aui_round(cx + AUI_SP(2), y + h - 3, tw_ - AUI_SP(4), 3, 1, AUI_ACCENT);
        cx += tw_;
    }
    if (focused) {
        /* Ring the SELECTED TAB, not the whole strip: a box drawn around every
         * tab at once says "one of these is focused" and nothing about which,
         * which is the only thing a focus indicator is for. */
        focus_ring(selx + 2, y + 3, selw - 4, h - 8, AUI_R_SM);
        if (in.ev == EV_KEY && !in.key_used) {
            if (in.a == KEY_LEFT)  { *sel = iclamp(*sel - 1, 0, n - 1); changed = 1; in.key_used = 1; }
            if (in.a == KEY_RIGHT) { *sel = iclamp(*sel + 1, 0, n - 1); changed = 1; in.key_used = 1; }
        }
    }
    return changed;
}

int aui_segmented(int x, int y, int w, int h, const char *const *items, int n, int *sel)
{
    int id = ++id_ctr, changed = 0;
    if (n <= 0) return 0;
    if (foc_n < 128) foc_ids[foc_n++] = id;
    int focused = (focus_id == id);
    int seg = w / n;
    aui_round(x, y, w, h, AUI_R_MD, AUI_TRACK);
    if (focused) focus_ring(x, y, w, h, AUI_R_MD);
    /* The moving pill is drawn first so the labels sit on top of it. */
    aui_shadow_ex(x + *sel * seg + 2, y + 2, seg - 4, h - 4, AUI_R_SM, 1, 3, 60);
    aui_round(x + iclamp(*sel, 0, n - 1) * seg + 2, y + 2, seg - 4, h - 4, AUI_R_SM, AUI_SURFACE);
    for (int i = 0; i < n; i++) {
        int sx = x + i * seg;
        int over = input_ok(X_(sx), Y_(y), seg, h);
        if (over) in.hot = id;
        if (over && in.ev == EV_MOUSE) { if (*sel != i) { *sel = i; changed = 1; } focus_id = id; focus_vis = 0; }
        aui_text_in(aui_r(sx, y, seg, h), items[i], i == *sel ? AUI_TEXT : AUI_MUTED,
                    AUI_FS_LABEL, AUI_ALIGN_CENTER);
    }
    if (focused && in.ev == EV_KEY && !in.key_used) {
        if (in.a == KEY_LEFT)  { *sel = iclamp(*sel - 1, 0, n - 1); changed = 1; in.key_used = 1; }
        if (in.a == KEY_RIGHT) { *sel = iclamp(*sel + 1, 0, n - 1); changed = 1; in.key_used = 1; }
    }
    return changed;
}

/* ---- dropdown + menu (deferred popups) ---- */

int aui_select(int x, int y, int w, const char *const *items, int n, int *sel)
{
    int id = ++id_ctr, h = AUI_H_CTL;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, 1, 1);
    unsigned fill = face_for(r.st);
    aui_vgrad_round(x, y, w, h, AUI_R_MD, aui_shade(fill, 10), aui_shade(fill, -10));
    if (focus_id == id) focus_ring(x, y, w, h, AUI_R_MD);
    aui_stroke(x, y, w, h, AUI_R_MD, 1, AUI_BORDER);
    const char *cur = (*sel >= 0 && *sel < n) ? items[*sel] : "";
    aui_text_ellipsis(x + AUI_SP(2), y + (h - PX) / 2 - 1, w - AUI_SP(9), cur, AUI_TEXT, PX);
    /* chevron */
    int ax = x + w - AUI_SP(5), ay = y + h / 2 - 1;
    for (int i = 0; i < 4; i++) aui_fill(ax + i, ay - 2 + i, 2, 2, AUI_MUTED);
    for (int i = 0; i < 4; i++) aui_fill(ax + 7 - i, ay - 2 + i, 2, 2, AUI_MUTED);

    if (r.clicked && pop.kind == 0) {
        pop.kind = 1; pop.owner = id; pop.x = X_(x); pop.y = Y_(y) + h + 4; pop.w = w;
        pop.itemh = ROWH; pop.n = n; pop.items = items; pop.sel = sel; pop.hi = *sel;
    }
    if ((focus_id == id) && in.ev == EV_KEY && !in.key_used &&
        (in.a == KEY_DOWN || in.a == KEY_UP)) {
        *sel = iclamp(*sel + (in.a == KEY_DOWN ? 1 : -1), 0, n - 1);
        in.key_used = 1; return 1;
    }
    if (pop_changed_id == id) { pop_changed_id = 0; return 1; }
    return 0;
}

static void draw_popup(void)
{
    if (pop.kind != 1) { pop_prev.w = 0; return; }
    int h = pop.n * pop.itemh + AUI_SP(2);
    int x = pop.x, y = pop.y;
    if (y + h > win_h) y = imax(2, win_h - h - 2);
    pop_prev = aui_r(x, y, pop.w, h);

    if (in.ev == EV_KEY) {
        if (in.a == KEY_DOWN)      pop.hi = iclamp(pop.hi + 1, 0, pop.n - 1);
        else if (in.a == KEY_UP)   pop.hi = iclamp(pop.hi - 1, 0, pop.n - 1);
        else if (in.a == '\n')     { *pop.sel = pop.hi; pop_changed_id = pop.owner; pop.kind = 0; }
        else if (in.a == 27)       pop.kind = 0;     /* Escape: see the note in aui.h */
        in.key_used = 1;
    }

    aui_shadow(x, y, pop.w, h, AUI_R_MD, AUI_ELEV_2);
    aui_round(x, y, pop.w, h, AUI_R_MD, AUI_SURFACE_2);
    aui_stroke(x, y, pop.w, h, AUI_R_MD, 1, AUI_BORDER);
    int hit = -1;
    for (int i = 0; i < pop.n; i++) {
        int iy = y + AUI_SP(1) + i * pop.itemh;
        int over = in.mx >= x && in.mx < x + pop.w && in.my >= iy && in.my < iy + pop.itemh;
        if (over) { pop.hi = i; hit = i; }
        if (i == pop.hi) aui_round(x + 3, iy, pop.w - 6, pop.itemh, AUI_R_SM, AUI_ACCENT);
        aui_text_ellipsis(x + AUI_SP(2), iy + (pop.itemh - PX) / 2 - 1, pop.w - AUI_SP(5),
                          pop.items[i], i == pop.hi ? AUI_ACCENT_TEXT : AUI_TEXT, PX);
    }
    if (in.ev == EV_MOUSE) {
        if (hit >= 0) { *pop.sel = hit; pop_changed_id = pop.owner; }
        pop.kind = 0;                       /* a click anywhere closes it */
    }
}

int aui_menubar(int x, int y, int w, const char *const *titles,
                const char *const *const *items, int n, int *mi, int *ii)
{
    static int open_menu = -1;
    int h = 28, chosen = 0;
    aui_fill(x, y, w, h, aui_mix(AUI_BG, AUI_SURFACE, 160));
    aui_hairline(x, y + h - 1, w);
    int cx = x + AUI_SP(2);
    for (int i = 0; i < n; i++) {
        int id = ++id_ctr;
        int tw_ = tw(titles[i]) + AUI_SP(4);
        struct wres r = wpoll(id, X_(cx), Y_(y), tw_, h - 1, 1, 0);
        if (r.st & AUI_HOVER && open_menu >= 0 && open_menu != i) open_menu = i;
        if (r.clicked) open_menu = (open_menu == i) ? -1 : i;
        if (open_menu == i) aui_round(cx, y + 2, tw_, h - 5, AUI_R_SM, AUI_ACCENT);
        else if (r.st & AUI_HOVER) aui_round_a(cx, y + 2, tw_, h - 5, AUI_R_SM, AUI_TEXT, 16);
        aui_text_in(aui_r(cx, y, tw_, h - 1), titles[i],
                    open_menu == i ? AUI_ACCENT_TEXT : AUI_TEXT, AUI_FS_LABEL, AUI_ALIGN_CENTER);
        if (open_menu == i && pop.kind == 0) {
            int cnt = 0; while (items[i][cnt]) cnt++;
            pop.kind = 1; pop.owner = -2 - i; pop.x = X_(cx); pop.y = Y_(y) + h;
            pop.w = 160; pop.itemh = ROWH; pop.n = cnt;
            pop.items = items[i]; pop.sel = ii; pop.hi = -1;
        }
        cx += tw_;
    }
    if (pop_changed_id <= -2) { *mi = -pop_changed_id - 2; pop_changed_id = 0; open_menu = -1; chosen = 1; }
    if (pop.kind == 0 && open_menu >= 0 && in.ev == EV_MOUSE) open_menu = -1;
    return chosen;
}

/* ---- dialog ---- */

static struct { int x, y, w, h; } dlg;

int aui_dialog_begin(const char *title, int w, int h)
{
    dlg_open_now = 1;
    int th = 40;
    int x = (win_w - w) / 2, y = (win_h - h - th) / 3;
    if (y < 12) y = 12;
    dlg.x = x; dlg.y = y + th; dlg.w = w; dlg.h = h;
    aui_fill_a(0, 0, win_w, win_h, AUI_SCRIM, 110);          /* the scrim */
    aui_shadow(x, y, w, h + th, AUI_R_XL, AUI_ELEV_3);
    aui_round(x, y, w, h + th, AUI_R_XL, AUI_SURFACE_2);
    aui_stroke(x, y, w, h + th, AUI_R_XL, 1, AUI_BORDER);
    aui_text_in(aui_r(x, y, w, th), title, AUI_TEXT, AUI_FS_TITLE, AUI_ALIGN_CENTER);
    in_dialog = 1;
    clip_push(aui_r(x, y + th, w, h));
    ox_ = x; oy_ = y + th;
    return 1;
}

void aui_dialog_end(void)
{
    ox_ = 0; oy_ = 0;
    clip_pop();
    in_dialog = 0;
}

int aui_dialog_buttons(const char *const *labels, int n)
{
    int bh = AUI_H_CTL + 4, gap = AUI_SP(2), pressed = -1;
    int y = dlg.h - bh - AUI_SP(4);
    int x = dlg.w - AUI_SP(4);
    for (int i = n - 1; i >= 0; i--) {
        int bw = imax(84, tw(labels[i]) + AUI_SP(8));
        x -= bw;
        if (aui_button_ex(x, y, bw, bh, labels[i],
                          i == 0 ? AUI_V_PRIMARY : AUI_V_SECONDARY, 1)) pressed = i;
        x -= gap;
    }
    if (in.ev == EV_KEY && !in.key_used) {
        if (in.a == '\n') { pressed = 0; in.key_used = 1; }
        else if (in.a == 27) { pressed = n - 1; in.key_used = 1; }
    }
    return pressed;
}

void aui_badge(int x, int y, const char *s, unsigned color)
{
    int px = AUI_FS_CAPTION, pad = AUI_SP(2);
    int w = aui_text_w(s, px) + 2 * pad, h = px + AUI_SP(2) + 2;
    aui_round_a(x, y, w, h, h / 2, color, 46);
    aui_stroke(x, y, w, h, h / 2, 1, aui_mix(color, AUI_BG, 120));
    aui_text_in(aui_r(x, y, w, h), s, color, px, AUI_ALIGN_CENTER);
}
