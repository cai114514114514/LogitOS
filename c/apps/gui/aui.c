#include "openlogit_bitmap.h"
#include "aui.h"
#include "aui_damage.h"
#include "gfx.h"
#include "openlogit_anim.h"
#include "openlogit_glass_material.h"
#include "aui_scroll_motion.h"

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
 * A build without AUI_COST compiles to exactly what it did before modulo the
 * flush-rectangle tracking directly below -- which is unconditional now (it
 * is the mechanism, not a bench), so the macros below are never simply
 * absent, only the TIMING half of them is. */
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
/* ad_note_*() are the flush-rectangle recorders, section 5a-flush below --
 * declared by use here (C does not require the callee to exist yet at a
 * macro's #define, only at its later EXPANSION, and every ad_note_* is fully
 * defined before this file's first actual gui_rect()/gui_clear()/etc. call
 * site) so AUI_COST and the flush tracking compose instead of one silently
 * replacing the other's macro of the same name. */
#define gui_clear(a)                 (ad_note_clear(a), t0_(), (gui_clear)(a), t1_(&ck_clear))
#define gui_rect(a,b,c,d,e)          (ad_note_rect(a,b,c,d,e), t0_(), (gui_rect)(a,b,c,d,e), t1_(&ck_shape))
#define gui_rrect(a,b,c,d,e,f)       (ad_note_rrect(a,b,c,d,e,f), t0_(), (gui_rrect)(a,b,c,d,e,f), t1_(&ck_shape))
#define gui_blit(a,b,c,d,e,f,g)      (ad_note_blit(a,b,c,d,e,f,g), t0_(), (gui_blit)(a,b,c,d,e,f,g), t1_(&ck_shape))
#define gui_glass(a,b,c,d,e,f,g,h,i) (ad_note_glass(a,b,c,d,e,f,g,h,i), t0_(), (gui_glass)(a,b,c,d,e,f,g,h,i), t1_(&ck_shape))
#define gui_icon(a,b,c,d,e)          (ad_note_icon(a,b,c,d,e), t0_(), (gui_icon)(a,b,c,d,e), t1_(&ck_shape))
#define gui_text_run(a,b,c,d,e,f,g)  (ad_note_text(a,b,c,d,e,f,g), t0_(), (gui_text_run)(a,b,c,d,e,f,g), t1_(&ck_text))
#define gui_clip(a,b,c,d)            (t0_(), (gui_clip)(a,b,c,d), t1_(&ck_other))
#define gui_flush()                  (t0_(), (gui_flush)(), t1_(&ck_other))
#define text_measure_px(a,b,c,d)     tm_(a,b,c,d)
#else
/* THE UNCONDITIONAL HALF -- every build, not just -DAUI_COST, routes these
 * seven raw syscalls through the flush-rectangle recorders (section
 * 5a-flush). This is the ONE place that can see every pixel this file (or an
 * app built on it, since these are the ONLY seven primitives Open Logit's
 * masks and every widget bottom out to -- gui_rect for a fill, gui_blit for a
 * rounded corner/gradient/shadow tile, gui_glass, gui_icon, gui_text_run for
 * every label, gui_rrect for the AUI_NO_AA fallback, gui_clear for the
 * per-frame background) ever actually asks the compositor to change, so
 * hooking it here rather than in each of the ~20 widget entry points is what
 * makes the tracking complete BY CONSTRUCTION rather than by enumeration: a
 * widget that reaches outside its own nominal rect (a focus ring, a shadow, a
 * popup) is caught because its OWN pixels are recorded at their OWN
 * geometry, not because someone remembered to pad a box to guess at the
 * bleed. See the "cannot be seen by reading the widget's own code" trap this
 * guards against, spelled out on ad_note_text() below. */
#define gui_clear(a)                 (ad_note_clear(a), (gui_clear)(a))
#define gui_rect(a,b,c,d,e)          (ad_note_rect(a,b,c,d,e), (gui_rect)(a,b,c,d,e))
#define gui_rrect(a,b,c,d,e,f)       (ad_note_rrect(a,b,c,d,e,f), (gui_rrect)(a,b,c,d,e,f))
#define gui_blit(a,b,c,d,e,f,g)      (ad_note_blit(a,b,c,d,e,f,g), (gui_blit)(a,b,c,d,e,f,g))
#define gui_glass(a,b,c,d,e,f,g,h,i) (ad_note_glass(a,b,c,d,e,f,g,h,i), (gui_glass)(a,b,c,d,e,f,g,h,i))
#define gui_icon(a,b,c,d,e)          (ad_note_icon(a,b,c,d,e), (gui_icon)(a,b,c,d,e))
#define gui_text_run(a,b,c,d,e,f,g)  (ad_note_text(a,b,c,d,e,f,g), (gui_text_run)(a,b,c,d,e,f,g))
#endif

/* ------------------------------------------------------------ 1. utilities */

static int slen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int win_w = 640, win_h = 480;

