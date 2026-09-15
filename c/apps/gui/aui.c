#include "openlogit_bitmap.h"
#include "aui.h"
#include "gfx.h"
#include "openlogit_anim.h"
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
 * THE METHOD IS THE SAME ONE c/apps/browser/browser_paint.c's pd_finish()
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
 * pixels of tightness. Padding vertically by a quarter of the point size
 * (floor 4px) and running the box to the window's own right edge -- rather
 * than trying to also learn the string's rendered WIDTH here -- is the
 * WIDEN-BEFORE-YOU-NARROW answer: always safe, costs nothing extra to
 * compute, and is clamped back down to whatever the real union turns out to
 * be by ad_finish()'s viewport clamp below regardless. */
static void ad_note_text(int x, int y, int px, int mono, unsigned color, const char *s, int len)
{
    unsigned h = 2166136261u; AD_MIX(h, px); AD_MIX(h, mono); AD_MIX(h, color); AD_MIX(h, len);
    if (s && len > 0) h = ad_hash_bytes(s, len, h);
    int pad = px / 4; if (pad < 4) pad = 4;
    int w = win_w - x; if (w < 1) w = 1;
    ad_record(x, y - pad, w, px + 2 * pad, h);
}

static void ad_union(int *dx0, int *dy0, int *dx1, int *dy1, int *any,
                     int x0, int y0, int x1, int y1)
{
    if (!*any) { *dx0 = x0; *dy0 = y0; *dx1 = x1; *dy1 = y1; *any = 1; return; }
    if (x0 < *dx0) *dx0 = x0; if (y0 < *dy0) *dy0 = y0;
    if (x1 > *dx1) *dx1 = x1; if (y1 > *dy1) *dy1 = y1;
}

/* -1: no honest diff (first frame, resize, or this frame's array overflowed)
 * -- caller must gui_flush() the whole canvas. 0: a rect was computed and it
 * is EMPTY -- nothing to flush at all. 1: g_ad_r{x,y,w,h} holds a real,
 * nonempty, canvas-clamped rect. Same three-way contract as
 * browser_paint_dirty_rect(), deliberately -- one jar, and now two doors that
 * agree on its shape instead of two that could drift. */
static int g_ad_result_valid;
static int g_ad_rx, g_ad_ry, g_ad_rw, g_ad_rh;

static void ad_finish(void)
{
    int can_diff = g_ad_have_prev && !g_ad_overflow &&
                   g_ad_prev_w == win_w && g_ad_prev_h == win_h;
    int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0, any = 0;
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
                ad_union(&dx0, &dy0, &dx1, &dy1, &any, ux0, uy0, ux1, uy1);
            } else {
                const struct ad_ent *e = inc ? &g_ad_cur[i] : &g_ad_prev[i];
                ad_union(&dx0, &dy0, &dx1, &dy1, &any, e->x, e->y, e->x + e->w, e->y + e->h);
            }
        }
    }
    if (g_ad_extra_have)
        ad_union(&dx0, &dy0, &dx1, &dy1, &any, g_ad_ex0, g_ad_ey0, g_ad_ex1, g_ad_ey1);

    if (!can_diff) {
        g_ad_result_valid = -1;
    } else if (!any) {
        g_ad_result_valid = 0;
    } else {
        if (dx0 < 0) dx0 = 0; if (dy0 < 0) dy0 = 0;
        if (dx1 > win_w) dx1 = win_w; if (dy1 > win_h) dy1 = win_h;
        if (dx1 <= dx0 || dy1 <= dy0) g_ad_result_valid = 0;
        else { g_ad_result_valid = 1; g_ad_rx = dx0; g_ad_ry = dy0; g_ad_rw = dx1 - dx0; g_ad_rh = dy1 - dy0; }
    }
    /* Pointer swap, not a copy -- the browser's pd_finish() does the same
     * with realloc'd buffers; these are fixed, so the swap is simpler still
     * and (see the section-open comment on why no libc) sidesteps ever
     * needing a memcpy-shaped loop that -O2's loop-idiom pass could rewrite
     * into an actual memcpy() call this file cannot link. */
    struct ad_ent *t = g_ad_prev; g_ad_prev = g_ad_cur; g_ad_cur = t;
    g_ad_prev_n = g_ad_cur_n; g_ad_cur_n = 0;
    g_ad_prev_w = win_w; g_ad_prev_h = win_h;
    g_ad_have_prev = 1;
    g_ad_overflow = 0;
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
    int x = g_ad_rx, y = g_ad_ry, w = g_ad_rw, h = g_ad_rh;
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


/* ---------------------------------------------------------------- 2. theme */

static int theme_dark, theme_inited;
static int theme_override = -1;
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
void aui_theme_override(int mode)
{
    theme_override = mode < 0 ? -1 : !!mode;
    aui_set_dark(theme_override < 0 ? sys_ui_dark(-1) > 0 : theme_override);
}

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

/* Blit a coverage tile into the point rect (x,y,w,h), optionally mirrored, out
 * of a CALLER-CHOSEN scratch buffer. Parameterized on the buffer (rather than
 * always writing rgba_buf) because the uncached path below needs a second,
 * bigger buffer, and every widget that goes through the small one should not
 * pay for the big one's size -- see the BIG_MASK block for the buffer itself.
 * The source is generated at the rect's exact device size, so the kernel's
 * rescale is a no-op and the anti-aliasing survives at any backing scale. One
 * rasterized quadrant serves all four corners, which is what the mirroring is
 * for. */
static void blit_mask_buf(unsigned char *rbuf, long rcap, int x, int y, int w, int h,
                          const unsigned char *cov, int cw, int ch, unsigned color,
                          int alpha, int fx, int fy)
{
    if (!cov || cw <= 0 || ch <= 0 || w <= 0 || h <= 0) return;
    if ((long)cw * ch * 4 > rcap) return;
    gfx_mask_to_rgba(rbuf, cov, cw, ch, color, alpha, fx, fy);
    gui_blit(x, y, w, h, rbuf, cw, ch);
}
/* NOTE: there is deliberately no plain `blit_mask(...)` wrapper pinned to
 * rgba_buf any more. Every call site below now goes through corner_mask()
 * first, which hands back the RIGHT buffer (rgba_buf for a cached tile,
 * big_rgba for an uncached one) -- so every blit already has to know which
 * one it got, and a fixed-to-rgba_buf wrapper would just be a second, unused
 * name for the same call. */

/* ---- the uncached escape hatch ----
 *
 * gfx_mask_corner() REFUSES -- returns NULL, never a truncated or wrong mask
 * -- once a tile's device geometry passes GFX_MASK_MAX (72 px); see the
 * contract comment on it in gfx_mask.c, which also names the three honest
 * responses a caller has. This toolkit used to have exactly ONE of the three
 * (round_impl's square fallback, tier 3 below) and, worse, two call sites
 * that checked nothing at all -- see aui_stroke and aui_vgrad_round's own
 * comments for what that produced. This block is tier 2, the one that keeps
 * the actual requested shape instead of a worse one: gfx_corner_fill/
 * gfx_corner_ring/gfx_corner_shadow (aliased above as raster_*_corner) are
 * exactly what gfx_mask_corner calls internally to FILL the cache, and they
 * are public so a caller can call them directly into its OWN buffer when the
 * cache's fixed-size pool is the wrong shape for this one request -- "the
 * cache is a cache; nothing forces geometry through it."
 *
 * BIG_MASK is a real ceiling, not a promotion to "unlimited": this buffer is
 * a static too, paid by every app that links aui.c whether it ever needs this
 * path or not, so it stays a deliberate, bounded step up from GFX_MASK_MAX
 * rather than sized to the largest thing anyone could ask for. 256 device px
 * was chosen against the ONE thing that actually bounds it -- not display
 * size, not a guess: gfx_mask.c's corner generators share a single 512-point,
 * 8-subpath scratch path (`cpt`/`csub` in that file) for EVERY call
 * regardless of the tile size they are asked to fill, and that scratch is not
 * this file's to resize (it is also kernel .bss, via fb.c -- see gfx.h's
 * LIMITS section). A stroke corner (gfx_corner_ring, the worst case: two
 * nested arcs in one path) at 256 device px measured 260 of those 512 points
 * -- and, verified by walking the same construction across sizes rather than
 * guessing the slope from one sample, that count does not move off 260 again
 * until past 700 px; it does not reach the 512 ceiling (gfx_corner_ring's
 * overflow -> a silently blank tile, the failure mode below) until 913 px.
 * 256 leaves better than 3.5x headroom, not merely "over 2x" -- against a
 * failure mode this file could not even detect if it happened:
 * gfx_corner_fill/gfx_corner_ring both discard gfx_fill_mask's success/
 * refuse return value internally, so an actual overflow there would hand
 * back a silently BLANK tile, not a NULL this file could check for. Measured,
 * not round, for that reason -- c/lib/gfx is a different unit's file, so the
 * only lever available here is staying well inside what it already provably
 * handles rather than asking it to grow. */
#define BIG_MASK 256
static unsigned char big_cov[BIG_MASK * BIG_MASK];
static unsigned char big_rgba[BIG_MASK * BIG_MASK * 4];

/* gfx_mask.c's refusal counter (mrefuse, incremented at the exact point
 * gfx_mask_corner returns NULL) is not declared in gfx.h -- see the comment
 * on gfx_mask_refused() in gfx_mask.c for why -- so a caller in this
 * translation unit reaches it with its own one-line extern, the sound thing
 * to do across a TU boundary for a symbol that does exist and is exported.
 * ck_report() below reads it so a bench run can show "N tile requests could
 * not be served at all this session", which used to be a number nothing
 * could produce. */
extern int gfx_mask_refused(void);

/* THE SHARED DECISION every fixed call site below now makes, instead of each
 * accepting gfx_mask_corner's NULL as final in its own slightly different
 * (and, twice, badly wrong) way: try the cache, then the uncached BIG_MASK
 * tile, and only report "nothing" if NEITHER fits. `kind`/`param` are the
 * same vocabulary mask_get() already takes (MK_FILL/MK_STROKE/MK_SHADOW).
 * Hands back which scratch RGBA buffer the mask is good against via the
 * rbuf/rcap out-params, because a cached tile's data lives in `mdata` (bounded by
 * rgba_buf, MASK_MAX) and an uncached one lives in `big_cov` (bounded by
 * big_rgba, BIG_MASK) -- blit_mask_buf() needs to know which ceiling applies
 * or it silently refuses to blit a big tile through the small buffer's size
 * check.
 *
 * ONE MORE REASON this is a function and not four inlined copies:
 * tests/unit/aui_mask_test.c can call it directly, with no gui_* syscall
 * anywhere near it (see that file's own note on why no syscall in that TU is
 * ever allowed to fire). That is what makes aui_stroke's fix -- "past the
 * ceiling, try the uncached ring before giving up the corners" -- provable
 * from a host test rather than only from a screenshot: the test calls this
 * exact function with an oversized stroke radius and asserts what comes back
 * is a real, continuous ring, not NULL. */
static const unsigned char *corner_mask(int kind, int cw, int ch, int param,
                                        unsigned char **rbuf, long *rcap)
{
    const unsigned char *m = mask_get(kind, cw, ch, param);
    if (m) { *rbuf = rgba_buf; *rcap = (long)sizeof rgba_buf; return m; }
    if (cw <= 0 || ch <= 0 || cw > BIG_MASK || ch > BIG_MASK) return 0;
    if (kind == MK_FILL)        raster_fill_corner(big_cov, cw, ch);
    else if (kind == MK_STROKE) raster_stroke_corner(big_cov, cw, ch, param);
    else                        raster_shadow_corner(big_cov, cw, ch, param);
    *rbuf = big_rgba; *rcap = (long)sizeof big_rgba;
    return big_cov;
}

/* --------------------------------------------- 5a. frame state (globals) */

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
static void clip_push(struct aui_rect r)
{
    r = clip_intersect(r);
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
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_FILL, cw, ch, 0, &rbuf, &rcap);
    if (!m) {
        /* Past BOTH the cache's ceiling and the uncached one. THE BUG THIS
         * WRITE-UP IS ABOUT: falling straight to a square used to be the
         * ONLY response, which is what let a 180-point clock face ask for a
         * 90-point corner and get a SQUARE clock, silently, correctly by
         * this function's own old contract, and wrong -- clock.c:138-148 is
         * the write-up, from the caller's side, of having to route around
         * this by hand. corner_mask() tries the uncached tile first now
         * (see the BIG_MASK block above), so this square is reached only
         * past BIG_MASK -- and it is the acceptable half of gfx_mask.c's
         * contract even then: COMPLETE (no missing pixels), and, because
         * gfx_mask_corner already counted the refusal that got us here, no
         * longer silent either. */
        aui_fill_a(x, y, w, h, c, alpha);
        return;
    }
    /* interior: three opaque bands, the kernel's fast path */
    aui_fill_a(x + r, y,         w - 2 * r, r,         c, alpha);
    aui_fill_a(x,     y + r,     w,         h - 2 * r, c, alpha);
    aui_fill_a(x + r, y + h - r, w - 2 * r, r,         c, alpha);
    /* four corners, one rasterized quadrant mirrored into place */
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y),         r, r, m, cw, ch, c, alpha, 0, 0);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y),         r, r, m, cw, ch, c, alpha, 1, 0);
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y + h - r), r, r, m, cw, ch, c, alpha, 0, 1);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y + h - r), r, r, m, cw, ch, c, alpha, 1, 1);
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
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_STROKE, cw, ch, td, &rbuf, &rcap);
    if (!m) {
        /* THE WORST OFFENDER IN THIS FILE, before this fix (see gfx_mask.c's
         * contract comment on gfx_mask_corner for the taxonomy). There used
         * to be NO check here at all: a radius past GFX_MASK_MAX went
         * straight into blit_mask with a NULL cov, which silently no-ops on
         * a NULL source -- so the four straight edges below still drew and
         * the four corners just never arrived. That is not "a plausible
         * wrong picture", it is a DIFFERENT, perfectly complete-looking
         * shape (a plain square outline where a rounded one was asked for),
         * which is exactly the "drops geometry" case that same contract
         * comment says is never acceptable -- unlike round_impl's square,
         * there is no honest degraded version of THIS shape with the
         * corners missing. corner_mask() tries the uncached ring first now;
         * only past BOTH ceilings does this still fall back to an actual
         * square ring (the r==0 shape above), which is complete and, like
         * every gfx_mask_corner refusal, already counted.
         * tests/unit/aui_mask_test.c's "aui_stroke past the ceiling" case
         * calls corner_mask() with exactly this kind at an oversized radius
         * and asserts it comes back a continuous ring rather than NULL --
         * which is what proves this branch is reached only when it truly
         * has to be. */
        aui_fill(x, y, w, t, c); aui_fill(x, y + h - t, w, t, c);
        aui_fill(x, y + t, t, h - 2 * t, c); aui_fill(x + w - t, y + t, t, h - 2 * t, c);
        return;
    }
    aui_fill(x + r, y,         w - 2 * r, t, c);
    aui_fill(x + r, y + h - t, w - 2 * r, t, c);
    aui_fill(x,         y + r, t, h - 2 * r, c);
    aui_fill(x + w - t, y + r, t, h - 2 * r, c);
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y),         r, r, m, cw, ch, c, 255, 0, 0);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y),         r, r, m, cw, ch, c, 255, 1, 0);
    blit_mask_buf(rbuf, rcap, X_(x),         Y_(y + h - r), r, r, m, cw, ch, c, 255, 0, 1);
    blit_mask_buf(rbuf, rcap, X_(x + w - r), Y_(y + h - r), r, r, m, cw, ch, c, 255, 1, 1);
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

/* One gradient corner: the mask's shape, each row tinted by its own position
 * in the vertical ramp so the curve does not shear away from the band it
 * abuts. Factored out of aui_vgrad_round so the cached tile (rgba_buf) and
 * the uncached one (big_rgba, see the BIG_MASK block above) share the one
 * copy of this loop instead of drifting apart. */
static void vgrad_corner(unsigned char *rbuf, long rcap, int devx, int devy, int r,
                         const unsigned char *m, int cw, int ch, int fx, int fy,
                         unsigned top, unsigned bot, int hd)
{
    if ((long)cw * ch * 4 > rcap) return;
    for (int j = 0; j < ch; j++) {
        int sj = fy ? ch - 1 - j : j;
        int grow = fy ? hd - ch + j : j;
        unsigned c = aui_mix(top, bot, iclamp(grow * 255 / hd, 0, 255));
        unsigned char *d = rbuf + (long)j * cw * 4;
        const unsigned char *s = m + (long)sj * cw;
        ol_bitmap_tint_row(d,s,cw,c,255,fx);
    }
    gui_blit(devx, devy, r, r, rbuf, cw, ch);
}