/* ============================================================================
 * 5a-flush. THE FLUSH RECTANGLE -- what changed this frame, in ONE place.
 *
 * Until this section existed, aui_end() called gui_flush() unconditionally --
 * every widget's draw call ran every frame (this is immediate mode: there is
 * no retained tree to diff against, see the file header), and the ONLY
 * question this file ever asked the compositor was "recomposite my whole
 * canvas". CLAUDE.md's own measurement of that: a keystroke into TextEdit,
 * 93.9 ms/composite, 1.57M px (68% of a 1920x1200 screen) for one character.
 *
 * THE METHOD IS THE SAME ONE c/apps/gui/browser/paint/renderer.c's pd_finish()
 * uses (that file's header comment is the fuller argument; this is the same
 * idea moved one layer down): a POSITIONAL diff against last frame, not a
 * second render or a retained tree. The browser diffs LAYOUT ITEMS, one per
 * DOM node; this diffs DRAW CALLS, one per gui_rect/gui_blit/gui_glass/
 * gui_icon/gui_text_run/gui_rrect/gui_clear -- the seven raw primitives
 * EVERYTHING in this file and everything built on it (every widget, every
 * app's own direct aui_fill()/aui_round()/gui_text_run() call, draw_popup(),
 * draw_tip()) ultimately issues, hooked once each via the macros directly
 * above this section rather than instrumented at each of the ~20 widget
 * entry points separately.
 *
 * WHY THE PRIMITIVE LEVEL AND NOT THE WIDGET LEVEL -- THE TRAP THE TASK NAMED
 * BY NAME: a widget whose paint reaches outside its own nominal (x,y,w,h) --
 * focus_ring()'s stroke at -4/-6px, aui_shadow_ex()'s blur bleeding past the
 * box it shadows, a popup or the candidate/tooltip layer drawn nowhere near
 * the widget that owns it -- would silently under-report if this diffed "the
 * button's rect" the way a first-draft version of this might. It cannot,
 * because it never reasons about "the widget's rect" AT ALL: it reasons about
 * the ACTUAL (x,y,w,h) each raw syscall actually touches, wherever that is.
 * A focus ring is just three more gui_blit() calls at their own real
 * geometry, recorded and diffed exactly like the button's fill. This is also
 * why textedit.c's raw gui_text_run() calls (it does not route its paragraph
 * text through any aui_* wrapper) are the one thing this mechanism CANNOT
 * see -- they never pass through these seven macros, because textedit.c's
 * call is textually a different call site in a different translation unit,
 * where these #defines are not in scope. That gap is textedit.c's own
 * problem to close (aui_end_rect(), below, plus that file's own damage
 * computation) precisely because it is invisible from here.
 *
 * ORDER IS THE ONLY IDENTITY. Like pd_finish, entry i of this frame is
 * compared against entry i of last frame by INDEX, not by any notion of "the
 * same widget" -- id_ctr already gives widgets that, but a static aui_label()
 * or aui_fill() in the middle of a widget's body never gets an id, and
 * inventing a second identity scheme for those would be the very "two doors"
 * CLAUDE.md warns about. The same consequence follows as in the browser: an
 * insertion in the middle of a frame's draw sequence (a conditional widget
 * appearing) makes everything AFTER it compare unequal to last frame's entry
 * at that index, which over-reports (safe -- WIDEN BEFORE YOU NARROW) rather
 * than missing anything, because whatever comes after a real content change
 * differs at its own index too. */

#define AD_MAX 3072            /* see g_ad_overflow: past this, "no rect" */

struct ad_ent { int x, y, w, h; unsigned sig; };
/* Two FIXED buffers, swapped by pointer at frame end -- never realloc'd.
 * aui.c links no libc (the file header: clock.aex is crt0 + this file + gfx,
 * nothing else), so the browser's pd_ensure_cap()/realloc() growth strategy
 * is not available here; a compile-time ceiling plus "past it, flush the
 * whole canvas" is the honest equivalent of that function's own OOM handling
 * (fails toward gui_flush(), never toward a silently truncated diff). */
static struct ad_ent ad_bufA[AD_MAX], ad_bufB[AD_MAX];
static struct ad_ent *g_ad_prev = ad_bufA, *g_ad_cur = ad_bufB;
static int g_ad_prev_n, g_ad_cur_n;
static int g_ad_have_prev, g_ad_overflow;
static int g_ad_prev_w, g_ad_prev_h;   /* canvas size the PREV array was built at */
/* aui_end_rect()'s caller-supplied extra rect (textedit.c's own computed
 * damage) -- unioned in alongside whatever this frame's diff found, never
 * instead of it, so an app's own hint can never make aui's chrome (a status
 * bar redrawn via aui_fill, say) go unreported. */
static int g_ad_extra_have, g_ad_ex0, g_ad_ey0, g_ad_ex1, g_ad_ey1;
/* Opacity groups (aui_fade_begin): the children are drawn at FULL opacity, so
 * a fade that only moves the group alpha leaves every ad_record() entry
 * byte-identical. The groups are therefore diffed on their own, rect + alpha,
 * and a changed one reaches the flush set through ad_add_halo (ad_finish). */
#define AD_GMAX 8
struct ad_grp { int x, y, w, h; unsigned sig; };
static struct ad_grp ad_gbufA[AD_GMAX], ad_gbufB[AD_GMAX];
static struct ad_grp *g_ad_gprev = ad_gbufA, *g_ad_gcur = ad_gbufB;
static int g_ad_gprev_n, g_ad_gcur_n;

/* FNV-1a, 32-bit -- the same hash browser_paint.c's pd_item_sig() uses, for
 * the same reason: cheap, and collisions only ever make the diff report a
 * position UNCHANGED when it was not (a mismatch is caught by (x,y,w,h)
 * differing in the overwhelming majority of real edits, since almost nothing
 * repaints identical geometry with different content); it does not need to be
 * cryptographic, it needs to be fast enough to run on every draw call. */