void aui_vgrad_round(int x, int y, int w, int h, int r, unsigned top, unsigned bot)
{
    if (w <= 0 || h <= 0) return;
    r = clamp_radius(w, h, r);
    if (r == 0) { aui_vgrad(x, y, w, h, top, bot); return; }

    /* Decide the corner source BEFORE drawing anything. The old order drew
     * the three bands FIRST and only then asked for the corners, so a
     * refusal's `if (!m) return;` left the four r x r corner squares as pure,
     * untouched background -- not a square corner, an actual HOLE in the
     * shape (gfx_mask.c's contract comment calls this out by name: the same
     * "drops geometry" failure as aui_stroke's vanishing arcs, just a gap
     * instead of a missing outline). Deciding first is what makes a clean
     * whole-rect fallback possible instead of a hole. */
    int cw = devlen(X_(x), r), ch = devlen(Y_(y), r);
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_FILL, cw, ch, 0, &rbuf, &rcap);
    if (!m) {
        /* Past both ceilings: a flat-cornered gradient over the WHOLE rect is
         * complete (no hole, no seam at the band edge) -- the acceptable
         * half of the contract, and, like every gfx_mask_corner refusal,
         * already counted there. */
        aui_vgrad(x, y, w, h, top, bot);
        return;
    }

    aui_vgrad(x + r, y,         w - 2 * r, r,         top, aui_mix(top, bot, r * 255 / h));
    aui_vgrad(x,     y + r,     w,         h - 2 * r, aui_mix(top, bot, r * 255 / h),
                                                      aui_mix(top, bot, (h - r) * 255 / h));
    aui_vgrad(x + r, y + h - r, w - 2 * r, r,         aui_mix(top, bot, (h - r) * 255 / h), bot);
    /* The corners take the gradient's colour at their own row, so the curve does
     * not shear away from the band it abuts. */
    int hd = imax(1, devlen(Y_(y), h));
    for (int corner = 0; corner < 4; corner++) {
        int fx = corner & 1, fy = corner >> 1;
        int px = fx ? x + w - r : x, py = fy ? y + h - r : y;
        vgrad_corner(rbuf, rcap, X_(px), Y_(py), r, m, cw, ch, fx, fy, top, bot, hd);
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

/* How many shadow requests this session had to shrink `blur` to fit even the
 * uncached tile (BIG_MASK) -- see aui_shadow_ex's tier 3 below. A count, not
 * a bool, and read the same way gfx_mask_refused() is: nothing else makes
 * "shadows are coming out tighter than CSS asked for" visible, and fb.c's
 * fb_shadow has the identical silent clamp today (see its own comment) --
 * this is the one half of that bug this unit can actually fix, since fb.c is
 * kernel code and cannot afford this file's BIG_MASK buffers (see fb.c). */
static unsigned shadow_degraded;

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
    unsigned char *rbuf; long rcap;
    const unsigned char *m = corner_mask(MK_SHADOW, cw, ch, rd, &rbuf, &rcap);
    if (!m) {
        /* Old code: `if (m) { four blits }` with NOTHING gating the edge
         * strips below, which draw unconditionally -- so a refused corner
         * left the edges with no corner to meet, exactly the seam gfx.h's
         * clip-mask note warns a mismatched pair produces. corner_mask()
         * covers tier 1 (cached) and tier 2 (uncached, AT THE REQUESTED
         * blur -- no clamp) above; reaching here means BOTH refused, and
         * only then does tier 3 shrink blur -- BEFORE recomputing T/cw/ch,
         * so the edge strips further down (which key off the same `blur`
         * variable) automatically fall off over the same, now-shorter,
         * distance and nothing seams.
         *
         * `blur`/`r` are POINTS here (aui_shadow_ex's whole interface is),
         * but BIG_MASK is a DEVICE bound -- the same point/device split
         * clock.c:126 works around with `MASK_MAX * 100 / aui_scale()`.
         * Same idiom: convert the device ceiling back to a point span
         * first, THEN shrink blur in the units it is actually expressed
         * in. */
        shadow_degraded++;
        int capT = BIG_MASK * 100 / aui_scale();
        blur = capT - r;
        if (blur <= 0) return;      /* radius alone doesn't fit even the device cap: no sound shadow to draw */
        T = blur + r;
        cw = devlen(sx - blur, T); ch = devlen(sy - blur, T);
        /* devlen rounds; clamp hard rather than trust the arithmetic -- this
         * feeds corner_mask()'s own BIG_MASK check, and a byte over would
         * just be refused again (safe, but pointless) rather than overflow
         * anything, since corner_mask never writes past what it checked. */
        if (cw > BIG_MASK) cw = BIG_MASK;
        if (ch > BIG_MASK) ch = BIG_MASK;
        rd = imin(aui_dev(r), cw - 1);
        if (rd < 0) rd = 0;
        m = corner_mask(MK_SHADOW, cw, ch, rd, &rbuf, &rcap);
        if (!m) return;    /* shouldn't happen -- cw,ch are now <= BIG_MASK by construction -- but never draw on an unmet assumption */
    }
    blit_mask_buf(rbuf, rcap, sx - blur,     sy - blur,     T, T, m, cw, ch, col, alpha, 0, 0);
    blit_mask_buf(rbuf, rcap, sx + w - r,    sy - blur,     T, T, m, cw, ch, col, alpha, 1, 0);
    blit_mask_buf(rbuf, rcap, sx - blur,     sy + h - r,    T, T, m, cw, ch, col, alpha, 0, 1);
    blit_mask_buf(rbuf, rcap, sx + w - r,    sy + h - r,    T, T, m, cw, ch, col, alpha, 1, 1);
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
/* A SLICE of a buffer, not a NUL-terminated string. Every text view that
 * scrolls has this shape -- one big buffer, a table of (offset, length) lines,
 * and only the visible ones drawn -- and until this existed the only way to
 * draw one was to call gui_text_run directly. That is a trap, because
 * gui_text_run takes WINDOW coordinates and every aui text call takes
 * CONTAINER ones: inside aui_scroll_begin the two differ by the container's
 * origin AND by the scroll offset, so the labels moved with the scroll and the
 * text did not. The symptom was a transcript whose role headings sat two rows
 * away from the text they belonged to, which reads as a wrapping bug and is
 * not one. Found by c/apps/gui/ch.c, the first scrolling text view in the tree
 * that is not the browser's own painter. */
void aui_text_n(int x, int y, const char *s, int len, unsigned color, int px)
{ if (len > 0) gui_text_run(X_(x), Y_(y), px, 0, color, s, len); }
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

/* ---------------------------------------------------- 5c. motion ----------
 * The animation core. aui.h carries the design; this carries the three things
 * that are only decidable in code.
 *
 * ONE. IDENTITY IN AN IMMEDIATE-MODE TOOLKIT. There are no widget objects, so
 * "the button that was 40% hovered last frame" has to be recovered from
 * something. It is recovered from the SAME THING FOCUS ALREADY USES: the widget
 * id, which is call order (`++id_ctr` at the top of every widget). During a
 * widget's body, `id_ctr` IS that widget's id, so aui_anim() needs no argument
 * for it and no registration step. This is not a new bet -- aui.h has said
 * "focus order IS call order" since Tab worked -- but it is a bet, and the two
 * rules below are what make it safe rather than merely conventional.
 *
 * TWO. THE CONTINUITY RULE, which is what stops a stuck or flickering state.
 * A slot carries the FRAME NUMBER it was last queried at. If that is not the
 * immediately preceding frame, the slot is treated as fresh: it latches to its
 * target and does not animate. Two things fall out, both wanted. A widget
 * appearing for the first time appears FINISHED -- open a dialog with thirty
 * controls and you get one frame, not thirty hover fades. And an evicted or
 * abandoned slot can never resume mid-flight holding a value that belonged to
 * something else. It is a frame counter and deliberately not a clock: an app
 * that sleeps ten seconds and then repaints is still "consecutive", which is
 * right, because an immediate-mode frame is a pure function of state and the
 * passage of time did not alter it.
 *
 * When the caller's layout DOES change so that a widget inherits an id, that
 * slot is continuous and re-aims from the previous occupant's value. The result
 * is one bounded cross-fade of the wrong quantity, over at most `ms`. It cannot
 * stick (arrival is `elapsed >= dur`, not a threshold on the value) and it
 * cannot flicker (re-aim is from the CURRENT value, so the sequence of drawn
 * values is continuous by construction). aui_anim_reset() is the escape hatch,
 * and it is the same escape hatch, for the same reason, as aui_set_focus().
 *
 * THREE. THE WAKE CONTRACT, and the property to protect is that an idle
 * desktop costs exactly what it cost before this file grew a clock. Both
 * public queries return from a static and READ NO CLOCK when nothing is
 * animating -- monotonic_ms() is a syscall on this machine, and a toolkit that
 * called it once per loop turn would be reintroducing, in a smaller way,
 * exactly the 3.3-million-syscall mistake the top of aui.h is about. */

struct anim_slot {
    int      id;        /* owning widget id; 0 = never used                 */
    int      key;       /* AUI_AK_*: several animations on one widget       */
    int      from, to;  /* endpoints, 0..255 (aui_mix's domain)             */
    int      cur;       /* what the last frame actually drew                */
    int      dur;       /* ms                                               */
    int      curve;     /* AUI_EASE_*                                       */
    unsigned t0;        /* frame_ms when this leg started                   */
    unsigned gen;       /* frame number this slot was last queried at       */
    int      arrived;   /* latched: returns `to` and schedules nothing      */
};

static struct anim_slot anim_tab[AUI_ANIM_MAX];
static int anim_reduced;
static unsigned anim_gen = 1;      /* frames since process start; aui_begin++ */
static unsigned anim_frames_n;     /* frames drawn because a deadline fired   */
static unsigned anim_due_ms;       /* absolute deadline, ms                   */
static int      anim_have_due;     /* ...and whether there is one at all      */
static int      anim_loop_want;    /* an ENDLESS animation drew this frame    */
static int      anim_live;         /* slots still in flight after this frame  */
static int      anim_armed;        /* aui_anim_due() said yes, frame pending  */

/* Offset pointers are the existing scroll-container identity. Using widget
 * call order here would alias slots when a list culls a different set of rows.
 * Keep only borrowed identities: an absent container's pointer is never read.
 */
#define SCROLL_SLOTS 16
static struct scroll_slot {
    int *owner;
    struct aui_scroll_motion motion;
    struct aui_rect viewport;
    unsigned gen, order;
} scroll_slots[SCROLL_SLOTS];
static unsigned scroll_order;
static int *scroll_wheel_owner;
static int scroll_wheel_used;

#ifndef AUI_ANIM_OFF          /* unreferenced in the negative-control build */
static struct anim_slot *anim_slot_for(int id, int key, int *fresh)
{
    struct anim_slot *freeslot = 0, *lru = &anim_tab[0];
    /* THE WHOLE TABLE IS SCANNED BEFORE ANYTHING IS ALLOCATED, and the first
     * version of this function did not do that: it took the first free slot and
     * broke out of the loop. With a hole at index 2 and this widget's live slot
     * at index 5, that allocated a NEW slot at 2 and orphaned 5 -- so the widget
     * restarted from its target every frame and never moved, while a second slot
     * quietly ate a table entry. It would have looked exactly like "the
     * animation does not work", which is the least informative symptom there is. */
    for (int i = 0; i < AUI_ANIM_MAX; i++) {
        struct anim_slot *s = &anim_tab[i];
        if (s->id == id && s->key == key) {
            /* Found. Continuous only if it was queried in the frame directly
             * before this one -- see THE CONTINUITY RULE above. */
            *fresh = (s->gen + 1 != anim_gen);
            return s;
        }
        if (!s->id) { if (!freeslot) freeslot = s; continue; }
        /* Least-recently-touched. Unsigned compare is fine: gen is monotone
         * and the table is scanned within one frame, so no two live entries
         * can straddle a wrap. */
        if (s->gen < lru->gen) lru = s;
    }
    struct anim_slot *n = freeslot ? freeslot : lru;
    n->id = id; n->key = key; n->gen = 0; n->t0 = frame_ms;
    *fresh = 1;
    return n;
}
#endif

int aui_anim(int key, int target, int ms, int curve)
{
#ifdef AUI_ANIM_OFF
    /* THE NEGATIVE CONTROL, and it has to live here rather than in the harness.
     * Every widget still calls aui_anim() and still draws whatever it returns,
     * so the picture is identical at rest and every code path above and below
     * is unchanged -- the value simply arrives instantly. Nothing is live, so
     * anim_schedule() registers no deadline and the interaction produces ONE
     * composite instead of eight.
     *
     * WHAT IT IS PROTECTING AGAINST is the reading this gate would otherwise
     * give for free: a compositor that happens to repaint several times after
     * any click would satisfy the positive assertion whether or not a single
     * animation ran. If the OFF build also reads eight composites, the harness
     * is measuring the machine's idle repaints and the `anim` row is not a
     * measurement. That is a thing this tree has shipped before -- see aui.h's
     * note on the toggle whose animation the header documented for as long as
     * it did not exist. */
    (void)key; (void)ms; (void)curve;
    return iclamp(target, 0, 255);
#else
    int id = id_ctr;                 /* the widget currently being drawn */
    int fresh = 0;
    struct anim_slot *s;

    target = iclamp(target, 0, 255);
    if (ms < 1) ms = 1;
    /* id 0 means aui_anim() was called outside any widget, where there is no
     * stable identity to key on. Answer instantly rather than colliding with a
     * free slot, which is what id 0 and key 0 would otherwise match. */
    if (id <= 0) return target;
    s = anim_slot_for(id, key, &fresh);

    if (fresh || anim_reduced) {
        s->from = s->to = s->cur = target;
        s->arrived = 1;
    } else if (target != s->to) {
        /* RE-AIM FROM WHERE THE PIXELS ARE, not from where this leg started.
         * A pointer that leaves mid-fade reverses out of the value on screen
         * and never snaps -- which is the whole difference between an
         * interruptible animation and a broken one. */
        s->from = s->cur;
        s->to = target;
        s->arrived = 0;
        s->t0 = frame_ms;
    }
    s->dur = ms; s->curve = curve; s->gen = anim_gen;

    if (!s->arrived) {
        unsigned el = frame_ms - s->t0;
        if ((int)el >= s->dur) {
            /* TERMINATION IS A PROOF, NOT A THRESHOLD. The slot latches its
             * target here and stops registering deadlines, so the machine
             * draws exactly one more frame -- the one that puts the final
             * pixel down -- and then sleeps for real. */
            s->cur = s->to;
            s->arrived = 1;
        } else {
            int sdk_curve = curve == AUI_EASE_INOUT ? OL_EASE_INOUT
                          : curve == AUI_EASE_LINEAR ? OL_LINEAR : OL_EASE_OUT;
            /* THE 255/256 SEAM IS CLOSED HERE AND NOWHERE ELSE. The curves are
             * exact on 0..256 because 256 is a power of two; aui_mix is on
             * 0..255. Scaling the DELTA by e/256 lands on `to` exactly when e
             * hits 256 and never overshoots, so no widget ever sees a 256. */
            /* 2026-09-13: widget identity and scheduling remain here; the
             * actual interpolation now belongs to the shared SDK. */
            s->cur = ol_transition256(s->from,s->to,el,(uint64_t)s->dur,sdk_curve,0);
        }
    }
    if (!s->arrived) anim_live++;
    return s->cur;
#endif /* AUI_ANIM_OFF */
}

unsigned aui_anim_loop(void) { if(anim_reduced)return 0; anim_loop_want = 1; return frame_ms; }

void aui_anim_reset(void)
{
    for (int i = 0; i < AUI_ANIM_MAX; i++) anim_tab[i].id = 0;
    for (int i = 0; i < SCROLL_SLOTS; i++) scroll_slots[i].owner = 0;
    anim_have_due = 0; anim_live = 0; anim_armed = 0;
}

int      aui_anim_active(void) { return anim_live; }
unsigned aui_anim_frames(void) { return anim_frames_n; }

/* Called by aui_end() AFTER gui_flush(), and the ordering is the whole
 * anti-spin argument: the deadline is `now + tick`, where `now` is measured
 * once the frame has actually landed -- never `last + tick`. A frame that took
 * 27 ms against a 16 ms tick therefore yields a 43 ms period rather than a
 * queue of already-expired deadlines to catch up on. */
static void anim_schedule(void)
{
    int tick;
    if (!anim_live && !anim_loop_want) {
        /* Nothing moves. Return before touching the clock: this is the path an
         * idle desktop takes and it must cost zero syscalls. */
        anim_have_due = 0;
        return;
    }
    if (anim_live) {
        /* CADENCE IS DERIVED FROM THE WINDOW, NOT A CONSTANT. SYS_GUI_FLUSH
         * carries no rectangle, so the frame this deadline buys will cost the
         * whole canvas; asking for frames faster than the compositor can make
         * them just queues work behind the BKL. A wrong estimate makes an
         * animation coarse and can never make it spin, because the floor is
         * AUI_ANIM_TICK_MIN and the deadline is computed after the fact. */
        int sc = aui_scale();
        long long px = (long long)win_w * win_h * sc * sc / 10000;
        long long est = px * AUI_NS_PER_PX / 1000000;
        tick = est > 1000 ? 1000 : (int)est;
        if (tick < AUI_ANIM_TICK_MIN) tick = AUI_ANIM_TICK_MIN;
        /* Both kinds live at once: the SOONER deadline wins. */
        if (anim_loop_want && AUI_ANIM_TICK_LOOP < tick) tick = AUI_ANIM_TICK_LOOP;
    } else {
        tick = AUI_ANIM_TICK_LOOP;
    }
    anim_due_ms = (unsigned)monotonic_ms() + (unsigned)tick;
    anim_have_due = 1;
}

int aui_anim_due(void)
{
    if (!anim_have_due) { anim_armed = 0; return 0; }
    /* A minimized window keeps its state, but cannot show intermediate frames.
     * Leave the deadline pending; restore's focus event makes the endpoint
     * eligible again without a periodic hidden-window wake. */
    if (_sys(SYS_GUI_WIN_STATE, WINS_MINIMIZED, 0, 0) > 0) return 0;
    unsigned now = (unsigned)monotonic_ms();
    if ((int)(now - anim_due_ms) < 0) return 0;     /* signed delta: wrap-safe */
    anim_armed = 1;
    return 1;
}

int aui_anim_wait(void)
{
    /* THE ZERO IS THE LOAD-BEARING VALUE. wait_idle(0) is "sleep until an
     * event", which is what every app did before this file grew a clock, so the
     * safe answer is also the default answer and no app can reintroduce a spin
     * by forgetting a case. */
    if (!anim_have_due) return 0;
    if (_sys(SYS_GUI_WIN_STATE, WINS_MINIMIZED, 0, 0) > 0) return 0;
    unsigned now = (unsigned)monotonic_ms();
    int d = (int)(anim_due_ms - now);
    /* ...and never 0 on this path, because 0 means FOREVER. A deadline already
     * passed must round up to 1 ms, not down to a sleep that never returns. */
    return d > 0 ? d : 1;
}

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
    id_ctr = 0; foc_n = 0; clipn = 0; ox_ = oy_ = 0;
    in_dialog = 0; in_popup = 0; dlg_open_now = 0;
    in.hot = 0;
    wbb_next = aui_r(0, 0, 0, 0);
    tip_text = 0;
    /* Reset THIS frame's flush-diff recording (section 5a-flush). g_ad_prev /
     * g_ad_prev_n are last frame's data and must survive into this frame's
     * ad_finish() -- only the CUR side resets. */
    g_ad_cur_n = 0; g_ad_overflow = 0; g_ad_extra_have = 0;
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
    focus_ring(r.st & AUI_FOCUSED, x, y, w, h, rad);

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
    focus_ring(r.st & AUI_FOCUSED, x, y, size, size, rad);
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
    tip_last_text = s; tip_last_x = in.mx; tip_last_y = in.my; tip_last_owner = id_ctr;
}

static void draw_tip(void)
{
    if (!tip_last_text) return;             /* never shown this app run: nothing to fade */
    /* AUI_AK_EXTRA on the OWNING WIDGET's id, not a slot of its own -- the
     * tooltip has no widget identity of its own to key on, but it borrows the
     * owner's for exactly as long as the fade needs it. Reading through a
     * borrowed id_ctr is safe here specifically because this function, unlike
     * aui_dialog_begin, runs EVERY frame regardless of whether a tooltip is
     * currently requested -- so the slot never goes stale and the continuity
     * rule that would otherwise swallow this fade never fires. See the
     * comment on tip_last_text above. */
    int saved_id = id_ctr;
    id_ctr = tip_last_owner;
    int t = aui_anim(AUI_AK_EXTRA, tip_text ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    id_ctr = saved_id;
    if (t <= 0) { tip_last_text = 0; return; }

    int pad = AUI_SP(2), px = AUI_FS_LABEL;
    int w = aui_text_w(tip_last_text, px) + 2 * pad, h = px + 2 * pad;
    int x = tip_last_x + 12, y = tip_last_y + 18;
    if (x + w > win_w) x = win_w - w - 2;
    if (y + h > win_h) y = tip_last_y - h - 6;
    if (x < 2) x = 2;
    if (y < 2) y = 2;
    /* No alpha channel on aui_shadow/aui_text_sz, so the fade is the same
     * faux-alpha trick focus_ring uses: mix the drawn colour toward the
     * background as `t` climbs, which reaches the true colour exactly at
     * t=255 and is invisible against it at t=0. Geometry never moves. */
    unsigned bg = aui_is_dark() ? rgb(70, 72, 84) : rgb(48, 50, 60);
    aui_shadow_ex(x, y, w, h, AUI_R_SM, 3, 10, (aui_is_dark() ? 120 : 55) * t / 255);
    aui_round_a(x, y, w, h, AUI_R_SM, bg, t);
    aui_text_sz(x + pad, y + pad - 1, tip_last_text, aui_mix(bg, rgb(250, 250, 252), t), px);
}

int aui_checkbox_ex(int x, int y, const char *label, int *state, int enabled)
{
    int id = ++id_ctr, box = 18, lw = label ? tw(label) : 0;
    int w = box + (label ? AUI_SP(2) + lw : 0);
    struct wres r = wpoll(id, X_(x), Y_(y), w, box, enabled, 1);
    /* AUI_T_FAST, not BASE: this is a colour change IN PLACE, not geometry --
     * the box never moves or resizes, only its fill tone slides between
     * unchecked and checked. Queried every frame regardless of *state, so the
     * slot stays continuous across the click and the release fades instead of
     * snapping, the same reasoning as face_for(). */
    int vt = aui_anim(AUI_AK_VALUE, *state ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    unsigned fill = (r.st & AUI_OFF) ? (*state ? AUI_DISABLED_TX : AUI_DISABLED)
                                      : aui_mix(AUI_SURFACE, AUI_ACCENT, vt);

    focus_ring(r.st & AUI_FOCUSED, x, y, box, box, AUI_R_SM);
    aui_round(x, y, box, box, AUI_R_SM, fill);
    /* The hover wash stays an instant overlay rather than a second animated
     * fill: a pointer sweeping a column of checkboxes crosses a dozen of them
     * in a couple of hundred milliseconds, and a fade there would still be
     * arriving when the pointer has already left three boxes down. */
    if (!(r.st & AUI_OFF) && (r.st & AUI_HOVER))
        aui_round_a(x, y, box, box, AUI_R_SM, *state ? rgb(255, 255, 255) : AUI_TEXT, *state ? 26 : 14);
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
    focus_ring(r.st & AUI_FOCUSED, x, y, box, box, box / 2);
    /* Same shape as the checkbox: FAST colour cross-fade for the selected
     * state, an instant hover wash on top (a radio group is swept the same
     * way a checkbox column is), and the inner dot stays an instant pop --
     * matching the checkbox tick's rejection, see anim-no-checkbox-tick. */
    int vt = aui_anim(AUI_AK_VALUE, on ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    aui_circle(cx, cy, box / 2, aui_mix(AUI_SURFACE, AUI_ACCENT, vt));
    if (!on && (r.st & AUI_HOVER)) aui_round_a(x, y, box, box, box / 2, AUI_TEXT, 14);
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

/* THE PROOF WIDGET for the motion core in section 5c, and it is this one for a
 * reason that is not aesthetic: aui.h has documented the switch knob as a
 * clock-driven animation since the toggle shipped, and `kx` teleported the knob
 * 22 points in a single frame. Implementing it closes a documentation lie
 * rather than adding a feature.
 *
 * TWO THINGS BECOME ONE CLOCK. The knob's position and the track's colour both
 * read the SAME slot, so they cannot drift apart into a knob that has arrived
 * over a track that has not. The knob's SIZE never changes, so its circle mask
 * is a gfx cache hit on every frame of the slide -- only x moves.
 *
 * THE CLICK IS HANDLED BEFORE THE ANIMATION IS SAMPLED, which is not the order
 * the old code used. Toggling *state after drawing would have made the frame
 * that handles the press draw the OLD position, so the knob would sit still for
 * one frame -- ~21 ms of a 180 ms motion -- and read as a dropped click. */
int aui_toggle(int x, int y, int *state, int enabled)
{
    int id = ++id_ctr, w = 44, h = 24;
    struct wres r = wpoll(id, X_(x), Y_(y), w, h, enabled, 1);
    int changed = 0;
    if (r.clicked) { *state = !*state; changed = 1; }

    /* AUI_T_BASE, not AUI_T_FAST: this is GEOMETRY. In a 640x480 pt window at
     * 150% the canvas is ~691,200 device px, so 90 ms delivers four frames and
     * the knob's 33 device-px travel would arrive in four 8-px jumps -- a
     * strobe. 180 ms delivers eight, i.e. ~4 px a step, which reads as motion.
     * The same split is why the track colour could have been FAST and is not:
     * it shares this slot so the two cannot disagree. */
    int t = aui_anim(AUI_AK_VALUE, *state ? 255 : 0, AUI_T_BASE, AUI_EASE_OUT);

    unsigned track = aui_mix(AUI_TRACK, AUI_ACCENT, t);
    if (r.st & AUI_OFF) track = AUI_DISABLED;
    else if (r.st & AUI_HOVER) track = aui_shade(track, *state ? 14 : -8);
    focus_ring(r.st & AUI_FOCUSED, x, y, w, h, h / 2);
    aui_round(x, y, w, h, h / 2, track);
    int kx = x + 2 + (w - h) * t / 255;        /* t=0 -> x+2; t=255 -> x+w-h+2 */
    aui_shadow_ex(kx, y + 2, h - 4, h - 4, (h - 4) / 2, 1, 3, 70);
    aui_circle(kx + (h - 4) / 2, y + h / 2, (h - 4) / 2, (r.st & AUI_OFF) ? AUI_DISABLED_TX : rgb(255, 255, 255));
    return changed;
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

    /* Value motion is driven by what moved it. A dragged knob must sit under
     * the finger with zero lag -- an eased knob under a pointer reads as a
     * bug, not as polish -- so a drag or a click bypasses aui_anim entirely
     * and draws the raw position. An arrow key is a discrete event with no
     * finger to track, so THAT SAME KNOB animates over AUI_T_BASE. Because the
     * animated branch is only reached while not dragging, the slot goes stale
     * for the duration of any drag; the continuity rule then makes the very
     * next animated frame latch fresh at the raw position with zero jump --
     * which is what buys the drag its zero-cost exemption without a second
     * code path drawing the knob. */
    int fill_raw = (v - lo) * w / (hi - lo);
    int fill;
    if (dragging || r.clicked) {
        fill = fill_raw;
    } else {
        int target = (hi > lo) ? (v - lo) * 255 / (hi - lo) : 0;
        int t = aui_anim(AUI_AK_VALUE, target, AUI_T_BASE, AUI_EASE_OUT);
        fill = t * w / 255;
    }
    int ty = y + h / 2 - 2;
    aui_round(x, ty, w, 4, 2, AUI_TRACK);
    aui_round(x, ty, fill, 4, 2, AUI_ACCENT);
    int kx = x + fill;
    focus_ring(r.st & AUI_FOCUSED, kx - kr, y + h / 2 - kr, 2 * kr, 2 * kr, kr);
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
         * it moves at the same speed whatever the repaint rate is -- except
         * that until aui_anim_loop() existed NOTHING SET THE REPAINT RATE. In
         * any app sleeping on wait_idle(0) this was a still picture, and the
         * only place in the tree where it moved was gallery.c, which
         * hand-rolled a 50 ms tick of its own. The wake is the fix; the phase
         * arithmetic below is unchanged. */
        int seg = w / 3;
        int t = (int)((aui_anim_loop() / 6) % (unsigned)(w + seg));
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
    /* aui_anim_loop() rather than frame_ms: same value, plus the wake that
     * makes it advance in an app that is otherwise asleep. Read aui.h's price
     * for this before putting a spinner in a large window -- a loop repaints
     * the WHOLE canvas at AUI_ANIM_TICK_LOOP, forever. */
    unsigned phase = (aui_anim_loop() / 100) % 8;
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

/* ---- UTF-8, at the one place a raw EV_KEY codepoint becomes bytes in this
 * buffer ----
 *
 * logit_abi.h: EV_KEY's `a` is "a character, or a KEY_* code ... all > 0xFF
 * so they never collide with a character" -- i.e. above ASCII, `a` is either
 * one of the eight enumerated navigation codes or a Unicode code point (the
 * pinyin IME commits CJK this way). `(char)k` truncates a code point to its
 * low byte, which for U+4F60 (你) used to store a lone 0x60 -- a stray
 * backtick where a character should have been, and every byte after it still
 * decoded as if the buffer were well-formed UTF-8, which it no longer was.
 * tf_is_nav_key is enumerated rather than range-tested against KEY_UP/
 * KEY_RIGHT so a future non-contiguous KEY_* addition cannot silently start
 * being typed into text fields. */
static int tf_is_nav_key(int a)
{
    return a == KEY_UP || a == KEY_DOWN || a == KEY_PGUP || a == KEY_PGDN ||
           a == KEY_HOME || a == KEY_END || a == KEY_LEFT || a == KEY_RIGHT;
}

static int tf_utf8_encode(unsigned cp, char out[4])
{
    if (cp < 0x80)    { out[0] = (char)cp; return 1; }
    if (cp < 0x800)   { out[0] = (char)(0xC0 | (cp >> 6));
                        out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12));
                        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* A UTF-8 CONTINUATION byte (10xxxxxx). The caret and every insert/delete
 * boundary must never land on one -- gui_text_run and SYS_TEXT_MEASURE are
 * handed a byte range and decode it as UTF-8 from its start, so a boundary
 * one byte into a multi-byte sequence hands them a lead byte's tail as if it
 * were a new character and desyncs every character after it. */
static int tf_is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int tf_step_left(const char *s, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && tf_is_cont(s[i])) i--;
    return i;
}

static int tf_step_right(const char *s, int n, int i)
{
    if (i >= n) return n;
    i++;
    while (i < n && tf_is_cont(s[i])) i++;
    return i;
}

static int tf_index_at(const char *buf, int n, int px_off)
{
    int lo = 0, hi = n;
    while (lo < hi) {                       /* binary search: each probe is a syscall */
        int mid = (lo + hi + 1) / 2;
        if (twn(buf, mid) <= px_off) lo = mid; else hi = mid - 1;
    }
    while (lo > 0 && tf_is_cont(buf[lo])) lo--;   /* never split a UTF-8 char */
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

/* One key against the field's buffer and caret -- pulled out of
 * aui_textfield_ex's body so tests/unit/aui_textfield_test.c can drive it
 * directly (`#include "aui.c"`, same technique as aui_mask_test.c) without
 * exercising a single drawing syscall: this function never calls gui_text_run,
 * aui_round or anything else that reaches the kernel. Returns 1 on Enter
 * (aui_textfield_ex's documented "returns 1 on Enter" contract), 0 if some
 * other key was consumed, -1 if the key was not this widget's to take (the
 * caller must not set in.key_used).
 *
 * -DAUI_BYTE_BACKSPACE is the negative control: it reverts backspace/delete to
 * removing one BYTE instead of one character, which is the original bug --
 * mid-buffer text after a deleted lead byte is left holding orphaned
 * continuation bytes, decodable as neither the old character nor a new one. */
static int tf_apply_key(char *buf, int cap, int *np, int k, int shift)
{
    int n = *np, ret = 0;
    if (k == '\n') ret = 1;
    else if (k == 1) { tf_anchor = 0; tf_caret = n; }             /* Ctrl+A */
    else if (k == '\b' || k == 127) {
        if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
        else if (k == '\b' && tf_caret > 0) {
#ifdef AUI_BYTE_BACKSPACE
            int a = tf_caret - 1;
#else
            int a = tf_step_left(buf, tf_caret);
#endif
            int d = tf_caret - a;
            for (int i = a; i + d <= n; i++) buf[i] = buf[i + d];
            n -= d; tf_caret = a; tf_anchor = tf_caret;
        } else if (k == 127 && tf_caret < n) {
#ifdef AUI_BYTE_BACKSPACE
            int b = tf_caret + 1;
#else
            int b = tf_step_right(buf, n, tf_caret);
#endif
            int d = b - tf_caret;
            for (int i = tf_caret; i + d <= n; i++) buf[i] = buf[i + d];
            n -= d;
        }
        buf[n] = 0;
    }
    else if (k == KEY_LEFT)  { if (tf_caret > 0) tf_caret = tf_step_left(buf, tf_caret); if (!shift) tf_anchor = tf_caret; }
    else if (k == KEY_RIGHT) { if (tf_caret < n) tf_caret = tf_step_right(buf, n, tf_caret); if (!shift) tf_anchor = tf_caret; }
    else if (k == KEY_HOME)  { tf_caret = 0; if (!shift) tf_anchor = 0; }
    else if (k == KEY_END)   { tf_caret = n; if (!shift) tf_anchor = n; }
    else if (k >= 32 && k < 127) {
        if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
        if (n < cap - 1) {
            for (int i = n; i > tf_caret; i--) buf[i] = buf[i - 1];
            buf[tf_caret++] = (char)k; n++; buf[n] = 0; tf_anchor = tf_caret;
        }
    }
    else if (k > 0x7F && !tf_is_nav_key(k)) {
        /* A code point above ASCII -- UTF-8 encode it and splice in all of its
         * bytes as one atomic unit, same shape as the ASCII branch above. */
        char enc[4]; int el = tf_utf8_encode((unsigned)k, enc);
        if (tf_caret != tf_anchor) tf_erase_sel(buf, &n);
        if (n + el < cap) {
            for (int i = n + el - 1; i >= tf_caret + el; i--) buf[i] = buf[i - el];
            for (int i = 0; i < el; i++) buf[tf_caret + i] = enc[i];
            n += el; tf_caret += el; buf[n] = 0; tf_anchor = tf_caret;
        }
    }
    else { *np = n; return -1; }                                  /* not ours */
    *np = n;
    return ret;
}

int aui_textfield_ex(int x, int y, int w, char *buf, int cap, const char *placeholder, int enabled)
{
    int id = ++id_ctr, h = AUI_H_CTL, ret = 0;
    int wx = X_(x), wy = Y_(y);
    /* Focusable, but the keyboard is the FIELD's -- see wpoll_kb. */
    struct wres r = wpoll_kb(id, wx, wy, w, h, enabled, 1, 0);
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
        int shift = in.mods & EV_MOD_SHIFT;
        in.key_used = 1;
        int kr = tf_apply_key(buf, cap, &n, in.a, shift);
        if (kr < 0) in.key_used = 0;                                  /* not ours */
        else ret = kr;
    }

    unsigned bg = (r.st & AUI_OFF) ? AUI_DISABLED : AUI_SURFACE;
    aui_round(x, y, w, h, AUI_R_MD, bg);
    focus_ring(foc, x, y, w, h, AUI_R_MD);
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

static int scrollbar_thumb(int h, int content, int view)
{ return imin(h, imax(24, (int)((long long)h * view / content))); }

static int scrollbar_input(int id, int x, int y, int h, int *off,
                           int content, int view, int *state)
{
    struct wres r = wpoll(id, X_(x) - 3, Y_(y), 16, h, 1, 0);
    *state = r.st;
    int direct = r.clicked || (in.active == id && in.down);
    if (direct) {
        int thumb = scrollbar_thumb(h, content, view), span = h - thumb;
        long long ty = (long long)in.my - Y_(y) - thumb / 2;
        *off = aui_scroll_clamp(span > 0 ? ty * (content - view) / span : 0,
                                content - view);
    }
    return direct;
}

static void scrollbar_draw(int x, int y, int h, int off, int content, int view, int state)
{
    int thumb = scrollbar_thumb(h, content, view), span = h - thumb;
    int ty = (int)((long long)aui_scroll_clamp(off, content - view) * span / (content - view));
    aui_round(x, y, 10, h, 5, AUI_TRACK);
    aui_round(x, y + ty, 10, thumb, 5,
              (state & (AUI_HOVER | AUI_ACTIVE)) ? AUI_MUTED : aui_mix(AUI_TRACK, AUI_TEXT, 90));
}

int aui_scrollbar(int x, int y, int h, int *off, int content, int view)
{
    int id = ++id_ctr, state = 0, old = *off;
    if (content <= view || h <= 0) { *off = 0; return old != 0; }
    *off = aui_scroll_clamp(*off, content - view);
    scrollbar_input(id, x, y, h, off, content, view, &state);
    scrollbar_draw(x, y, h, *off, content, view, state);
    return old != *off;
}

static struct {
    int x, y, w, h, sx, sy, state, show_bar;
    int *off, content;
} scr[4];
static int scrn, scr_skip;

static struct scroll_slot *scroll_slot_for(int *off)
{
    struct scroll_slot *free_slot = 0, *oldest = &scroll_slots[0];
    for (int i = 0; i < SCROLL_SLOTS; i++) {
        struct scroll_slot *s = &scroll_slots[i];
        if (s->owner == off) return s;
        if (!s->owner && !free_slot) free_slot = s;
        if ((unsigned)(anim_gen - s->gen) > (unsigned)(anim_gen - oldest->gen)) oldest = s;
    }
    struct scroll_slot *s = free_slot ? free_slot : oldest;
    s->owner = off; s->gen = anim_gen - 2; /* new/evicted owners latch */
    return s;
}

void aui_scroll_begin(int x, int y, int w, int h, int *off, int content_h)
{
    /* A rejected nested begin still has a matching end. Without this balance
     * the fifth container used to pop the fourth one's clip and origin. */
    if (scr_skip || scrn >= 4 || w <= 0 || h <= 0 || !off) { scr_skip++; return; }
    int wx = X_(x), wy = Y_(y), maxo = content_h > h ? content_h - h : 0;
    /* Tiny containers still clip and scroll; only the thumb is omitted when
     * its gutter/track cannot fit. Skipping begin would leave child drawing
     * unbounded during a small resize. */
    int show_bar = maxo > 0 && w >= 14 && h >= 5;
    int state = 0, direct = 0;
    struct scroll_slot *s = scroll_slot_for(off);
    int fresh = s->gen + 1 != anim_gen;
    /* Poll before painting the content, draw the thumb after it. The old
     * end-only poll moved the thumb but left the content one frame behind;
     * on mouse-up there might never be another frame to repair that mismatch.
     * A container reserves the gutter so its children cannot take the drag.
     */
    if (show_bar)
        direct = scrollbar_input(++id_ctr, x + w - 12, y + 2, h - 4,
                                  off, content_h, h, &state);
    long long delta = 0;
    if (in.ev == EV_WHEEL && !scroll_wheel_used && maxo > 0 &&
        (!scroll_wheel_owner || scroll_wheel_owner == off) && input_ok(wx, wy, w, h)) {
        delta = (long long)in.wheel * 48;
        scroll_wheel_used = 1;
    }
    *off = aui_scroll_sample(&s->motion, *off, maxo, delta, frame_ms, AUI_T_BASE,
                             fresh, direct, anim_reduced);
    if (s->motion.running) anim_live++;
    s->gen = anim_gen; s->order = ++scroll_order;
    scr[scrn].x = x; scr[scrn].y = y; scr[scrn].w = w; scr[scrn].h = h;
    scr[scrn].sx = ox_; scr[scrn].sy = oy_; scr[scrn].off = off;
    scr[scrn].content = content_h; scr[scrn].state = state;
    scr[scrn].show_bar = show_bar;
    scrn++;
    /* Wheel hit testing includes the gutter; content clipping excludes it. */
    s->viewport = clip_intersect(aui_r(wx, wy, w, h));
    clip_push(aui_r(wx, wy, show_bar ? w - 12 : w, h));
    ox_ = wx; oy_ = wy - *off;
}

void aui_scroll_end(void)
{
    if (scr_skip) { scr_skip--; return; }
    if (!scrn) return;
    scrn--;
    ox_ = scr[scrn].sx; oy_ = scr[scrn].sy;
    clip_pop();
    if (scr[scrn].show_bar)
        scrollbar_draw(scr[scrn].x + scr[scrn].w - 12, scr[scrn].y + 2, scr[scrn].h - 4,
                        *scr[scrn].off, scr[scrn].content, scr[scrn].h, scr[scrn].state);
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
    focus_ring(focused, x, y, w, h, AUI_R_MD);
    aui_stroke(x, y, w, h, AUI_R_MD, 1, AUI_BORDER);

    aui_scroll_begin(x + 1, y + 1, w - 2, h - 2, scroll, nrow * ROWH);
    for (int i = 0; i < nrow; i++) {
        int ry = i * ROWH;
        if (ry + ROWH < *scroll || ry > *scroll + h) continue;     /* culled */
        int hovered = input_ok(X_(0), Y_(ry), w - 2, ROWH);
        /* DELIBERATELY INSTANT, both of these. Selection must land where the
         * click landed on the frame the click landed -- a highlight that
         * travels toward a row the pointer has already left reads as lag, not
         * motion. And a list is the one place the pointer crosses a dozen
         * targets in a couple hundred milliseconds; a per-row fade would still
         * be arriving three rows after the pointer moved on. Keeping this
         * instant is itself an application of the vocabulary, not an omission
         * from it -- see the design dossier's list-selection item. */
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
        cx += tw_;
    }
    /* THE MOVING INDICATOR, drawn once after the loop rather than once per tab,
     * and on AUI_EASE_INOUT: this is the one category that earns it, an
     * indicator that starts and ends at rest and whose midpoint the eye tracks
     * across open space (aui.h's own words for this and the segmented pill).
     * aui_anim's domain is 0..255, which cannot hold a raw pixel x or width on
     * a window wider than that -- so both are normalised to a fraction of
     * win_w first and scaled back afterward, the same trick the toggle knob
     * uses for its own travel, generalised from two stops to N. Width is
     * animated too, unlike the segmented pill, because tab labels are not a
     * fixed division of the strip. */
    int tx = aui_anim(AUI_AK_VALUE, selx * 255 / imax(1, win_w), AUI_T_BASE, AUI_EASE_INOUT);
    int twid = aui_anim(AUI_AK_EXTRA, selw * 255 / imax(1, win_w), AUI_T_BASE, AUI_EASE_INOUT);
    int ux = tx * win_w / 255, uw = twid * win_w / 255;
    aui_round(ux + AUI_SP(2), y + h - 3, imax(0, uw - AUI_SP(4)), 3, 1, AUI_ACCENT);
    if (focused) {
        /* Ring the SELECTED TAB, not the whole strip: a box drawn around every
         * tab at once says "one of these is focused" and nothing about which,
         * which is the only thing a focus indicator is for. */
        if (in.ev == EV_KEY && !in.key_used) {
            if (in.a == KEY_LEFT)  { *sel = iclamp(*sel - 1, 0, n - 1); changed = 1; in.key_used = 1; }
            if (in.a == KEY_RIGHT) { *sel = iclamp(*sel + 1, 0, n - 1); changed = 1; in.key_used = 1; }
        }
    }
    focus_ring(focused, selx + 2, y + 3, selw - 4, h - 8, AUI_R_SM);
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
    focus_ring(focused, x, y, w, h, AUI_R_MD);
    /* The moving pill is drawn first so the labels sit on top of it. Segment
     * width is CONSTANT across a change of selection -- only x moves -- so
     * only one aui_anim call is needed (unlike the tab underline, which also
     * has to animate width) and the pill's mask stays a cache hit through the
     * whole slide. AUI_EASE_INOUT for the same reason as the tab underline:
     * this is the indicator category that earns it. */
    int pt = aui_anim(AUI_AK_VALUE, iclamp(*sel, 0, n - 1) * seg * 255 / imax(1, w), AUI_T_BASE, AUI_EASE_INOUT);
    int px = x + pt * w / 255 + 2;
    aui_shadow_ex(px, y + 2, seg - 4, h - 4, AUI_R_SM, 1, 3, 60);
    aui_round(px, y + 2, seg - 4, h - 4, AUI_R_SM, AUI_SURFACE);
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
    focus_ring(focus_id == id, x, y, w, h, AUI_R_MD);
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
    /* Fade in on open, alpha only -- no translate and no scale. Scale was
     * considered and rejected (see the design dossier): it would re-key every
     * rounded corner in the popup on every frame of the fade, which misses
     * the gfx mask cache by construction. Queried every frame regardless of
     * pop.kind, exactly like draw_tip(), so a dismiss has something continuous
     * to fade out from instead of vanishing. pop.owner is negative for a
     * menu-bar submenu (aui_menubar's `-2 - i` encoding) and aui_anim's id<=0
     * guard answers those instantly and on purpose: the menu bar is pure
     * glass, and animating anything that touches it is the single most
     * expensive mistake on this machine (dmg_expand grows the damage to the
     * whole panel -- see aui.h). */
    int saved_id = id_ctr;
    id_ctr = pop.owner;
    int t = aui_anim(AUI_AK_EXTRA, pop.kind == 1 ? 255 : 0, AUI_T_FAST, AUI_EASE_OUT);
    id_ctr = saved_id;
    if (t <= 0) { pop_prev.w = 0; return; }

    int h = pop.n * pop.itemh + AUI_SP(2);
    int x = pop.x, y = pop.y;
    if (y + h > win_h) y = imax(2, win_h - h - 2);

    if (pop.kind == 1) {
        /* Input and dismissal only apply while genuinely open; a fading-out
         * ghost takes no input but keeps blocking the rect it still occupies
         * on screen (pop_prev is simply left unrefreshed during the fade, so
         * it holds the last real geometry until t reaches 0 above). */
        pop_prev = aui_r(x, y, pop.w, h);
        if (in.ev == EV_KEY) {
            if (in.a == KEY_DOWN)      pop.hi = iclamp(pop.hi + 1, 0, pop.n - 1);
            else if (in.a == KEY_UP)   pop.hi = iclamp(pop.hi - 1, 0, pop.n - 1);
            else if (in.a == '\n')     { *pop.sel = pop.hi; pop_changed_id = pop.owner; pop.kind = 0; }
            else if (in.a == 27)       pop.kind = 0;     /* Escape: see the note in aui.h */
            in.key_used = 1;
        }
    }

    /* Same faux-alpha mix as focus_ring/draw_tip for the border, since
     * aui_stroke has no alpha parameter. */
    aui_shadow_ex(x, y, pop.w, h, AUI_R_MD, 3, 10, (aui_is_dark() ? 120 : 55) * t / 255);
    aui_round_a(x, y, pop.w, h, AUI_R_MD, AUI_SURFACE_2, t);
    aui_stroke(x, y, pop.w, h, AUI_R_MD, 1, aui_mix(AUI_SURFACE_2, AUI_BORDER, t));
    int hit = -1;
    for (int i = 0; i < pop.n; i++) {
        int iy = y + AUI_SP(1) + i * pop.itemh;
        int over = pop.kind == 1 &&
                   in.mx >= x && in.mx < x + pop.w && in.my >= iy && in.my < iy + pop.itemh;
        if (over) { pop.hi = i; hit = i; }
        if (i == pop.hi) aui_round_a(x + 3, iy, pop.w - 6, pop.itemh, AUI_R_SM, AUI_ACCENT, t);
        aui_text_ellipsis(x + AUI_SP(2), iy + (pop.itemh - PX) / 2 - 1, pop.w - AUI_SP(5),
                          pop.items[i], aui_mix(AUI_SURFACE_2, i == pop.hi ? AUI_ACCENT_TEXT : AUI_TEXT, t), PX);
    }
    if (pop.kind == 1 && in.ev == EV_MOUSE) {
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

/* THE ONE PLACE THIS FILE DOES NOT ROUTE THROUGH aui_anim()'s SLOT TABLE, and
 * it earns the exception rather than assuming it. aui_dialog_begin() is only
 * called by the app WHILE THE DIALOG IS OPEN -- unlike a hover fade or the
 * tooltip/popup above (which run every frame via aui_end() regardless of
 * their own visibility), there is no counterpart here that keeps polling
 * during the closed frames. So the slot's continuity rule -- "not queried on
 * the immediately preceding frame means fresh, latch instantly" -- fires on
 * the very first frame of every single open, and the entrance would silently
 * never animate. That is exactly the class of thing rule 5 (a control that
 * cannot be watched failing is worse than no control) warns about, so it is
 * written down here instead of hidden behind a plausible-looking aui_anim()
 * call that would always return 255.
 *
 * The fix is the SAME continuity test the slot table runs, done by hand
 * against the same frame counter (`anim_gen`, a static in this file), and it
 * reuses the shared curve and duration -- gfx_ease_out(), AUI_T_SLOW -- rather
 * than inventing a second clock. A dialog held open across frames re-observes
 * its own t0 and does not restart; closing and reopening (a frame gap, by
 * definition, since this function was not called in between) is a fresh
 * entrance, correctly. Close itself stays instant -- see aui.h / the design
 * dossier's DISMISS rule -- so there is nothing to animate on the way out. */
static unsigned dlg_last_gen;   /* anim_gen this ran at, last time; 0 = never */
static unsigned dlg_t0;         /* frame_ms when the CURRENT entrance began   */

int aui_dialog_begin(const char *title, int w, int h)
{
    dlg_open_now = 1;
    int th = 40;
    int x = (win_w - w) / 2, y = (win_h - h - th) / 3;
    if (y < 12) y = 12;
    dlg.x = x; dlg.y = y + th; dlg.w = w; dlg.h = h;

    if (!dlg_last_gen || dlg_last_gen + 1 != anim_gen) dlg_t0 = frame_ms;   /* a fresh open */
    dlg_last_gen = anim_gen;
    unsigned el = frame_ms - dlg_t0;
    int e = anim_reduced || (int)el >= AUI_T_SLOW ? 256 : ol_ease256(OL_EASE_OUT,(int)(el * 256u / AUI_T_SLOW));
    if (e < 256) anim_live++;      /* keep the wake contract awake for the rest of the entrance */
    int scrim_a = 110 * e / 256;
    int rise = 12 * (256 - e) / 256;

    aui_fill_a(0, 0, win_w, win_h, AUI_SCRIM, scrim_a);       /* the scrim */
    aui_shadow(x, y + rise, w, h + th, AUI_R_XL, AUI_ELEV_3);
    aui_round(x, y + rise, w, h + th, AUI_R_XL, AUI_SURFACE_2);
    aui_stroke(x, y + rise, w, h + th, AUI_R_XL, 1, AUI_BORDER);
    aui_text_in(aui_r(x, y + rise, w, th), title, AUI_TEXT, AUI_FS_TITLE, AUI_ALIGN_CENTER);
    in_dialog = 1;
    clip_push(aui_r(x, y + rise + th, w, h));
    ox_ = x; oy_ = y + rise + th;
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