static unsigned ad_hash_bytes(const void *p, long n, unsigned h)
{
    const unsigned char *b = (const unsigned char *)p;
    for (long i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}
#define AD_MIX(h, v) ((h) = ((h) ^ (unsigned)(v)) * 16777619u)

/* The one place an entry actually lands in this frame's array. w<=0||h<=0
 * is a no-op draw (every real primitive already refuses these before it
 * would reach the syscall -- aui_fill/round_impl/aui_stroke all `if (w <= 0
 * || h <= 0) return;` first) so recording it would just be a zero-area union
 * member forever, never contributing to any rect. */
static void ad_record(int x, int y, int w, int h, unsigned sig)
{
    if (w <= 0 || h <= 0) return;
    if (g_ad_cur_n >= AD_MAX) { g_ad_overflow = 1; return; }
    struct ad_ent *e = &g_ad_cur[g_ad_cur_n++];
    e->x = x; e->y = y; e->w = w; e->h = h; e->sig = sig;
}

static void ad_note_group(int x, int y, int w, int h, int alpha)
{
    if (w <= 0 || h <= 0) return;
    if (g_ad_gcur_n >= AD_GMAX) { g_ad_overflow = 1; return; }
    struct ad_grp *g = &g_ad_gcur[g_ad_gcur_n++];
    g->x = x; g->y = y; g->w = w; g->h = h; g->sig = (unsigned)alpha;
}

static void ad_note_clear(unsigned color)
{
    unsigned h = 2166136261u; AD_MIX(h, color);
    ad_record(0, 0, win_w, win_h, h);       /* gui_clear has no extent of its own: it IS the canvas */
}
static void ad_note_rect(int x, int y, int w, int h, unsigned color)
{
    unsigned s = 2166136261u; AD_MIX(s, color);
    ad_record(x, y, w, h, s);
}
/* unused in an ordinary build: gui_rrect() itself is only ever called from the
 * AUI_NO_AA fallback paths in round_impl()/aui_stroke() (a DIFFERENT negative
 * control, for anti-aliasing, not this one), so a build without -DAUI_NO_AA
 * never expands the gui_rrect macro and this is dead by construction, not by
 * omission -- kept anyway so an AUI_NO_AA build's flush-rect tracking is
 * exactly as complete as an ordinary build's, rather than silently losing
 * coverage of the one shape kind that build draws differently. */
static void ad_note_rrect(int x, int y, int w, int h, int r, unsigned color) __attribute__((unused));
static void ad_note_rrect(int x, int y, int w, int h, int r, unsigned color)
{
    unsigned s = 2166136261u; AD_MIX(s, r); AD_MIX(s, color);
    ad_record(x, y, w, h, s);
}
static void ad_note_blit(int x, int y, int w, int h, const unsigned char *rgba, int sw, int sh)
{
    /* The FULL buffer, not a sample of it -- a stride-sampled hash could miss
     * a change confined to the sampled-over bytes, which is an UNDER-report
     * (the dangerous direction; see the "widen before you narrow" rule). Cost
     * is bounded by BIG_MASK (256x256x4 = 256 KiB) and reached only on the
     * rare past-both-mask-ceilings path corner_mask()'s own comments already
     * name as rare; the common case is a cached corner tile a few KiB across,
     * a rounding error next to the rasterization that same call already
     * pays for. */
    unsigned s = 2166136261u; AD_MIX(s, sw); AD_MIX(s, sh);
    if (rgba && sw > 0 && sh > 0) s = ad_hash_bytes(rgba, (long)sw * sh * 4, s);
    ad_record(x, y, w, h, s);
}
static void ad_note_glass(int x, int y, int w, int h, int radius, int tr, int tg, int tb, int ta)
{
    /* gui_glass reads its OWN backdrop (frost + refraction over whatever is
     * already drawn beneath it) so its true visual result can change even
     * when these call parameters do not -- but c/kernel/gui/wm.c's own
     * dmg_expand ALREADY grows any damage rectangle touching a glass panel to
     * the whole panel (CLAUDE.md: "the compositor grows any damage touching a
     * glass panel until it contains the whole panel"), at the kernel level,
     * unconditionally. That is the existing safety net for exactly this
     * problem; this function only needs to report ITS OWN nominal geometry
     * so a glass call that is byte-identical to last frame's does not, on its
     * own, force a wider flush than the panel it already sits in. */
    unsigned s = 2166136261u;
    AD_MIX(s, radius); AD_MIX(s, tr); AD_MIX(s, tg); AD_MIX(s, tb); AD_MIX(s, ta);
    ad_record(x, y, w, h, s);
}
static void ad_note_icon(int icon, int x, int y, int size, unsigned color)
{
    unsigned s = 2166136261u; AD_MIX(s, icon); AD_MIX(s, color);
    ad_record(x, y, size, size, s);
}
/* THE TRAP NAMED IN THE SECTION HEADER, IN CONCRETE FORM: a text run's true
 * ink extends past its own nominal (px-tall) box -- descenders, italic
 * overhang (none in this tree yet, but the box should not assume it stays
 * that way), antialiasing bleed a pixel or two past the glyph's hinted edge.
 * Measuring the real extent would mean a second SYS_TEXT_MEASURE per draw
 * call, doubling the syscall count of every label in the system for a few
 * pixels of tightness. Padding vertically by HALF the point size (a quarter
 * until 2026-09-24 -- see the correction in the function body)
 * (floor 4px) and running the box to the window's own right edge -- rather
 * than trying to also learn the string's rendered WIDTH here -- is the
 * WIDEN-BEFORE-YOU-NARROW answer: always safe, costs nothing extra to
 * compute, and is clamped back down to whatever the real union turns out to
 * be by ad_finish()'s viewport clamp below regardless. */
static void ad_note_text(int x, int y, int px, int mono, unsigned color, const char *s, int len)
{
    unsigned h = 2166136261u; AD_MIX(h, px); AD_MIX(h, mono); AD_MIX(h, color); AD_MIX(h, len);
    if (s && len > 0) h = ad_hash_bytes(s, len, h);
    /* HALF the size, not a quarter. A quarter was enough while every change
     * in a frame was unioned into one box, because a neighbour's box usually
     * covered what a glyph overhung. Since the damage became a SET of boxes
     * (aui_damage.h) each text run is reported on its own, and at 22 px the
     * descenders of a replaced string ("Spotlight Search" -> "term") left
     * their last antialiased rows on screen, unreported. Found by looking at
     * Spotlight, 2026-09-24. */
    int pad = px / 2; if (pad < 4) pad = 4;
    int w = win_w - x; if (w < 1) w = 1;
    ad_record(x, y - pad, w, px + 2 * pad, h);
}

/* The damage is a small set of rectangles, not one bounding box: see
 * aui_damage.h for why and for the merge rule. */
static struct ad_set g_ad;
#define g_ad_r  (g_ad.r)
#define g_ad_nr (g_ad.n)

static void ad_union(int *any, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;
    *any = 1;
    ad_set_add(&g_ad, x0, y0, x1, y1);
}

/* -1: no honest diff (first frame, resize, or this frame's array overflowed)
 * -- caller must gui_flush() the whole canvas. 0: a rect was computed and it
 * is EMPTY -- nothing to flush at all. 1: g_ad_r{x,y,w,h} holds a real,
 * nonempty, canvas-clamped rect. Same three-way contract as
 * browser_paint_dirty_rect(), deliberately -- one jar, and now two doors that
 * agree on its shape instead of two that could drift. */
static int g_ad_result_valid;

/* The glass control panels drawn this frame (ad_note_glass2). A panel SAMPLES
 * the backdrop within its halo, so a changed pixel there changes the panel's own
 * pixels although its call is byte-identical: ad_finish grows the flush set to
 * contain the whole panel when a changed rectangle reaches its halo. */
#define AD_GLMAX 16
struct ad_gl { int x, y, w, h, halo; };
static struct ad_gl g_ad_gl[AD_GLMAX];
static int g_ad_gl_n;

static void ad_grow_glass(int *any)
{
    for (int pass = 0; pass < 2 && *any; pass++) {
        int grew = 0;
        for (int i = 0; i < g_ad_gl_n; i++) {
            const struct ad_gl *g = &g_ad_gl[i];
            int hit = 0, covered = 0;
            for (int k = 0; k < g_ad_nr; k++) {
                const struct ad_rect *r = &g_ad_r[k];
                if (r->x1 > g->x - g->halo && r->x0 < g->x + g->w + g->halo &&
                    r->y1 > g->y - g->halo && r->y0 < g->y + g->h + g->halo) hit = 1;
                if (r->x0 <= g->x && r->y0 <= g->y && r->x1 >= g->x + g->w && r->y1 >= g->y + g->h) covered = 1;
            }
            if (hit && !covered) { ad_union(any, g->x, g->y, g->x + g->w, g->y + g->h); grew = 1; }
        }
        if (!grew) break;
    }
}

static void ad_finish(void)
{
    int can_diff = g_ad_have_prev && !g_ad_overflow &&
                   g_ad_prev_w == win_w && g_ad_prev_h == win_h;
    int any = 0;
    g_ad_nr = 0;
    if (can_diff) {
        int n = g_ad_prev_n > g_ad_cur_n ? g_ad_prev_n : g_ad_cur_n;
        for (int i = 0; i < n; i++) {
            int inp = i < g_ad_prev_n, inc = i < g_ad_cur_n;
            if (inp && inc) {
                const struct ad_ent *a = &g_ad_prev[i], *b = &g_ad_cur[i];
                if (a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h && a->sig == b->sig)
                    continue;                          /* byte-identical to last pass */
                int ux0 = imin(a->x, b->x), uy0 = imin(a->y, b->y);
                int ux1 = imax(a->x + a->w, b->x + b->w), uy1 = imax(a->y + a->h, b->y + b->h);
                ad_union(&any, ux0, uy0, ux1, uy1);
            } else {
                const struct ad_ent *e = inc ? &g_ad_cur[i] : &g_ad_prev[i];
                ad_union(&any, e->x, e->y, e->x + e->w, e->y + e->h);
            }
        }
    }
    if (can_diff) {
        int gn = g_ad_gprev_n > g_ad_gcur_n ? g_ad_gprev_n : g_ad_gcur_n;
        int hl[4] = { 0, 0, 0, 0 };
        hl[0] = hl[1] = hl[2] = hl[3] = ad_halo_stroke(2);       /* the composite's AA edge */
        for (int i = 0; i < gn; i++) {
            int inp = i < g_ad_gprev_n, inc = i < g_ad_gcur_n;
            const struct ad_grp *a = inp ? &g_ad_gprev[i] : 0, *b = inc ? &g_ad_gcur[i] : 0;
            if (a && b && a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h &&
                a->sig == b->sig)
                continue;
            int x0 = a ? a->x : b->x, y0 = a ? a->y : b->y;
            int x1 = a ? a->x + a->w : b->x + b->w, y1 = a ? a->y + a->h : b->y + b->h;
            if (a && b) {
                x0 = imin(a->x, b->x); y0 = imin(a->y, b->y);
                x1 = imax(a->x + a->w, b->x + b->w); y1 = imax(a->y + a->h, b->y + b->h);
            }
            ad_add_halo(&g_ad, x0, y0, x1 - x0, y1 - y0, hl[0], hl[1], hl[2], hl[3]);
            any = 1;
        }
    }
    if (can_diff && any) ad_grow_glass(&any);
    if (g_ad_extra_have)
        ad_union(&any, g_ad_ex0, g_ad_ey0, g_ad_ex1, g_ad_ey1);

    if (!can_diff) {
        g_ad_result_valid = -1;
    } else if (!any) {
        g_ad_result_valid = 0;
    } else {
        /* Clamp each to the canvas; drop any that clamps to nothing. */
        int k = 0;
        for (int i = 0; i < g_ad_nr; i++) {
            struct ad_rect r = g_ad_r[i];
            if (r.x0 < 0) r.x0 = 0; if (r.y0 < 0) r.y0 = 0;
            if (r.x1 > win_w) r.x1 = win_w; if (r.y1 > win_h) r.y1 = win_h;
            if (r.x1 > r.x0 && r.y1 > r.y0) g_ad_r[k++] = r;
        }
        g_ad_nr = k;
        g_ad_result_valid = k ? 1 : 0;
    }
    /* Pointer swap, not a copy -- the browser's pd_finish() does the same
     * with realloc'd buffers; these are fixed, so the swap is simpler still
     * and (see the section-open comment on why no libc) sidesteps ever
     * needing a memcpy-shaped loop that -O2's loop-idiom pass could rewrite
     * into an actual memcpy() call this file cannot link. */
    struct ad_ent *t = g_ad_prev; g_ad_prev = g_ad_cur; g_ad_cur = t;
    g_ad_prev_n = g_ad_cur_n; g_ad_cur_n = 0;
    struct ad_grp *gt = g_ad_gprev; g_ad_gprev = g_ad_gcur; g_ad_gcur = gt;
    g_ad_gprev_n = g_ad_gcur_n; g_ad_gcur_n = 0;
    g_ad_prev_w = win_w; g_ad_prev_h = win_h;
    g_ad_have_prev = 1;
    g_ad_overflow = 0;
    g_ad_gl_n = 0;
    g_ad_extra_have = 0;
}

/* Decide what to hand the compositor for THIS frame and hand it: gui_flush()
 * (ad_finish() could not prove a smaller extent honest), nothing at all (a
 * real rect was computed and it is empty), or gui_flush_rect() of the union
 * this frame's diff actually found -- browser.c's redraw_page() three-way
 * dispatch, restated for this file's own ad_finish(). */
static void ad_flush(void)
{
    ad_finish();
    if (g_ad_result_valid == -1) { gui_flush(); return; }
    if (g_ad_result_valid == 0) return;
    for (int i = 0; i < g_ad_nr; i++) {
        int x = g_ad_r[i].x0, y = g_ad_r[i].y0, w = g_ad_r[i].x1 - x, h = g_ad_r[i].y1 - y;
#ifdef AUI_FLUSH_NEGCTL_SHRINK
        /* THE NEGATIVE CONTROL for this whole mechanism (make AUIFLUSHCTL=1;
         * tests/appflush.mk's test-appflush-negctl). Deliberately reports a
         * rectangle SMALLER than what ad_finish() actually computed -- 3px
         * inward on every side, floored at 1px so a thin change never collapses
         * to w<=0||h<=0 and gets read as "whole canvas" by SYS_GUI_FLUSH_RECT's
         * own contract (logit_abi.h says so explicitly; c/apps/logit.h's
         * gui_flush_rect comment repeats it). If the compositor is honouring the
         * rectangle this app reports rather than quietly recompositing more than
         * it was told, this MUST leave a visible border of stale pixels on
         * screen after any narrow-flushed edit -- watching that happen is what
         * earns the right to trust any table measured with this flag off
         * (AGENTS.md rule 5: a control that cannot be watched failing is worse
         * than no control). */
        x += 3; y += 3; w -= 6; h -= 6;
        if (w < 1) w = 1; if (h < 1) h = 1;
#endif
        gui_flush_rect(x, y, w, h);
    }
}


#include "aui_theme.inc"

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

#define AUI_PAINT_MASK
#include "aui_paint.inc"
#undef AUI_PAINT_MASK

/* --------------------------------------------- 5a. frame state (globals) */

static int id_ctr;
static int focus_id, focus_vis;
static int ox_, oy_;                          /* current translation, points */
static unsigned frame_ms;


/* clip stack. The kernel has one clip rect per surface, so aui keeps the stack
 * and pushes the intersection (the local rect also feeds clip_has() and the
 * scroll viewports). When the kernel reports LOGIT_GUI2_F_CLIP the level is
 * ALSO a kernel level (SYS_GUI_CLIP_PUSH, which intersects with what is in
 * force and, with a radius, clips to the rounded rect), and then it is the
 * ONLY thing that sets the kernel clip: a gui_clip() from inside such a level
 * would overwrite the rounded layer's clip. A level the kernel refused
 * (OL_LIMIT) falls back to gui_clip() like a kernel without the call. */
#define CLIP_MAX 16
static struct aui_rect clipst[CLIP_MAX];
static unsigned char clipv2[CLIP_MAX];      /* 1: this level is a kernel CLIP level */
static int clipn, clip_over;                /* clip_over: pushes refused for depth, popped silently */

/* GUI ABI v2 levels aui has open (clip and group, kernel order). */
static unsigned char cgs[32];
static int cgn;
enum { CG_CLIP = 1, CG_GROUP = 2 };
static unsigned cg_leaks;                   /* levels found open at a frame boundary */
static int cg_caps = -1;                    /* -1 not asked this frame */
static int cg_has(unsigned bits)
{
    if (cg_caps < 0) {
        long i = ol_gui2_info(0);
        cg_caps = i > 0 ? (int)(i & 0xFFFF) : 0;
    }
    return ((unsigned)cg_caps & bits) == bits;
}

static void clip_apply(void)
{
    if (!clipn) { gui_clip(0, 0, 0, 0); return; }
    struct aui_rect r = clipst[clipn - 1];
    if (r.w <= 0 || r.h <= 0) { gui_clip(0, 0, 1, 1); return; }
    gui_clip(r.x, r.y, r.w, r.h);
}
static struct aui_rect clip_intersect(struct aui_rect r)
{
    if (clipn > 0) {
        struct aui_rect p = clipst[clipn - 1];
        int x0 = imax(r.x, p.x), y0 = imax(r.y, p.y);
        int x1 = imin(r.x + r.w, p.x + p.w), y1 = imin(r.y + r.h, p.y + p.h);
        r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    }
    return r;
}
/* r: absolute window points; rad[4]: TL TR BR BL in points (all 0 = square). */
static void clip_push_r4(struct aui_rect r, const int rad[4])
{
    if (clipn >= CLIP_MAX) { clip_over++; return; }
    struct aui_rect lr = clip_intersect(r);
    int v2 = 0;
    if (cg_has(LOGIT_GUI2_F_CLIP) && cgn < 32 && r.w >= 0 && r.h >= 0) {
        struct logit_clip2 c;
        c.x = r.x * 256; c.y = r.y * 256; c.w = r.w * 256; c.h = r.h * 256;
        for (int k = 0; k < 4; k++) c.radius[k] = (rad[k] > 0 ? rad[k] : 0) * 256;
        c.flags = 0;
        if (ol_window_clip_push2(&c) == OL_OK) { v2 = 1; cgs[cgn++] = CG_CLIP; }
    }
    clipst[clipn] = lr;
    clipv2[clipn] = (unsigned char)v2;
    clipn++;
    if (!v2) clip_apply();
}
static void clip_push(struct aui_rect r)
{
    static const int sq[4] = { 0, 0, 0, 0 };
    clip_push_r4(r, sq);
}
static void clip_pop(void)
{
    if (clip_over) { clip_over--; return; }
    if (!clipn) { clip_apply(); return; }
    clipn--;
    if (clipv2[clipn]) {
        if (cgn > 0 && cgs[cgn - 1] == CG_CLIP) cgn--;
        ol_window_clip_pop2();
    } else clip_apply();
}

/* Opacity groups. aui_fade_begin() opens a kernel group at `alpha` when it can;
 * otherwise (no GROUP bit, OL_LIMIT, nesting past 8) the level is a LEGACY fade:
 * fade_leg carries the alpha the widget must apply itself by mixing colours (255
 * when a real group, or no fade, is in force), which is exactly what the widgets
 * did before groups existed. -DAUI_FADE_LEGACY_MIX forces that path: the
 * negative control of tests/aui-clip-group-adopt.mk. */
#define FADE_MAX 8
#define AUI_DISABLED_ALPHA 110          /* a disabled control's group opacity */
static struct { unsigned char kind; int prev_leg; } fadest[FADE_MAX];   /* kind 0 noop 1 group 2 legacy */
static int faden, fade_over;
static int fade_leg = 255;

/* Opens a fade over r (absolute window points). Returns 1 when a kernel group
 * was opened, 0 when the caller must mix (legacy) or nothing is needed. */
static int fade_open(struct aui_rect r, int alpha, int allow_legacy)
{
    if (alpha > 255) alpha = 255;
    if (alpha < 0) alpha = 0;
    if (faden >= FADE_MAX) { fade_over++; return 0; }
    int k = faden++;
    fadest[k].prev_leg = fade_leg;
    fadest[k].kind = 0;
    if (alpha >= 255) return 0;
#ifdef AUI_FADE_LEGACY_MIX
    (void)r;
#else
    if (cg_has(LOGIT_GUI2_F_GROUP) && cgn < 32 && r.w >= 0 && r.h >= 0) {
        struct logit_group2 g;
        g.x = r.x * 256; g.y = r.y * 256; g.w = r.w * 256; g.h = r.h * 256;
        g.radius[0] = g.radius[1] = g.radius[2] = g.radius[3] = 0;
        g.opacity = alpha;
        g.flags = 0;
        if (ol_window_group_begin2(&g) == OL_OK) {
            fadest[k].kind = 1;
            cgs[cgn++] = CG_GROUP;
            ad_note_group(r.x, r.y, r.w, r.h, alpha);
            return 1;
        }
    }
#endif
    if (allow_legacy) { fadest[k].kind = 2; fade_leg = fade_leg * alpha / 255; }
    return 0;
}
static void fade_close(void)
{
    if (fade_over) { fade_over--; return; }
    if (!faden) return;
    faden--;
    if (fadest[faden].kind == 1) {
        if (cgn > 0 && cgs[cgn - 1] == CG_GROUP) cgn--;
        ol_window_group_end2();
    }
    fade_leg = fadest[faden].prev_leg;
}

/* Close whatever a frame left open, innermost first (the kernel would do the
 * same at the flush and count it: SYS_GUI2_INFO a=1). `count`: it is a leak. */
static void cg_close_all(int count)
{
    if (count && (clipn || faden || clip_over || fade_over || cgn)) cg_leaks++;
    while (cgn > 0) {
        if (cgs[cgn - 1] == CG_GROUP) ol_window_group_end2(); else ol_window_clip_pop2();
        cgn--;
    }
    clipn = 0; clip_over = 0; faden = 0; fade_over = 0; fade_leg = 255;
}

static int clip_has(int x, int y)
{
    if (!clipn) return 1;
    struct aui_rect r = clipst[clipn - 1];
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* ---- widget-level aims (aui.h "aims", ui_aim.h; added by ui-aui-core) ----
 * aui_aim_prefix / aui_aim_names ARM the next aims-aware widget; the widget
 * takes the arming with aim_take() (which disarms it) and publishes each of
 * its items with aim_item() once its layout is final. Only window-local points
 * that the current clip and the window contain are published: an item scrolled
 * or clipped out of view has no aim, so a driver can never click it blind. */
#define AIM_PFX_MAX 20
#define AIM_ITEMS_MAX 32      /* items per widget that get an aim (tabs keep their centres) */
struct aim_ctx { char pfx[AIM_PFX_MAX]; const char *const *names; int n, on; };
static struct aim_ctx aim_arm;

void aui_aim_prefix(const char *p)
{
    int i = 0;
    for (; p && p[i] && i < AIM_PFX_MAX - 1; i++) aim_arm.pfx[i] = p[i];
    aim_arm.pfx[i] = 0;
    aim_arm.names = 0; aim_arm.n = 0;
    aim_arm.on = i > 0;
}
void aui_aim_names(const char *const *names, int n)
{
    aim_arm.pfx[0] = 0;
    aim_arm.names = names; aim_arm.n = n;
    aim_arm.on = names && n > 0;
}
/* The arming for THIS widget call into *c (disarmed for the next one). Off
 * unless the app has named itself. Field by field: a struct copy may lower to
 * a memcpy call, and aui.o links no libc. */
static void aim_take(struct aim_ctx *c)
{
    int i = 0;
    for (; aim_arm.pfx[i] && i < AIM_PFX_MAX - 1; i++) c->pfx[i] = aim_arm.pfx[i];
    c->pfx[i] = 0;
    c->names = aim_arm.names; c->n = aim_arm.n;
    c->on = aim_arm.on && ui_aim_on();
    aim_arm.on = 0;
}
static int aim_visible(int wx, int wy)
{
    return wx >= 0 && wy >= 0 && wx < win_w && wy < win_h && clip_has(wx, wy);
}
/* Item i of an armed widget at window-local (wx, wy). */
static void aim_item(const struct aim_ctx *c, int i, int wx, int wy)
{
    if (!c->on || !aim_visible(wx, wy)) return;
    if (c->names) { if (i >= 0 && i < c->n && c->names[i]) ui_aim(c->names[i], wx, wy); }
    else ui_aim_i(c->pfx, i, wx, wy);
}
/* A widget that is ONE control (select): the prefix itself, or names[0]. */
static void aim_one(const struct aim_ctx *c, int wx, int wy)
{
    if (!c->on || !aim_visible(wx, wy)) return;
    if (c->names) { if (c->names[0]) ui_aim(c->names[0], wx, wy); }
    else ui_aim(c->pfx, wx, wy);
}
/* The names aui publishes on its own (popup-<i>, dialog-*): no arming. */
static void aim_fixed_i(const char *pfx, int i, int wx, int wy)
{
    if (ui_aim_on() && aim_visible(wx, wy)) ui_aim_i(pfx, i, wx, wy);
}
static void aim_fixed(const char *name, int wx, int wy)
{
    if (ui_aim_on() && aim_visible(wx, wy)) ui_aim(name, wx, wy);
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
static unsigned pop_gen;      /* anim_gen of the frame that opened the popup */

static const char *tip_text;
static int tip_x, tip_y;
/* The tooltip fades, and it is the LAST-SHOWN content that fades, not the
 * current-frame request: `tip_text` is cleared every aui_begin() and only set
 * while the pointer is still dwelling, so the frame the pointer leaves is
 * exactly the frame the fade-out has to start from. draw_tip() runs
 * unconditionally every frame from aui_end() (see aui_end below), which is
 * what keeps this slot continuous through the whole fade in both directions --
 * unlike a dialog or a popup, whose OWNING call is itself conditional. */
static const char *tip_last_text;
static int tip_last_x, tip_last_y, tip_last_owner;

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

#include "aui_paint.inc"

/* ---- public clip / fade API (needs X_/Y_ from aui_paint.inc) ---- */
void aui_clip_pop(void) { clip_pop(); }
int aui_clip_push_round(struct aui_rect r, int radius)
{
    int rad[4];
    rad[0] = rad[1] = rad[2] = rad[3] = radius > 0 ? radius : 0;
    int before = cgn;
    clip_push_r4(aui_r(X_(r.x), Y_(r.y), r.w, r.h), rad);
    return cgn > before;                /* 1: a rounded kernel clip is in force */
}
void aui_fade_begin(struct aui_rect r, int alpha)
{ fade_open(aui_r(X_(r.x), Y_(r.y), r.w, r.h), alpha, 1); }
void aui_fade_end(void) { fade_close(); }

#include "aui_text.inc"

/* -------------------------------------------------- 5b. frame + input */

unsigned aui_ms(void) { return frame_ms; }

#include "aui_motion.inc"

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
    if (e->type == EV_THEME) {
        if (theme_override < 0) aui_set_dark(sys_ui_dark(-1) > 0);
        in.repaint = 1;
    }
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
/* `key_activates` is what separates a CONTROL from a TEXT INPUT, and it exists
 * because conflating them was a real bug with two visible symptoms.
 *
 * A button is activated by Enter or Space when it has focus, and wpoll below
 * implements that for every control by consuming the key. A text field is
 * focusable by the same mechanism, and it was getting the same treatment: the
 * `in.key_used = 1` fired before aui_textfield_ex's own key handler ran, so
 *   - Enter never reached it and it could not return 1 -- despite aui.h saying
 *     "Returns 1 on Enter", which is how Finder commits a rename
 *     (c/apps/gui/files.c:475) and how the chat window sends a prompt; and
 *   - SPACE never reached it either, so a space could not be typed into any
 *     text field in the system.
 *
 * Both were silent: the widget drew, the caret moved, letters appeared, and the
 * two keys simply did nothing. Found by c/apps/gui/ch.c, whose prompt field is
 * the first one in this tree whose Enter had to work for a test to pass. */
static struct wres wpoll_kb(int id, int x, int y, int w, int h, int enabled,
                            int focusable, int key_activates)
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
    if (key_activates && in.ev == EV_KEY && !in.key_used && focus_id == id && enabled &&
        (in.a == '\n' || in.a == ' ')) { r.clicked = 1; in.key_used = 1; }
    return r;
}

static struct wres wpoll(int id, int x, int y, int w, int h, int enabled, int focusable)
{
    return wpoll_kb(id, x, y, w, h, enabled, focusable, 1);
}

/* The colour a control's face should be in a given state. One place, so every
 * control in the system lights up by the same rule. */
/* Hover is a FAST cross-fade (colour, in place); press is deliberately
 * INSTANT -- the pressed face has to land on the frame that took the click or
 * the control reads as deaf, and it is the release that decays, not the
 * press. aui_anim() is called EVERY time this runs, active or not, so the
 * hover slot stays continuous across a press -- without that, the release
 * would find a stale slot and snap instead of decaying. */
static int hover_t(int st)
{
    return aui_anim(AUI_AK_FACE, (st & AUI_HOVER) ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
}

static unsigned face_for(int st)
{
    if (st & AUI_OFF) return AUI_DISABLED;
    int t = hover_t(st);
    if (st & AUI_ACTIVE) return AUI_FACE_ACTIVE;      /* instant: same frame as the click */
    return aui_mix(AUI_FACE, AUI_FACE_HOVER, t);
}

/* ---------------------------------------------------------------------------
 * Glass control faces (GUI ABI v2 SYS_GUI_GLASS2).
 *
 * Buttons, the segmented pill and the toggle track are ONE glass2 call with the
 * SYMMETRIC rim and the THIN blur whenever the kernel reports GLASS2 and the
 * user has not asked for reduced transparency (ui.reduce_transparency, read
 * once per frame in aui_begin). The DIRECTIONAL rim is right for the dock,
 * which refracts a wallpaper; on a control it paints two grey crescents (see
 * button_face), the SYMMETRIC rim lights every rim pixel alike. The face colour
 * the gradient used to carry is the material TINT, so hover, press and the
 * disabled tone still read. Without the capability, or with reduced
 * transparency, the gradient face is drawn exactly as before. There is no
 * setting of its own. -DAUI_GLASS_DIRECTIONAL (the negative control) asks for
 * the directional rim. */
#if defined(AUI_NO_AA)
#define glass_ctl_ok() 0
#else
static int glass_caps = -1;               /* -1 not asked yet, 0 no, 1 yes */
static int glass_reduced;                 /* ui.reduce_transparency, this frame */
static int glass_ctl_ok(void)
{
    if (glass_caps < 0) {
        long i = ol_gui2_info(0);
        glass_caps = (i > 0 && (i & LOGIT_GUI2_F_GLASS2)) ? 1 : 0;
    }
    return glass_caps && !glass_reduced;
}
#endif

#ifdef AUI_GLASS_DIRECTIONAL
#define GLASS_CTL_RIM OL_GLASS_RIM_DIRECTIONAL
#else
#define GLASS_CTL_RIM OL_GLASS_RIM_SYMMETRIC
#endif

/* The 「澄」 paint API (aui_material, aui_backdrop, aui_image, aui_elev): after
 * the probe above, which aui_material shares. */
#define AUI_PAINT_API2
#include "aui_paint.inc"
#undef AUI_PAINT_API2


/* Two concentric RINGS, never a filled halo: a filled rounded rect behind the
 * control is drawn over it by the control itself, and drawn after it washes the
 * control blue. Rings also survive on any background, which is the point -- the
 * ring has to read over a card, over glass and over an image.
 *
 * FADED, NOT POPPED, and that is why `focused` is a PARAMETER rather than a
 * caller-side `if`: the ring must be queried every frame the owning widget
 * draws, focused or not, or the slot goes stale the instant focus leaves and
 * the fade-out has nothing to animate from. Geometry never changes -- only the
 * mix fraction toward AUI_FOCUS -- so every corner tile this draws stays a
 * gfx mask-cache hit through the whole fade, in either direction. */
static void focus_ring(int focused, int x, int y, int w, int h, int r)
{
    int t = aui_anim(AUI_AK_FOCUS, (focused && focus_vis) ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    if (t <= 0) return;
    aui_stroke(x - 4, y - 4, w + 8, h + 8, r + 4, 2, aui_mix(AUI_BG, aui_mix(AUI_BG, AUI_FOCUS, 110), t));
    aui_stroke(x - 2, y - 2, w + 4, h + 4, r + 2, 2, aui_mix(AUI_BG, AUI_FOCUS, t));
}

void aui_begin(unsigned bg)
{
    int s = theme_override < 0 ? sys_ui_dark(-1) > 0 : theme_override;
    if (!theme_inited || s != theme_dark) { aui_set_dark(s); bg = aui_t.bg; }
    aui_ensure();
    frame_ms = (unsigned)monotonic_ms();
    anim_reduced = setting_int("ui.reduce_motion",0) != 0;
#ifndef AUI_NO_AA
    glass_reduced = setting_int("ui.reduce_transparency",0) != 0;
#endif
    /* Last frame's clipped rectangles pick the innermost eligible scroller.
     * Both ancestors and descendants used to consume the same wheel event.
     * Resolve before any slot is touched in this frame; no live state pointer
     * is dereferenced during hit testing. New containers use first-hit fallback.
     */
    scroll_wheel_owner = 0; scroll_wheel_used = 0; scroll_order = 0;
    if (in.ev == EV_WHEEL) {
        unsigned last = 0;
        for (int i = 0; i < SCROLL_SLOTS; i++) {
            struct scroll_slot *s = &scroll_slots[i];
            if (s->owner && s->gen == anim_gen && s->motion.limit > 0 &&
                aui_hit(s->viewport, in.mx, in.my) && s->order >= last) {
                scroll_wheel_owner = s->owner; last = s->order;
            }
        }
    }
    /* The animation frame counter. It is what the continuity rule in section 5c
     * compares against, so it advances here -- once per DRAWN frame -- and not
     * on a clock. `anim_live` and `anim_loop_want` are re-derived by the widgets
     * about to run, so anything that stops being drawn stops being scheduled
     * for free. */
    anim_gen++;
    anim_live = 0; anim_loop_want = 0;
    if (anim_armed) { anim_frames_n++; anim_armed = 0; }
    id_ctr = 0; foc_n = 0; clipn = 0; clip_over = 0; ox_ = oy_ = 0;
    in_dialog = 0; in_popup = 0; dlg_open_now = 0;
    in.hot = 0;
    aim_arm.on = 0;                     /* an arming no widget took dies with its frame */
    wbb_next = aui_r(0, 0, 0, 0);
    tip_text = 0;
    /* Reset THIS frame's flush-diff recording (section 5a-flush). g_ad_prev /
     * g_ad_prev_n are last frame's data and must survive into this frame's
     * ad_finish() -- only the CUR side resets. */
    g_ad_cur_n = 0; g_ad_gcur_n = 0; g_ad_overflow = 0; g_ad_extra_have = 0;
    cg_close_all(0);
    cg_caps = -1;                       /* the kernel's answer is asked once per frame */
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
    /* refuse: gfx_mask_corner() turned a tile down outright this session
     * (gfx_mask.c's mrefuse, via the local extern above -- not per-interval
     * like the others, because a refusal is rare enough that a running total
     * is more useful than a delta that reads 0 almost every print). shdeg:
     * how many of THIS app's shadow requests needed aui_shadow_ex's tier-3
     * blur clamp. Both used to be unknowable from outside gdb; now they are
     * two more fields in the same line this bench already prints. */
    char b[220]; int q = 0;
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
    PUT(" refuse=");   NUM((unsigned)gfx_mask_refused());
    PUT(" shdeg=");    NUM(shadow_degraded);
    b[q++] = '\n';
    #undef PUT
    #undef NUM
    sys_write(1, b, q);
    ck_clear = ck_text = ck_shape = ck_other = ck_frames = ck_wall = 0;
}
#endif

/* Shared tail of aui_end()/aui_end_rect(): popups and tooltips are drawn LAST
 * (see the comment on draw_popup's forward declaration above) so they sit
 * over everything, then the frame's bookkeeping (modal state, the widget
 * bounding box the NEXT frame's input tests against, the radio range) is
 * committed. Neither the flush decision nor anim_schedule() lives here --
 * the two callers below need different things done between "the frame is
 * fully drawn" and "hand it to the compositor". */
static void ae_common(void)
{
    cg_close_all(1);                       /* a frame must balance its levels */
    ox_ = oy_ = 0; clipn = 0; gui_clip(0, 0, 0, 0);
    in_popup = 1;
    draw_popup();
    in_popup = 0;
    draw_tip();
    modal_prev = dlg_open_now;
    wbb = wbb_next; wbb_any = wbb.w > 0;
    rg_lo = rg_lo_a; rg_hi = rg_hi_a;      /* the radio range this frame observed */
}

void aui_end(void)
{
    ae_common();
    ad_flush();
    /* AFTER the flush, deliberately: see anim_schedule(). This is also the only
     * place the deadline is set, so an app that never calls aui_end() -- there
     * is none -- would simply never animate rather than animate wrongly. */
    anim_schedule();
#ifdef AUI_COST
    ck_wall += monotonic_ns() - ck_fstart;
    ck_report();
#endif
}

/* aui_end() for a caller that has ALREADY computed its own damage rect this
 * frame -- textedit.c's line/cursor/wrap-cascade math, terminal.c would use
 * the equivalent if it linked this file (it does not; see terminal.c's own
 * gui_flush_rect call). (x,y,w,h) is UNIONED with whatever this frame's own
 * primitive-level diff found, never SUBSTITUTED for it: aui's own tracked
 * drawing (a status bar redrawn via aui_fill/aui_text_ellipsis, say) must
 * still be accounted, or a caller's narrower hint would silently swallow it.
 * w<=0||h<=0 means "no hint this frame" -- identical to aui_end() -- so a
 * caller unsure whether anything narrower is provable can always pass
 * (0,0,0,0) and get exactly aui_end()'s behaviour. */
void aui_end_rect(int x, int y, int w, int h)
{
    ae_common();
    if (w > 0 && h > 0) {
        g_ad_extra_have = 1;
        g_ad_ex0 = x; g_ad_ey0 = y; g_ad_ex1 = x + w; g_ad_ey1 = y + h;
    }
    ad_flush();
    anim_schedule();
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

/* The popup row height IS the list row height (aui_select and aui_menubar open
 * their lists at it), so it is shared by aui_button.inc, aui_lists.inc and
 * aui_overlay.inc and lives here, ahead of all three. */
#define ROWH 26

#include "aui_button.inc"
#include "aui_controls.inc"
#include "aui_fields.inc"
#include "aui_lists.inc"
#include "aui_overlay.inc"
#include "aui_nav.inc"
#include "aui_form.inc"
#include "aui_data.inc"
