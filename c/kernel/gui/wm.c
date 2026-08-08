#include <stdint.h>
#include <stddef.h>
#include "wm.h"
#include "fb.h"
#include "text.h"
#include "icons.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "pit.h"
#include "serial.h"
#include "sched.h"
#include "vfs.h"
#include "settings.h"   /* settings line: theme + wallpaper are persisted */
#include "rtc.h"
#include "aex.h"
#include "blkdev.h"
#include "img.h"
/* The byte identifier /bin/show, the Terminal's output guard and Preview all
 * already share. Pure inline, no libc, no allocation -- see its header. */
#include "logit_sniff.h"
#include "logit_abi.h"
#include "prot.h"       /* cpu_prot_nx_usable() + PTE_NX for the app stack below */
/* Generated from include/abi/logit_calls.abi, which is where the packed syscall
 * arguments are described. Unpacking them by hand here meant the convention was
 * stated once in a logit_abi.h comment, once in the caller's packing, and once
 * here -- three copies of `(x<<16)|y` that nothing checked against each other.
 * Writing it down found a real disagreement: SYS_GUI_GLASS's radius is 8 bits,
 * which the comment's `(radius<<32)|...` never said. */
#include "logit_pack.h"
#include "ktime.h"
#include "evq.h"
#include "notify.h"     /* WM-HOOK 1/6: the notification overlay (see notify.h) */
#include "keyboard.h"
#include "net.h"
#include "http.h"
#include "kprintf.h"
#include "usercopy.h"
#include "proc.h"
#include "file.h"
#include "smp.h"
#include "percpu.h"
#include "spinlock.h"

#define MAXWIN     16
/* ---- units ----------------------------------------------------------------
 * Every geometry constant in this file is in POINTS; `struct win`'s x/y/w/h and
 * everything that touches a pixel are in DEVICE pixels. S() converts one to the
 * other and is the boundary between the two worlds.
 *
 * The surfaces are allocated at DEVICE size, and that single choice is the whole
 * difference between a sharp desktop and a magnified one. Allocating a window's
 * canvas at its logical size and scaling the blit at composite time would be far
 * less code -- and would produce exactly the blurry upscale this work exists to
 * avoid, because the glyphs would have been rasterized once at 1x. Instead the
 * app's draw calls are scaled on the way in, so text and vector icons are
 * rasterized AT the device size and the blit is 1:1.
 *
 * The other half is symmetry: what goes down as points*scale must come back up
 * as pixels/scale. A scaled UI with unscaled hit-testing is worse than no
 * scaling at all -- every click lands at a fraction of where it was aimed -- so
 * every enqueue_input() below routes its window-local coordinates through PT().
 */
/* ---- the mid-frame guard ---------------------------------------------------
 *
 * A window has ONE canvas. The app draws straight into the buffer the
 * compositor blits from, and SYS_GUI_FLUSH swaps nothing -- it marks the window
 * damaged and returns. So between an app's first draw of a frame (aui_begin ->
 * gui_clear, which erases the whole canvas) and its flush, that buffer is not a
 * picture; it is a picture being made. Compositing it in that interval puts a
 * blank window on the display, and THAT is the "one click and it does a
 * reload-refresh" the machine's user reported -- a VISIBLE defect, not a slow
 * one. A frame counter cannot see it, because a torn frame and a correct frame
 * are both one frame.
 *
 * Nothing ordered the two. The app runs ahead of the compositor whenever events
 * arrive faster than it paints -- a scroll burst, or the press AND release of a
 * single click, which are two repaints -- so the damage from frame N is still
 * pending when the app starts erasing the canvas for frame N+1.
 *
 * 1 = the compositor HOLDS BACK a damage rectangle that overlaps a window whose
 * canvas is half drawn, and composites it after the flush instead. This is
 * nearly free because a rectangle that is not composited is also not presented
 * -- fb_present_rect is called from exactly one place, render_region -- so the
 * pixels already in `back`, the last complete composite, stay on the display
 * untouched. No second buffer and no copy: the screen back buffer already IS
 * the retained copy, which is the invariant stated at the top of this file.
 *
 * 0 IS THE NEGATIVE CONTROL: the old behaviour, in which the compositor blits
 * whatever happens to be in the canvas at the moment it runs.
 * tests/qmp/qmp_flash.py --negative builds with it and requires the pixel check
 * to FAIL. `perf_torn` is counted either way, so both builds report the same
 * quantity for the same thing.
 */
#ifndef WM_MIDFRAME_GUARD
#define WM_MIDFRAME_GUARD 1
#endif
/* The hard bound: how long ONE FRAME of an app may hold its own rectangle off
 * the display. Past this, the rectangle composites regardless -- one torn frame
 * is a worse picture, a permanently frozen window is a worse machine.
 *
 * The clock starts at the app's first draw and is reset by its flush, so what
 * this actually catches is an app that has stopped: one that drew and never
 * flushed, or died holding a half-painted canvas. It deliberately does NOT
 * catch an app that paints one frame after another without pause -- a fast
 * scroll does exactly that. That window then shows its last COMPLETE frame for
 * as long as the burst lasts, which is the right answer and the same answer a
 * double-buffered compositor gives: there is no newer complete frame to show,
 * and half of the next one is not a substitute for it. An earlier version also
 * bounded how long the SCREEN could go without any composite at all, and that
 * bound did nothing but re-introduce the defect once per interval during a long
 * scroll -- 9 torn frames in a run that is otherwise 0.
 *
 * 400 ms is set against a measurement rather than a taste: `drawmax` in the
 * perf line is the longest app frame the machine has actually produced, and
 * under TCG the Finder's is 30-250 ms with a full browser repaint the slow one.
 * Every time the deadline fires is counted (`late`), so "the deadline never
 * fires" stays a reading rather than an assumption. */
#define WM_MIDFRAME_MAX_MS 400

#define S(v)       fb_pt(v)          /* points -> device pixels */
#define PT(v)      fb_dev2pt(v)      /* device pixels -> points */
#define MENUBAR_H  24                /* points */
#define TITLEBAR_H 30                /* points */
#define MBH        S(MENUBAR_H)
#define TBH        S(TITLEBAR_H)
#define FW         LOGIT_FONT_W
#define FH         LOGIT_FONT_H
#define USER_PATH_MAX 128
#define USER_URL_MAX  384
#define USER_TEXT_MAX 1024

/* ---- resize ----------------------------------------------------------------
 *
 * The grab band STRADDLES the frame edge -- RESIZE_OUT_PT of empty space
 * outside it and RESIZE_IN_PT of the window inside. A band that lived purely
 * outside would be unhittable in the one case that matters most (a window
 * against the screen edge), and one purely inside would steal that many points
 * from a scrollbar without giving anything back. Straddling costs the app the
 * outermost 4 points of its own content on each edge, which is the trade every
 * desktop makes and the reason a scrollbar is never flush to the frame.
 *
 * The floor is a WINDOW MANAGER floor, not a suggestion: an app may raise it
 * (SYS_GUI_WIN_MIN) and may not lower it. A window narrower than its own
 * titlebar controls is a window that cannot be closed or dragged, and "the app
 * asked for it" is not a defence when the user is the one who cannot get out. */
#define RESIZE_OUT_PT    4
#define RESIZE_IN_PT     4
#define RESIZE_CORNER_PT 16          /* along each edge, from the corner */
#define MIN_CONTENT_W_PT 180         /* the WM floor: room for the three lights */
#define MIN_CONTENT_H_PT 60

/* Which edges a grab is pulling. */
#define RZ_L 1
#define RZ_R 2
#define RZ_T 4
#define RZ_B 8

/* How often the CANVAS may be reallocated (and EV_RESIZE delivered) while a
 * drag is in flight. The frame itself follows the pointer with no throttle at
 * all -- this bounds only the expensive half.
 *
 * The number is a measurement, not a taste: a full-window repaint at 1920x1200
 * costs 24-27 ms of compositing, and a resize step costs TWO of them (the WM
 * relaying the damaged region, then the app's own SYS_GUI_FLUSH) plus the app's
 * paint. At one step per PS/2 packet that is several hundred milliseconds of
 * work per hundred milliseconds of hand movement, which is not slow, it is
 * unbounded. At 120 ms the app repaints ~8 times a second during a drag and the
 * frame still tracks the hand exactly, because the frame was never the cost. */
#define RESIZE_APPLY_MS 120

void *memcpy(void *, const void *, size_t);

/* The render pipeline (DOM/CSS/layout/paint) and <style>/<script> collection now
 * live in the ring-3 browser app; the kernel only provides fetch + draw + font
 * primitives. See net/{dom,css,layout}.c (compiled into browser.aex) and L1 plan. */

/* ---------- windows + apps ---------- */
enum wkind { WK_FINDER, WK_APP };

struct app {
    int  used, alive, id;
    char name[32];
    char arg[64];
    uint64_t base;
    int  win;                 /* window index, -1 until the app creates one */
};

struct win {
    int  used;
    int  x, y, w, h;          /* outer rectangle */
    char title[40];
    enum wkind kind;
    struct app *app;          /* owner (NULL for builtin) */
    struct surface surf;      /* content canvas (w x (h-TITLEBAR_H)) for apps */
    struct evq ev;            /* SYS_POLL_EVENT ring (coalesces motion -- see evq.h) */
    int  wants_close;
    int  cw_pt, ch_pt;        /* content size in POINTS -- what the app CURRENTLY has */
    char cwd[128];            /* Finder: current directory path */
    uint64_t open_t0;         /* tick the open "pop" animation began (0 = settled) */
    /* ---- resize / zoom / minimise -----------------------------------------
     * `w`,`h` above are the AUTHORITATIVE outer frame and they change the
     * instant the pointer moves. surf.w/surf.h is the canvas that has actually
     * been allocated, and the two are allowed to DISAGREE mid-drag -- see
     * blit_content(), which stretches the old canvas into the new frame until
     * the app has repainted at the new size. Keeping the frame live while the
     * canvas lags is what makes a resize feel attached to the hand without
     * paying for a full app repaint per pointer sample. */
    int  min_w_pt, min_h_pt;  /* app-declared minimum CONTENT size, in points */
    int  zoomed;              /* filling the desktop (see zoom_rect) */
    int  minimized;           /* hidden: not composited, not hit-tested */
    int  sx, sy, sw, sh;      /* the frame to restore to; only valid while zoomed */
    /* Pixels the canvas ALLOCATION holds, which is >= surf.w * surf.h. The
     * canvas is reshaped inside this block whenever the new size fits, so a
     * drag does not hand the kernel heap a differently-sized multi-megabyte
     * request eight times a second -- see win_apply_size. */
    int  surf_cap;
    /* Is this canvas half drawn RIGHT NOW? 1 from the app's first drawing
     * syscall after a flush until the next flush -- see the mid-frame guard
     * note near the top of this file. There is no ABI call that says "I am
     * starting a frame" and there does not need to be: the first draw after a
     * flush IS the start of one, which is true of every app in the tree and of
     * any app that has not been written yet. */
    int  drawing;
    uint64_t draw_t0;         /* time_mono_ms() when this frame's drawing began */
};

static struct app apps[MAXWIN];
static struct win wins[MAXWIN];
static int order[MAXWIN], norder;      /* z-order; order[norder-1] is on top */

static int mx, my, mleft, mright, mmiddle;
static int dragging = -1, drag_dx, drag_dy;
/* The window that owns the pointer until every button comes back up. Set on a
 * press inside a window's content; motion and the matching release go there even
 * once the pointer has left the window. Without it, dragging a scrollbar or
 * selecting text stops the instant the cursor slips outside -- and the app never
 * sees the button-up at all, so it stays stuck in "dragging" forever. */
static int mouse_capture = -1;
/* The resize drag. `rz_win` is the window; `rz_edge` the RZ_* mask; the four
 * anchors are the frame AS IT WAS when the grab started, and the delta is
 * always measured from the grab point rather than accumulated per sample --
 * accumulating drifts, and a resize that drifts is one that will not return to
 * the size it started at when the hand returns to where it started. */
static int rz_win = -1, rz_edge, rz_x0, rz_y0, rz_x1, rz_y1, rz_mx, rz_my;
static uint64_t rz_apply_ms;             /* time_mono_ms of the last canvas apply */
static volatile int dirty = 1;           /* the next frame needs a full recomposite */

static uint32_t *back, *bg;
static int W, H;

/* ---- damage ---------------------------------------------------------------
 *
 * A compositor that redraws the whole screen for a keystroke is not slow
 * because compositing is slow. It is slow because it does 4.1 M pixels of work
 * for a 200 x 20 change, and no amount of making the wallpaper copy faster
 * fixes an argument about WHAT to redraw.
 *
 * Dirty rectangles were here once and were removed on purpose; the reason was
 * written down at this spot and half of it is now obsolete. The obsolete half:
 * the pointer used to be pixels in the frame, the old partial path saved and
 * restored what was under it, and a restore over a region that had never been
 * recomposited smeared stale pixels across the screen. The pointer is a display
 * plane now -- there is no save-under, and nothing to restore.
 *
 * The half that is NOT obsolete is the hard half, and it is why this is a
 * correctness change wearing a performance change's clothes: `dirty_rect` used
 * to DISCARD ALL FOUR of its arguments, so no caller in this file was ever held
 * to reporting the true extent of what it changed. Every one of them is now.
 * A caller that under-reports leaves pixels on screen that no longer belong
 * there, and they stay until something else happens to cover them -- there is
 * deliberately no periodic full repaint to paper over that, because a bug that
 * heals itself twice a second is a bug no test can see. tests/qmp/qmp_damage.py
 * is that test, and its negative control makes exactly this mistake on purpose
 * and must fail.
 *
 * THE INVARIANT everything rests on: when wm_render returns, `back` holds a
 * correct composite of the WHOLE screen. A frame re-lays each damage rectangle
 * from the wallpaper up and draws every layer clipped to it, so every pixel it
 * writes was reset first. That is what keeps the read-modify-write chrome --
 * drop shadows, hairlines, the frosted panels -- idempotent from frame to frame
 * instead of accumulating onto itself.
 *
 * Two primitives read a NEIGHBOURHOOD instead of their own pixel: fb_blur_rect
 * and fb_liquid_glass. Those cannot be clipped to a sub-rectangle and still be
 * right, because the backdrop they would sample outside the clip is last
 * frame's output -- already frosted. So a damage rectangle that touches a glass
 * panel is grown to contain the WHOLE panel (dmg_expand), which restores the
 * property that the panel's entire backdrop was re-laid this frame. */
#define NDMG 6                                 /* rectangles; past this, merged */
struct drect { int x0, y0, x1, y1; };          /* half-open, DEVICE pixels */
static struct drect dmg[NDMG];
static int ndmg;
static int dirty_all = 1;                      /* next frame is the whole screen */

static int rect_hit(const struct drect *a, const struct drect *b)
{ return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1; }
static int rect_in(const struct drect *inner, const struct drect *outer)
{ return inner->x0 >= outer->x0 && inner->y0 >= outer->y0 &&
         inner->x1 <= outer->x1 && inner->y1 <= outer->y1; }
static void rect_or(struct drect *a, const struct drect *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}
static long rect_area(const struct drect *r)
{ return (long)(r->x1 - r->x0) * (long)(r->y1 - r->y0); }

static void dirty_full(void) { dirty_all = 1; ndmg = 0; dirty = 1; }

/* Fold a rectangle into the list. Ones that touch are unioned -- and unioning
 * two can bring the result into contact with a third, hence the loop. When the
 * list is full the pair whose union wastes the least area is merged rather than
 * the newest rectangle dropped: DROPPING DAMAGE IS HOW STALE PIXELS HAPPEN, and
 * an over-large rectangle is only slow. */
static void dmg_add(struct drect n)
{
    if (dirty_all) return;
    for (;;) {
        int merged = 0;
        for (int i = 0; i < ndmg; i++)
            if (rect_hit(&n, &dmg[i])) { rect_or(&n, &dmg[i]); dmg[i] = dmg[--ndmg]; merged = 1; break; }
        if (merged) continue;
        if (ndmg < NDMG) break;
        int best = 0; long bestcost = -1;
        for (int i = 0; i < ndmg; i++) {
            struct drect u = n; rect_or(&u, &dmg[i]);
            long cost = rect_area(&u) - rect_area(&n) - rect_area(&dmg[i]);
            if (bestcost < 0 || cost < bestcost) { bestcost = cost; best = i; }
        }
        rect_or(&n, &dmg[best]);
        dmg[best] = dmg[--ndmg];
    }
    dmg[ndmg++] = n;
    /* Past three quarters of the screen the rectangles are no longer telling
     * the truth about being small, and the per-region bookkeeping costs more
     * than it saves. Say so, rather than pretending. */
    long total = 0;
    for (int i = 0; i < ndmg; i++) total += rect_area(&dmg[i]);
    if (total * 4 > (long)W * (long)H * 3) dirty_full();
}

static void dirty_rect(int x, int y, int w, int h)
{
    if (dirty_all) { dirty = 1; return; }
    struct drect r = { x, y, x + w, y + h };
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > W) r.x1 = W;
    if (r.y1 > H) r.y1 = H;
    /* Nothing on screen changed, so do not ask for a frame. Setting `dirty`
     * here and then adding no rectangle would leave wm_render with an empty
     * damage list, which it can only read as "repaint everything" -- a window
     * dragged entirely off the left edge would have cost a full screen. */
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;
    dirty = 1;
    dmg_add(r);
}

/* WM-HOOK 2/6 -- what kernel chrome living OUTSIDE this file needs in order to
 * composite into this desktop (today: c/kernel/gui/notify.c). Declared in wm.h,
 * documented there. Damage is in device pixels, and the honest-extent rule
 * above applies to a caller in another file exactly as it applies to every
 * caller in this one. */
void wm_damage(int x, int y, int w, int h) { dirty_rect(x, y, w, h); }

/* A window's real footprint: its rectangle PLUS the drop-shadow bands
 * draw_frame lays around it (the widest is S(8)). Damage that stops at the
 * window's own edge leaves the old shadow standing when the window moves --
 * a thin dark ghost of the previous position, which is exactly the kind of
 * artefact that makes people distrust a partial-render path. */
static void win_box(const struct win *w, struct drect *r)
{
    int t = S(8) + 1;
    r->x0 = w->x - t;            r->y0 = w->y - t;
    r->x1 = w->x + w->w + t;     r->y1 = w->y + w->h + t;
}
/* THE NEGATIVE CONTROL. Set to 1 -- tests/qmp/qmp_damage.py --negative flips
 * this exact line in a throwaway copy of the tree -- a window reports only its
 * top-left quarter as damaged. That is precisely the mistake the whole change
 * is exposed to, made on purpose, and the pixel checks in that driver MUST fail
 * against it. Without it, "no stale pixels" is a claim about a test that has
 * never once failed, which is not evidence of anything. */
#define WM_DAMAGE_LIE 0

static void dirty_win(const struct win *w)
{
    struct drect r; win_box(w, &r);
#if WM_DAMAGE_LIE
    r.x1 = r.x0 + (r.x1 - r.x0) / 2;
    r.y1 = r.y0 + (r.y1 - r.y0) / 2;
#endif
    dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

/* THE RESIZE NEGATIVE CONTROL, and it is a DIFFERENT mistake from the one
 * above. WM_DAMAGE_LIE shrinks a window's reported box; this one reports the
 * new box honestly and forgets the OLD one -- the specific error a resize
 * invites, because a resize is the one gesture where the two boxes are
 * different shapes rather than the same shape in two places. Shrink a window
 * with this set and the strip it used to cover is never re-laid: the old
 * content simply stays on the wallpaper. Set to 1 by
 * tests/qmp/qmp_window.py --negative in a throwaway copy of the tree, whose
 * pixel checks must then fail. */
#define WM_RESIZE_DAMAGE_LIE 0

static int next_app_id = 1;

/* ---- compositor cost, measured on the machine ------------------------------
 *
 * "It got laggier" is not a number, and neither is a frame counter: what makes
 * the compositor expensive is a full recomposite, and what makes the DESKTOP
 * feel expensive is how many of those a user's hand provokes per second. So the
 * two things counted here are (a) composites and how long each one took, and
 * (b) pointer-motion samples -- the highest-frequency input in the system, and
 * the one whose cost this work is about. The ratio between them IS the claim:
 * one composite per motion sample is the old behaviour, ~zero is the new one.
 *
 * The clock is the monotonic ns clock (ktime.h), not timer_ticks(): a 10 ms tick
 * cannot resolve a frame, and a frame time quoted in ticks is exactly the kind
 * of unitless duration that let a 2x-fast PIT hide for the life of the kernel.
 *
 * Cumulative, with a timestamp, deliberately: a test brackets an interval with
 * two lines and subtracts. A driver that printed per-interval rates would force
 * its own idea of the interval on every reader. */
static uint64_t perf_composites, perf_comp_ns, perf_comp_ns_max;
static uint64_t perf_motions;                  /* pointer-motion samples processed */
static uint64_t perf_cursor_moves, perf_cursor_ns;   /* host-plane cursor updates */
/* ---- and WHAT a frame touched ---------------------------------------------
 *
 * A composite count answers "how often", never "how much", and those are the
 * two independent halves of the lag: motion moved the first one to ~zero and
 * left the second at the whole screen. So a frame is also priced in PIXELS:
 *
 *   perf_cpx  pixels recomposited (drawn into `back`)
 *   fb_present_px()  pixels pushed to the display -- the RAM copy + the DMA
 *   perf_present_ns  the part of a frame spent doing that pushing
 *   perf_full/perf_rects  frames that were the whole screen, and how many
 *                    rectangles the rest were split into
 *
 * perf_present_ns is what says whether flushing a sub-rectangle is worth
 * anything ON ITS OWN, separately from compositing less -- a question the
 * composite total cannot answer and which was asked directly. */
static uint64_t perf_cpx, perf_present_ns, perf_full, perf_rects;
/* ---- and whether a frame was a PICTURE ------------------------------------
 *
 * `torn` is the number of times the compositor has blitted a window whose
 * canvas was half drawn -- the defect itself, as a number. It is counted
 * whether or not the guard is compiled in, so the negative control reports the
 * same quantity as the fix does.
 *
 * The other three are what keep "torn == 0" from being uninterpretable:
 *   defer    rectangles the guard held back. Were this 0 while torn is 0, the
 *            guard would simply never have fired and the run would prove
 *            nothing about it.
 *   late     frames in which a deadline expired and the guard gave up. Nonzero
 *            means an app is painting slower than WM_MIDFRAME_MAX_MS.
 *   drawmax  the longest app frame seen, in ms -- the number the deadline has
 *            to be larger than, measured on the machine rather than guessed. */
static uint64_t perf_torn, perf_defer, perf_late, perf_drawmax;

/* System light/dark theme. The kernel-drawn chrome (menu bar, dock, window
 * frames) reads this directly; ring-3 apps query it via SYS_UI_DARK and follow. */
static int g_ui_dark;
/* WM-HOOK 3/6: so kernel chrome drawn from another file follows the system
 * theme instead of carrying its own idea of it. Declared in wm.h. */
int wm_dark(void) { return g_ui_dark; }
static void wm_set_dark(int on);
static int cascade;

/* app registry built by scanning the disk for *.aex */
struct regent { char file[48], name[32], ext[8]; char icon; uint32_t color; };
static struct regent reg[MAXWIN];
static int nreg;
static uint64_t reg_bounce[MAXWIN];    /* tick a dock icon's launch bounce started (0 = none) */

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return fb_rgb(r, g, b); }
static int lerp(int a, int b, int n, int d) { return a + (b - a) * n / d; }
static void blit(uint32_t *d, const uint32_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static int ends_aex(const char *s) {
    int n = 0; while (s[n]) n++;
    return n >= 4 && s[n-4]=='.' && s[n-3]=='a' && s[n-2]=='e' && s[n-1]=='x';
}
static const char *ext_of(const char *s) {
    const char *dot = 0;
    for (const char *p = s; *p; p++) if (*p == '.') dot = p;
    return dot ? dot + 1 : "";
}

/* ---------- z-order helpers ---------- */
static void raise_win(int wi)
{
    int at = -1;
    for (int i = 0; i < norder; i++) if (order[i] == wi) at = i;
    if (at < 0) { order[norder++] = wi; return; }
    for (int j = at; j < norder - 1; j++) order[j] = order[j + 1];
    order[norder - 1] = wi;
}
static void remove_win(int wi)
{
    int at = -1;
    for (int i = 0; i < norder; i++) if (order[i] == wi) at = i;
    if (at < 0) return;
    for (int j = at; j < norder - 1; j++) order[j] = order[j + 1];
    norder--;
}

/* Send a window to the BACK. The exact inverse of raise_win, which is what
 * makes Cmd+Shift+Tab undo Cmd+Tab instead of merely being another rotation
 * in the same direction. */
static void lower_win(int wi)
{
    int at = -1;
    for (int i = 0; i < norder; i++) if (order[i] == wi) at = i;
    if (at < 0) return;
    for (int j = at; j > 0; j--) order[j] = order[j - 1];
    order[0] = wi;
}

/* The topmost window a user can actually see and type into, as an index into
 * wins[] -- or -1. Minimised windows keep their place in the z-order (so
 * un-minimising restores where they were) but must not be focused, hit-tested
 * or handed a keystroke, which is why "the top of the stack" and "the focused
 * window" stopped being the same expression. */
static int top_visible(void)
{
    for (int i = norder - 1; i >= 0; i--) {
        struct win *w = &wins[order[i]];
        if (w->used && !w->minimized) return order[i];
    }
    return -1;
}

/* ---------- resize, zoom, minimise ----------
 *
 * Geometry lives here, in device pixels, and nowhere else. Every gesture --
 * an edge drag, the green light, a double-click, Cmd+M -- funnels into
 * win_set_frame(), so there is exactly one place that clamps, exactly one that
 * reports damage, and no way to add a fifth gesture that forgets either. */
static void dock_geom(int *x0, int *y0, int *dw, int *dh);   /* body with the dock */
static int in_rect(int px, int py, int x, int y, int w, int h);
static void enqueue(struct win *w, int type, int a, int b);

/* The smallest OUTER frame this window may have: the WM's floor, raised by
 * whatever the app asked for, plus the titlebar the app does not own. */
static void win_min_frame(const struct win *w, int *mw, int *mh)
{
    int cw = w->min_w_pt > MIN_CONTENT_W_PT ? w->min_w_pt : MIN_CONTENT_W_PT;
    int ch = w->min_h_pt > MIN_CONTENT_H_PT ? w->min_h_pt : MIN_CONTENT_H_PT;
    *mw = S(cw);
    *mh = TBH + S(ch);
}

/* Move and/or resize a window, damaging BOTH the region it left and the region
 * it now occupies.
 *
 * Reporting only the destination is the whole failure mode of a partial
 * compositor, and a resize is where it is easiest to commit: when a window
 * SHRINKS the two boxes are nested, the new one is entirely inside the old,
 * and every counter-based check still passes while a band of the previous
 * frame stays on screen untouched. WM_RESIZE_DAMAGE_LIE above makes exactly
 * that mistake on purpose. */
static void win_set_frame(struct win *w, int x, int y, int ww, int wh)
{
    int mw, mh;
    win_min_frame(w, &mw, &mh);
    if (ww < mw) ww = mw;
    if (wh < mh) wh = mh;
    /* The canvas is allocated at the frame's size, so a frame larger than the
     * display would allocate more than the screen -- and w*h*4 is the one
     * multiplication in this file that a user's hand controls directly. */
    if (ww > W) ww = W;
    if (wh > H) wh = H;
    if (y < MBH) y = MBH;                     /* never under the menu bar */
    if (w->x == x && w->y == y && w->w == ww && w->h == wh) return;
#if !WM_RESIZE_DAMAGE_LIE
    dirty_win(w);                             /* where it was */
#endif
    w->x = x; w->y = y; w->w = ww; w->h = wh;
    dirty_win(w);                             /* where it now is */
}

/* The rectangle a zoomed window fills.
 *
 * "Maximize" here means FILL THE DESKTOP, not full-screen, and the two are
 * different products. A full-screen mode hides the menu bar, and this desktop
 * has no reveal gesture to get it back -- the clock and the dark-mode switch
 * would simply be gone, with the keyboard offering no way to return. The dock
 * is the same argument from the other end: it is composited on top and windows
 * already slide under it, so a window that filled to the bottom edge would put
 * its last 70 points permanently behind glass. Stopping above the dock is what
 * macOS's zoom does with the Dock pinned, and it is the honest answer for a
 * dock that cannot auto-hide. */
static void zoom_rect(int *x, int *y, int *ww, int *wh)
{
    int dx0, dy0, dw, dh;
    dock_geom(&dx0, &dy0, &dw, &dh);
    (void)dx0; (void)dw; (void)dh;
    *x = 0;
    *y = MBH;
    *ww = W;
    *wh = dy0 - S(8) - MBH;
    if (*wh < TBH + S(MIN_CONTENT_H_PT)) *wh = H - MBH;   /* a dock taller than the screen */
}

static void win_set_zoom(struct win *w, int on)
{
    on = on ? 1 : 0;
    if (on == w->zoomed) return;
    if (on) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        int x, y, ww, wh;
        zoom_rect(&x, &y, &ww, &wh);
        w->zoomed = 1;
        win_set_frame(w, x, y, ww, wh);
    } else {
        w->zoomed = 0;
        /* EXACTLY the frame it had. Not "roughly where it was" and not
         * re-derived from the app's original gui_create() size -- a window the
         * user had already dragged and resized before zooming would land
         * somewhere it has never been, which reads as the machine losing the
         * window rather than restoring it. */
        win_set_frame(w, w->sx, w->sy, w->sw, w->sh);
    }
}

/* A manual drag or resize of a zoomed window un-zooms it and FORGETS the saved
 * frame. Keeping the frame would mean the green light later teleports the
 * window back to a position the user has since deliberately moved it away
 * from; a zoom the user has begun editing is no longer a zoom.
 *
 * CALLED ON MOVEMENT, NEVER ON THE PRESS. Calling it when the button goes down
 * broke double-click-to-restore in a way that looked like the double-click not
 * being detected at all: the FIRST click of the pair took the drag branch and
 * silently cleared `zoomed` while leaving the frame where it was, so the second
 * click's toggle read "not zoomed" and zoomed a window that was already filling
 * the screen. Nothing moved, twice. Touching a titlebar is not editing a zoom;
 * moving the window is. */
static void win_break_zoom(struct win *w)
{
    if (!w->zoomed) return;
    w->zoomed = 0;
    w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
}

static void win_set_min(struct win *w, int on)
{
    on = on ? 1 : 0;
    if (on == w->minimized) return;
    dirty_win(w);                    /* the space it vacates, or is about to fill */
    w->minimized = on;
    if (!on) raise_win((int)(w - wins));
    dirty_win(w);
}

/* Which edges of `w` the point (x,y) grabs, or 0 for "not a resize".
 *
 * A corner is a longer stretch of BOTH edges rather than the intersection of
 * two bands: the intersection is RESIZE_IN+RESIZE_OUT points square, which is
 * a target nobody hits on purpose. RESIZE_CORNER_PT along each edge is the
 * target people actually aim at. */
static int resize_edge_at(const struct win *w, int x, int y)
{
    if (w->minimized) return 0;
    int out = S(RESIZE_OUT_PT), in = S(RESIZE_IN_PT), corner = S(RESIZE_CORNER_PT);
    int x0 = w->x, y0 = w->y, x1 = w->x + w->w, y1 = w->y + w->h;
    if (x < x0 - out || x > x1 + out || y < y0 - out || y > y1 + out) return 0;
    int e = 0;
    if (x >= x0 - out && x <= x0 + in) e |= RZ_L;
    if (x <= x1 + out && x >= x1 - in) e |= RZ_R;
    if (y >= y0 - out && y <= y0 + in) e |= RZ_T;
    if (y <= y1 + out && y >= y1 - in) e |= RZ_B;
    if (!e) return 0;
    /* Extend whichever edges were hit along the perpendicular, so the last
     * `corner` points of a side resize both axes. */
    if (e & (RZ_L | RZ_R)) {
        if (y <= y0 + corner) e |= RZ_T;
        else if (y >= y1 - corner) e |= RZ_B;
    }
    if (e & (RZ_T | RZ_B)) {
        if (x <= x0 + corner) e |= RZ_L;
        else if (x >= x1 - corner) e |= RZ_R;
    }
    /* A window pinned under the menu bar cannot be pulled further up, and
     * offering the grab anyway gives a cursor that promises something the
     * clamp will refuse. */
    if ((e & RZ_T) && w->y <= MBH) e &= ~RZ_T;
    return e;
}

/* The topmost window offering a resize grab at (x,y), and the edges. The
 * front-to-back walk matters: a window's outer band overhangs whatever is
 * behind it, and the one you can see is the one that must win. */
static int resize_hit(int x, int y, int *edge)
{
    for (int i = norder - 1; i >= 0; i--) {
        struct win *w = &wins[order[i]];
        if (!w->used || w->minimized) continue;
        int e = resize_edge_at(w, x, y);
        if (e) { *edge = e; return order[i]; }
        /* Stop at the first window that CONTAINS the point: a window behind it
         * may have an edge running under this one, and a grab that reaches
         * through an opaque window is a grab aimed at something invisible. */
        if (in_rect(x, y, w->x, w->y, w->w, w->h)) return -1;
    }
    return -1;
}

/* Everything of an (cw x ch) canvas that is NOT the (copy_w x copy_h) corner
 * carried over from the previous size: the strip to the right, and the band
 * below. Flat, in the window background colour -- the app repaints on the
 * event, so this is only ever seen for the frame or two in between. */
static void fill_new_area(uint32_t *px, int cw, int ch, int copy_w, int copy_h)
{
    uint32_t fill = g_ui_dark ? rgb(30, 30, 36) : rgb(250, 250, 252);
    for (int y = 0; y < copy_h; y++)
        for (int x = copy_w; x < cw; x++) px[(long)y * cw + x] = fill;
    for (int y = copy_h; y < ch; y++)
        for (int x = 0; x < cw; x++) px[(long)y * cw + x] = fill;
}

/* Reallocate a window's canvas to match its frame, and tell the app.
 *
 * THE ORDER MATTERS AND IT IS THE ABI: the surface is replaced FIRST and
 * EV_RESIZE queued second, so by the time an app reads the event the canvas it
 * is about to draw into already has the new dimensions. The other order would
 * hand an app an event describing a size it cannot yet draw at, and every app
 * would need to guess how long to wait.
 *
 * The old pixels are carried over row by row over the overlap and the rest is
 * filled flat. Not because anyone will look at them -- the app repaints on the
 * event -- but because "the rest" is whatever kmalloc last had in that block,
 * and a window that flashes a stranger's heap for one frame is a worse bug
 * than a window that flashes grey.
 *
 * A failed allocation is NOT a failed resize. The frame keeps its new size and
 * the compositor goes on stretching the canvas it already has (blit_content),
 * which is exactly what it does mid-drag anyway. Refusing the resize instead
 * would make a window's geometry depend on heap pressure. */
static void win_apply_size(struct win *w)
{
    int cw = w->w, ch = w->h - TBH;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    /* No canvas at all means the app has not created its window yet; there is
     * nothing to resize and nobody to tell. */
    if (!w->surf.px) return;
    if (w->surf.w == cw && w->surf.h == ch) return;

    /* GROW THE ALLOCATION IN STEPS; RESHAPE INSIDE IT EVERY TIME.
     *
     * The first version freed the old canvas and allocated a new one on every
     * apply. During a drag that is a differently-sized multi-megabyte request
     * eight times a second, and no allocator reuses those: at 1920x1200 the
     * kernel heap grew through seven 8 MiB extensions to 40 MiB inside a single
     * two-second resize, with `[mm] low:` warnings the whole way down. The
     * pixels were not leaked -- every block was freed -- but a heap that has to
     * be 40 MiB to satisfy a sequence of 4 MiB requests has been fragmented,
     * and fragmentation is the failure that looks like a leak.
     *
     * So the allocation is a CAPACITY, grown by half again when it is exceeded
     * and never shrunk while the window lives, and the canvas is reshaped
     * within it. A drag past the first few steps allocates nothing at all. The
     * ceiling is the screen, because win_set_frame already clamps the frame to
     * it, so the capacity cannot run away. */
    uint64_t px = (uint64_t)cw * (uint64_t)ch;
    if ((int64_t)px > (int64_t)w->surf_cap) {
        uint64_t want = px + px / 2;
        uint64_t ceil_px = (uint64_t)W * (uint64_t)H;
        if (want > ceil_px) want = ceil_px;
        if (want < px) want = px;
        uint32_t *nb = (uint32_t *)kmalloc((size_t)(want * 4));
        if (!nb) {
            /* Not a failed resize: the frame keeps its new size and the
             * compositor goes on stretching the canvas it has, which is what
             * it does mid-drag anyway. Refusing here would make a window's
             * geometry depend on heap pressure. */
            serial_puts("[wm] resize: canvas alloc failed; stretching\n");
            return;
        }
        int copy_w = w->surf.w < cw ? w->surf.w : cw;
        int copy_h = w->surf.h < ch ? w->surf.h : ch;
        for (int y = 0; y < copy_h; y++)
            for (int x = 0; x < copy_w; x++)
                nb[(long)y * cw + x] = w->surf.px[(long)y * w->surf.w + x];
        kfree(w->surf.px);
        w->surf.px = nb;
        w->surf_cap = (int)want;
        /* Everything outside the copied corner is whatever kmalloc last had in
         * that block. Nobody will look at it -- the app repaints on the event
         * -- but a window that flashes a stranger's heap for one frame is a
         * worse bug than one that flashes grey. */
        fill_new_area(w->surf.px, cw, ch, copy_w, copy_h);
    } else {
        /* Reshape in place. The row stride changes, so rows have to be walked
         * in the direction that never overwrites a row not yet read: down when
         * the stride grows (each row moves to a HIGHER offset), up when it
         * shrinks. Getting the direction wrong does not fault -- it silently
         * smears the top of the window over the rest of it. */
        int copy_w = w->surf.w < cw ? w->surf.w : cw;
        int copy_h = w->surf.h < ch ? w->surf.h : ch;
        int ow = w->surf.w;
        uint32_t *p = w->surf.px;
        if (cw > ow) {
            for (int y = copy_h - 1; y >= 0; y--)
                for (int x = copy_w - 1; x >= 0; x--)
                    p[(long)y * cw + x] = p[(long)y * ow + x];
        } else if (cw < ow) {
            for (int y = 0; y < copy_h; y++)
                for (int x = 0; x < copy_w; x++)
                    p[(long)y * cw + x] = p[(long)y * ow + x];
        }
        fill_new_area(p, cw, ch, copy_w, copy_h);
    }
    w->surf.w = cw;
    w->surf.h = ch;
    /* A clip belonging to the old canvas cannot mean anything on the new one,
     * and a stale one is the "white Terminal" bug (see fb.h) with a different
     * cause. The app is about to redraw from scratch regardless. */
    w->surf.clip_on = 0;
    w->cw_pt = PT(cw);
    w->ch_pt = PT(ch);
    enqueue(w, EV_RESIZE, w->cw_pt, w->ch_pt);
    dirty_win(w);
}

/* Bring every window's canvas up to date with its frame, THROTTLED while a
 * drag is in flight. Called once per WM loop pass -- not per input event, and
 * not per motion sample -- so a drain that hands us twenty pointer packets
 * costs one reallocation, not twenty. On the release (rz_win < 0) it runs
 * unthrottled, which is what guarantees the final size is always applied
 * however briefly the last motion sample and the button-up were apart. */
static void wm_apply_sizes(void)
{
    if (rz_win >= 0) {
        uint64_t now = time_mono_ms();
        if (now - rz_apply_ms < RESIZE_APPLY_MS) return;
        rz_apply_ms = now;
    }
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].kind == WK_APP) win_apply_size(&wins[i]);
}

/* Draw a window's content, stretching if the canvas has not caught up with the
 * frame yet. The stretch is the visible half of the resize throttle: what a
 * user sees mid-drag is their own last frame scaled, which is what every other
 * desktop shows and reads as the window resizing rather than blanking. */
static void blit_content(struct win *w, int dx, int dy, int cw, int ch)
{
    if (!w->surf.px) return;
    /* THE DEFECT, COUNTED -- here, at the blit, rather than where the fix is,
     * so the measurement is the same instrument in both builds. Every path that
     * puts an app's canvas on screen goes through this function. */
    if (w->drawing) perf_torn++;
    if (w->surf.w == cw && w->surf.h == ch) fb_blit_surface(dx, dy, &w->surf);
    else fb_blit_surface_scaled(dx, dy, cw, ch, &w->surf);
}

/* ---------- launching apps ---------- */
static struct app *find_live_app(const char *name)
{
    for (int i = 0; i < MAXWIN; i++)
        if (apps[i].used && apps[i].alive && streq(apps[i].name, name))
            return &apps[i];
    return NULL;
}

void wm_launch(const char *aex_file, const char *arg)
{
    int sz = vfs_size(aex_file);
    if (sz <= 0) { serial_puts("[wm] launch: not found\n"); return; }
    if (sz < AEX_HDR_SIZE) { serial_puts("[wm] launch: bad aex\n"); return; }  /* aex_info reads the 64-byte header */

    if (sz > 0x7FF00000) { serial_puts("[wm] launch: too large\n"); return; }  /* sz+511 would overflow int */
    int bytes = ((sz + 511) / 512) * 512;
    void *img = kmalloc((unsigned)bytes);
    if (!img) { serial_puts("[wm] launch: kmalloc img failed\n"); return; }
    if (vfs_read(aex_file, img, bytes) <= 0) { serial_puts("[wm] launch: vfs_read failed\n"); kfree(img); return; }

    char name[32], ext[8];
    if (aex_info(img, name, ext) != 0) { serial_puts("[wm] launch: bad aex\n"); kfree(img); return; }

    struct app *exist = find_live_app(name);
    if (exist) {                            /* single instance: just focus it */
        serial_puts("[wm] launch: already live, focusing\n");
        /* And UN-MINIMISE it. Without this the dock icon of a minimised app
         * does nothing visible at all -- the app is already live, so the
         * launcher's single-instance branch "focuses" a window that is not on
         * screen. That is the only way back for a minimised window besides
         * Cmd+Tab, so it is not an optional nicety. */
        if (exist->win >= 0) { win_set_min(&wins[exist->win], 0); raise_win(exist->win); }
        dirty_full();
        kfree(img);                         /* image not needed -- app already running */
        return;
    }

    /* kick off the dock launch bounce for this app's icon */
    for (int i = 0; i < nreg; i++)
        if (streq(reg[i].file, aex_file)) { uint64_t t = timer_ticks(); reg_bounce[i] = t ? t : 1; break; }

    /* Each app gets its own address space so apps can't touch each other's
     * memory. The new PML4 shares the kernel + framebuffer mappings but has a
     * private user region. elf_load + the stack mapping both target the *active*
     * space, so switch CR3 into it (interrupts off, so the scheduler can't run
     * and reset CR3 mid-load), load, then restore the kernel space. */
    uint64_t space = vmm_new_space();
    if (!space) { serial_puts("[wm] launch: no address space\n"); kfree(img); return; }

    /* wm_launch may run from the WM thread (kernel CR3) OR from an app's
     * syscall (SYS_OPEN_PATH / dock-open) while a ring-3 app is current (that
     * app's CR3). Save and restore the CR3 that was actually active, not the
     * kernel's -- otherwise returning into the interrupted app would run it
     * with the wrong address space. (Input IRQs no longer reach here: they
     * only enqueue into inq[], and the WM thread calls us.) */
    uint64_t prev_cr3, fl;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) :: "memory");   /* save IF, then off */
    __asm__ volatile ("mov %%cr3, %0" : "=r"(prev_cr3));
    vmm_switch(space);
    uint64_t img_top = 0;
    uint64_t entry = aex_load(img, (uint64_t)bytes, name, ext, &img_top);   /* maps + copies into `space` */
    uint64_t ustack_top = 0;
    if (entry) {
        /* The stack must sit ABOVE the whole app image. browser/js link a large
         * mini-libc arena in BSS (96 MiB for the browser) plus several big CSS/page
         * buffers; a stack landing *inside* that BSS corrupts the allocator (and
         * vice versa), which is exactly what happened when the arena outgrew the
         * old fixed entry+40 MiB stack slot. Keep 40 MiB as the floor for small
         * apps, but raise the stack above the real image top (+ 4 MiB guard) when
         * the image is bigger. The stack is 8 MiB for the browser because QuickJS
         * recurses deeply throwing errors on real pages' scripts (github overran a
         * 256 KiB stack inside JS_ThrowError2); other apps get 4 MiB. */
        int stk_pages = streq(name, "browser.aex") ? 2048 : 1024;
        ustack_top = entry + 0x2800000;          /* 40 MiB above the link base */
        uint64_t need = img_top + 0x400000 + (uint64_t)stk_pages * 0x1000;
        if (img_top && need > ustack_top) ustack_top = need;
        /* NO-EXECUTE, the last of the three stack gaps exec.c predicted above
         * setup_cli_stack: "GUI apps get their stack from c/kernel/gui/wm.c,
         * not from here." A CLI program's stack has been non-exec since the
         * mask fix; without this line a windowed app -- which is every app the
         * user actually runs, including the browser -- kept a writable AND
         * executable stack, so the boundary test would pass while the thing it
         * protects stayed open. Conditional on cpu_prot_nx_usable() for the
         * same reason every other site is: setting bit 63 without EFER.NXE is
         * a reserved-bit fault, not a no-op. */
        uint64_t stk_flags = VMM_WRITABLE | VMM_USER |
                             (cpu_prot_nx_usable() ? PTE_NX : 0);
        for (int i = 1; i <= stk_pages; i++) {
            uint64_t frame = pmm_alloc();
            if (!frame) { entry = 0; break; }    /* OOM: fail the launch, don't run on a partial stack */
            vmm_map_page(ustack_top - (uint64_t)i * 0x1000, frame, stk_flags);
        }
    }
    vmm_switch(prev_cr3);
    /* Restore IF to the caller's state, NOT unconditionally: from the int 0x80
     * gate (SYS_OPEN_PATH -> launch) IF=0 on entry and the syscall-exit path
     * expects it still off; a blind sti here leaks IF=1 through the whole
     * return path (nested-IRQ windows the gate never planned for). */
    if (fl & 0x200) __asm__ volatile ("sti");
    if (!entry) { serial_puts("[wm] launch: load failed\n"); vmm_free_space(space); kfree(img); return; }

    int ai = -1;
    for (int i = 0; i < MAXWIN; i++) if (!apps[i].used) { ai = i; break; }
    if (ai < 0) { serial_puts("[wm] launch: app slots full\n"); vmm_free_space(space); kfree(img); return; }
    struct app *ap = &apps[ai];
    ap->used = ap->alive = 1;
    ap->id = next_app_id++;
    ap->base = entry;
    ap->win = -1;
    scopy(ap->name, name, sizeof ap->name);
    scopy(ap->arg, arg ? arg : "", sizeof ap->arg);

    /* Every app is a process now. The proc (not the struct app) is the thread's
     * payload; it carries the address space + fd table, and points back at the
     * window owner via ->gui. GUI apps are launched by the WM (ppid 0). */
    struct proc *p = proc_create(space, ap, ap->name, 0);
    if (!p) { serial_puts("[wm] launch: proc table full\n"); ap->used = ap->alive = 0; vmm_free_space(space); kfree(img); return; }
    /* Give every app real stdio (fd 0/1/2 = the serial console). Apps that only
     * draw never touch them, but it means pipe()/dup2() in an app (e.g. the
     * Terminal spawning a shell) get fds >= 3 and don't collide with 0/1/2. */
    { struct file *tty = file_open_tty();
      if (tty) { p->fd[0] = tty; file_dup(tty); p->fd[1] = tty; file_dup(tty); p->fd[2] = tty; } }
    p->tid = thread_create_user(ap->name, entry, ustack_top, p, space);
    if (p->tid < 0) {
        /* OOM: no thread will ever run this proc. Undo everything (the slot-undo
         * idiom matches proc_fork's failure path) or the proc + its address space
         * + the app slot all leak, and a RUNNING proc with no thread is unreapable. */
        for (int i = 0; i < NFD; i++) if (p->fd[i]) { file_close(p->fd[i]); p->fd[i] = NULL; }
        p->state = PROC_FREE; p->pid = 0; p->cr3 = 0;
        ap->used = ap->alive = 0;
        vmm_free_space(space);
        kfree(img);
        serial_puts("[wm] launch: no thread\n");
        return;
    }
    kfree(img);                             /* aex_load copied the image into `space`; drop the load buffer */
    serial_puts("[wm] launched ");
    serial_puts(ap->name);
    serial_puts("\n");
}

/* THE FILE ASSOCIATION, AND WHY IT ASKS THE BYTES.
 *
 * An .aex header carries ONE `ext` (aex.h), so an app that opens a dozen
 * formats can register for exactly one of them and the other eleven land
 * somewhere else. Preview is that app: it decodes PNG, APNG, GIF (animated),
 * JPEG, BMP, ICO, WebP, SVG, H.264, H.265, MP4, fragmented MP4, Matroska,
 * WebM, WAV, FLAC, MP3 and AAC, and it registered `h264`.
 *
 * Widening the header to a LIST of extensions would only move the lie. The
 * extension is a claim made by whoever named the file, and this system already
 * refuses to trust it in three places: Preview picks its decoder by sniffing,
 * /bin/show picks its renderer by sniffing, and the Terminal guards its
 * character grid by sniffing. So the launcher asks the same question they do,
 * through the same table (c/apps/coreutils/logit_sniff.h).
 *
 * 64 bytes off the front, matched against magic numbers. This is deliberately
 * NOT a parser: no length in the file is believed, nothing is allocated from
 * it, and the only thing the answer decides is WHICH app receives the path.
 * The app then re-sniffs the whole file with the real decoders and is the one
 * that can refuse -- and says why when it does.
 *
 * A registered extension still wins, so an app that claims a type keeps it. */
/* THERE IS NO PARTIAL READ IN THIS VFS, and that is why the head of the file
 * arrives the expensive way. `vfs_read(path, buf, max)` means "read the WHOLE
 * file if it fits": logitfs's inode_read() returns -1 when size > max rather
 * than filling what it can (c/fs/logitfs.c). So the obvious form of this
 * function --
 *      unsigned char b[64]; int n = vfs_read(path, b, sizeof b);
 * -- returns -1 for every file longer than 64 bytes, i.e. every real one, and
 * the sniff silently answers "no" for everything. It did, for a day: every
 * double-click landed in the Terminal, which is what a user sees as "MP4 will
 * not open in Preview" and "WebM says there is no such format" (that message
 * was the Terminal's).
 *
 * So: read the file, look at its first bytes, free it. Bounded by
 * SNIFF_READ_MAX because this runs under the big lock on every double-click,
 * and above that bound the launcher falls back to the file's NAME. That
 * fallback is the honest shape of the compromise -- sniff when we can afford
 * to, and when we cannot, believe the extension, which is what every other
 * system does all the time. A partial-read primitive would remove the bound
 * entirely and has been handed to the filesystem line. */
#define SNIFF_READ_MAX (4u << 20)

static int ext_opens_in_preview(const char *ext)
{
    static const char *k[] = {
        "png", "apng", "gif", "jpg", "jpeg", "bmp", "dib", "ico", "cur",
        "webp", "svg", "h264", "264", "h265", "265", "hevc", "mp4", "m4v",
        "mov", "m4a", "mkv", "webm", "mka", "wav", "flac", "mp3", "aac", 0
    };
    for (int i = 0; k[i]; i++) if (streq(k[i], ext)) return 1;
    return 0;
}

static int sniff_opens_in_preview(const unsigned char *b, int n)
{
    switch (sniff_id(b, n < SNIFF_PREFIX ? n : SNIFF_PREFIX)) {
    case SN_PNG: case SN_JPEG: case SN_GIF: case SN_BMP: case SN_SVG:
    case SN_WEBP: case SN_H264: case SN_H265: case SN_MP4: case SN_MKV:
    case SN_WAV: case SN_FLAC: case SN_MP3: case SN_OGG:
        return 1;
    default:
        /* ICO/CUR -- 00 00 01|02 00 <count> -- is the one format in Preview's
         * set that logit_sniff.h has no id for, and its first three bytes read
         * as an Annex B start code. Named here rather than edited into that
         * header, which belongs to another line. */
        return n >= 6 && !b[0] && !b[1] && (b[2] == 1 || b[2] == 2) && !b[3]
               && (b[4] | b[5]);
    }
}

static int opens_in_preview(const char *path, const char *ext)
{
    int sz = vfs_size(path);
    if (sz <= 0) return 0;
    if ((unsigned)sz > SNIFF_READ_MAX) return ext_opens_in_preview(ext);
    unsigned char *b = kmalloc((unsigned)sz);
    if (!b) return ext_opens_in_preview(ext);
    int n = vfs_read(path, b, sz);
    int yes = n > 0 ? sniff_opens_in_preview(b, n) : ext_opens_in_preview(ext);
    kfree(b);
    return yes;
}

static void launch_for_ext(const char *ext, const char *file)
{
    for (int i = 0; i < nreg; i++)
        if (reg[i].ext[0] && streq(reg[i].ext, ext)) { wm_launch(reg[i].file, file); return; }
    /* Anything Preview can decode opens in Preview, decided by content. */
    if (opens_in_preview(file, ext)) { wm_launch("preview.aex", file); return; }
    /* No registered handler -> open it in the Terminal (it runs `as <file>` for
     * .as scripts, else `cat`). Beats the old "no app handles that file type". */
    wm_launch("terminal.aex", file);
}

/* ---------- system info text (for the Activity Monitor app) ---------- */
/* Bounded appenders: never write at or past `end` (caller reserves end for NUL). */
static char *ap_num(char *p, char *end, uint64_t v)
{
    char t[20]; int i = 0;
    if (!v) { if (p < end) *p++ = '0'; return p; }
    while (v) { t[i++] = '0' + v % 10; v /= 10; }
    while (i && p < end) *p++ = t[--i];
    return p;
}
static char *ap_str(char *p, char *end, const char *s) { while (*s && p < end) *p++ = *s++; return p; }

static int sysinfo_text(char *buf, int max)
{
    if (max <= 0) return 0;
    char *p = buf, *end = buf + max - 1;            /* reserve 1 byte for NUL */
    /* Display geometry, in both units. Printed rather than merely known because
     * "the desktop is 1280x800" and "the framebuffer is 1280x800" stopped being
     * the same sentence the moment a scale factor existed, and every screenshot
     * argument from here on needs to say which one it means. */
    p = ap_str(p, end, "Display "); p = ap_num(p, end, (uint64_t)fb_width());
    p = ap_str(p, end, "x"); p = ap_num(p, end, (uint64_t)fb_height());
    p = ap_str(p, end, " px @ "); p = ap_num(p, end, (uint64_t)fb_scale());
    p = ap_str(p, end, "% = "); p = ap_num(p, end, (uint64_t)fb_width_pt());
    p = ap_str(p, end, "x"); p = ap_num(p, end, (uint64_t)fb_height_pt());
    p = ap_str(p, end, " pt\n");
    p = ap_str(p, end, "Uptime  "); p = ap_num(p, end, timer_ticks() / 100); p = ap_str(p, end, " s\n");
    p = ap_str(p, end, "Memory  "); p = ap_num(p, end, (pmm_total_bytes() - pmm_free_bytes()) >> 20);
    p = ap_str(p, end, " / "); p = ap_num(p, end, pmm_total_bytes() >> 20); p = ap_str(p, end, " MB used\n");
    p = ap_str(p, end, "Switches "); p = ap_num(p, end, sched_switches()); p = ap_str(p, end, "\n");
    /* Write barriers issued since boot. A journal orders nothing without them
     * -- a disk reorders freely inside its own write cache -- so this is the
     * one number that says whether the ordering the log claims is actually
     * being asked of the hardware. tests/boot/run-barrier-test.sh reads it. */
    p = ap_str(p, end, "Barriers "); p = ap_num(p, end, (long)blk_flush_count());
    p = ap_str(p, end, "\n");
    /* Milliseconds since boot -- the same number SYS_MONOTONIC_MS answers, so a
     * caller can cross-check its own clock against the machine's without a
     * second syscall. 10 ms granular (100 Hz tick); see pit.h. */
    p = ap_str(p, end, "Uptime-ms "); p = ap_num(p, end, timer_ms());
    p = ap_str(p, end, "\n");
    /* Event-ring accounting. `merged` is mouse motion coalesced onto an unread
     * motion sample instead of taking a slot; `dropped` is events lost to a full
     * ring, and it is the number that has to stay 0 -- a dropped click is a
     * click the user made and the machine did not act on. Printed rather than
     * merely counted because "motion cannot overflow the queue" is a claim, and
     * a claim you cannot read back is a comment. */
    p = ap_str(p, end, "Events "); p = ap_num(p, end, evq_queued());
    p = ap_str(p, end, " queued, "); p = ap_num(p, end, evq_coalesced());
    p = ap_str(p, end, " merged, "); p = ap_num(p, end, evq_dropped());
    p = ap_str(p, end, " dropped\n\n");
    p = ap_str(p, end, "PID  NAME\n");
    p = ap_str(p, end, "  0  wm (compositor)\n");
    for (int i = 0; i < MAXWIN; i++)
        if (apps[i].used && apps[i].alive) {
            p = ap_str(p, end, "  "); p = ap_num(p, end, apps[i].id); p = ap_str(p, end, "  ");
            p = ap_str(p, end, apps[i].name); p = ap_str(p, end, "\n");
        }
    *p = 0;
    return (int)(p - buf);
}

/* ---------- GUI syscalls (called from syscall.c in the app's context) ---------- */
/* Watchdog for g_net_busy: set when a fetch owns the net (SYS_HTTP_GET /
 * SYS_RES_FETCH). If that thread is killed mid-fetch (window closed, fault),
 * the flag would stay 1 forever and net_poll in the WM loop would never run
 * again. Armed/cleared alongside g_net_busy; checked in the WM main loop. */
static uint64_t net_busy_t0;

/* Hard wall-clock cap on one SYS_HTTP_GET, retries and redirects included.
 *
 * Every layer below has its own timeout -- tcp_connect 5 s, http's idle budget
 * 8 s, up to 5 redirect hops -- and they MULTIPLY. Nothing bounded the product,
 * so an unreachable host could hold the big kernel lock for minutes while the
 * machine looked hung. 15 s is long enough for a slow-but-working site over a
 * TLS handshake and short enough that a user waits rather than reboots.
 *
 * Note this got worse, not better, when the PIT was fixed: the tick had been
 * running at twice its programmed rate, so every timeout expressed in ticks was
 * silently half its stated duration. Correcting the clock doubled every wait in
 * this path. The commit that fixed it said durations now mean what they claim;
 * it should also have said they therefore got longer. */
#define HTTP_FETCH_CAP_TICKS 1500
static struct win *app_window(struct app *ap)
{
    return (ap && ap->win >= 0) ? &wins[ap->win] : NULL;
}

/* The running thread's window-owning app, via its process. A CLI/forked process
 * has gui == NULL, so GUI syscalls from it simply fail (it has no window). */
static struct app *cur_app(void)
{
    struct proc *p = proc_current();
    return p ? (struct app *)p->gui : NULL;
}

/* Does this syscall put pixels into the app's canvas?
 *
 * One list, in one place, rather than a line added to each of the nine cases:
 * the flag is set by the FIRST such call in a frame, and a tenth drawing call
 * gets added here. SYS_GUI_CLIP is deliberately absent -- it writes no pixels,
 * and aui_begin issues it immediately before gui_clear, so counting it would
 * start the frame one call early for nothing. */
static int is_draw_call(long num)
{
    switch (num) {
    case SYS_GUI_CLEAR: case SYS_GUI_RECT: case SYS_GUI_RRECT:
    case SYS_GUI_TEXT:  case SYS_GUI_TEXT_MONO: case SYS_GUI_ICON:
    case SYS_GUI_GLASS: case SYS_GUI_TEXT_RUN:  case SYS_GUI_BLIT:
        return 1;
    }
    return 0;
}

long wm_gui_syscall(long num, long a, long b, long c)
{
    struct app *ap = cur_app();
    if (!ap && num != SYS_HTTP_GET && num != SYS_HTTP_STATUS &&
        num != SYS_HTTP_BODY && num != SYS_SYSINFO && num != SYS_SCREEN_INFO) {
        /* Window ADOPTION: a CLI process (e.g. /bin/as running a script) gets a
         * window on its first SYS_GUI_CREATE -- allocate an app slot and bind it
         * to the proc, then fall through to the normal create. Exit/teardown
         * reuses the standard path: proc_exit -> wm_app_exit (alive=0) -> reap.
         * HTTP fetch/body/status are process-safe non-GUI services and also pass
         * this gate, as does SYSINFO -- it is a read-only query about the system
         * with no window semantics at all, and gating it meant /bin/sh and every
         * CLI program could not ask the machine about itself. Any other GUI call
         * without a window stays an error. */
        struct proc *p = proc_current();
        if (!p || num != SYS_GUI_CREATE) return -1;
        int ai = -1;
        for (int i = 0; i < MAXWIN; i++) if (!apps[i].used) { ai = i; break; }
        if (ai < 0) return -1;
        ap = &apps[ai];
        ap->used = ap->alive = 1;
        ap->id = next_app_id++;
        ap->base = 0;
        ap->win = -1;
        scopy(ap->name, p->name, sizeof ap->name);
        ap->arg[0] = 0;
        p->gui = ap;
    }

    /* The frame boundary, observed from the kernel side. This runs before the
     * switch and for EVERY call, so a window is marked mid-draw by whatever the
     * app happens to draw first: there is no ordering an app has to obey and no
     * ABI for it to get wrong. */
    {
        struct win *dw = app_window(ap);
        if (dw) {
            if (num == SYS_GUI_FLUSH) {
                if (dw->drawing) {
                    uint64_t d = time_mono_ms() - dw->draw_t0;
                    if (d > perf_drawmax) perf_drawmax = d;
                }
                dw->drawing = 0;
            } else if (is_draw_call(num) && !dw->drawing) {
                dw->drawing = 1;
                dw->draw_t0 = time_mono_ms();
            }
        }
    }

    switch (num) {
    case SYS_GUI_CREATE: {
        if (ap->win >= 0) return 0;
        char title[64];
        if (user_copy_string(title, sizeof title, (const char *)a) < 0) return -1;
        int wi = -1;
        for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { wi = i; break; }
        if (wi < 0) return -1;
        /* (cw,ch) are POINTS. Every app in the tree passed device pixels here
         * before scaling existed, and at scale 100 the two are the same number,
         * which is precisely why reinterpreting the unit needed no ABI break and
         * no app edit: an app that says 640x444 keeps getting the window it has
         * always got, and on a denser display gets the same window drawn with
         * more pixels. */
        int cw_pt = LOGIT_GUI_CREATE_B_W(b), ch_pt = LOGIT_GUI_CREATE_B_H(b);
        if (cw_pt <= 0 || ch_pt <= 0) return -1;
        int cw = S(cw_pt), ch = S(ch_pt);
        /* cw,ch are each up to 65535 -- cw*ch*4 overflows int. A window larger
         * than the screen is meaningless, so cap to the framebuffer; do the size
         * math in 64-bit. */
        if (cw > (int)fb_width() || ch > (int)fb_height()) return -1;
        uint64_t pxcount = (uint64_t)cw * (uint64_t)ch;
        struct win *w = &wins[wi];
        w->used = 1; w->kind = WK_APP; w->app = ap;
        w->cw_pt = cw_pt; w->ch_pt = ch_pt;
        w->w = cw; w->h = TBH + ch;
        w->x = S(110 + cascade * 28); w->y = S(70 + cascade * 28);
        cascade = (cascade + 1) % 6;
        { int SW = (int)fb_width(), SH = (int)fb_height();   /* keep big windows on-screen */
          if (w->x + w->w > SW) w->x = SW - w->w; if (w->x < 0) w->x = 0;
          if (w->y + w->h > SH - S(4)) w->y = SH - S(4) - w->h; if (w->y < MBH) w->y = MBH; }
        scopy(w->title, title, sizeof w->title);
        w->surf.w = cw; w->surf.h = ch;
        w->surf.clip_on = 0;             /* fresh canvas: a reused slot must not inherit a clip */
        w->surf.px = kmalloc((size_t)(pxcount * 4));
        if (!w->surf.px) { w->used = 0; return -1; }
        w->surf_cap = (int)pxcount;      /* the canvas grows in steps from here */
        for (uint64_t i = 0; i < pxcount; i++) w->surf.px[i] = rgb(250, 250, 252);
        w->drawing = 0; w->draw_t0 = 0;   /* a fresh canvas is a finished picture */
        evq_reset(&w->ev); w->wants_close = 0;
        /* A REUSED SLOT MUST NOT INHERIT the previous tenant's window state.
         * `used` guards the readers, but zoomed/minimized/min_* are read the
         * moment the new window is composited -- a slot whose last occupant was
         * minimised would open a window that is not on screen, and nothing
         * would say why. */
        w->min_w_pt = w->min_h_pt = 0;
        w->zoomed = w->minimized = 0;
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        { uint64_t t = timer_ticks(); w->open_t0 = t ? t : 1; }   /* trigger open pop */
        ap->win = wi;
        raise_win(wi);
        dirty_full();
        return 0;
    }
    case SYS_GUI_CLEAR: {
        struct win *w = app_window(ap); if (!w) return -1;
        fb_target(&w->surf); fb_clear((uint32_t)a); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_RECT: {
        struct win *w = app_window(ap); if (!w) return -1;
        /* Points in, device pixels out. The width is the DIFFERENCE of two
         * converted edges, not the converted width: at scale 150 two abutting
         * 5-point columns are 7 and 8 pixels wide, and converting each width on
         * its own would make them 7 and 7 and leave a seam that moves as the
         * app scrolls. Same idiom in every rect-shaped call below. */
        int x = S(LOGIT_GUI_RECT_A_X(a)), y = S(LOGIT_GUI_RECT_A_Y(a));
        int rw = S(LOGIT_GUI_RECT_A_X(a) + LOGIT_GUI_RECT_B_W(b)) - x;
        int rh = S(LOGIT_GUI_RECT_A_Y(a) + LOGIT_GUI_RECT_B_H(b)) - y;
        /* rw/rh are user-controlled (up to 65535 each): an unclamped fill runs
         * up to 65535^2 fb_put calls inside the syscall gate (IF=0 + BKL held),
         * freezing the machine for seconds per call. Intersect with the surface. */
        if (rw > w->surf.w - x) rw = w->surf.w - x;
        if (rh > w->surf.h - y) rh = w->surf.h - y;
        fb_target(&w->surf); fb_fill_rect(x, y, rw, rh, (uint32_t)c); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_RRECT: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = S(LOGIT_GUI_RRECT_A_X(a)), y = S(LOGIT_GUI_RRECT_A_Y(a));
        int rw = S(LOGIT_GUI_RRECT_A_X(a) + LOGIT_GUI_RRECT_B_W(b)) - x;
        int rh = S(LOGIT_GUI_RRECT_A_Y(a) + LOGIT_GUI_RRECT_B_H(b)) - y;
        int radius = S(LOGIT_GUI_RRECT_C_RADIUS(c));
        /* same surface-intersection clamp as SYS_GUI_RECT (user-controlled size
         * behind the syscall gate) */
        if (rw > w->surf.w - x) rw = w->surf.w - x;
        if (rh > w->surf.h - y) rh = w->surf.h - y;
        fb_target(&w->surf); fb_round_rect(x, y, rw, rh, radius, (uint32_t)LOGIT_GUI_RRECT_C_COLOR(c)); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_TEXT: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = S(LOGIT_GUI_TEXT_A_X(a)), y = S(LOGIT_GUI_TEXT_A_Y(a));
        char text[USER_TEXT_MAX];
        if (user_copy_string(text, sizeof text, (const char *)c) < 0) return -1;
        /* fb_text picks up the scaled UI size itself (fb_ui_px), so the glyphs
         * are re-rasterized larger, not blown up. */
        fb_target(&w->surf); fb_text(x, y, text, (uint32_t)b); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_TEXT_MONO: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = S(LOGIT_GUI_TEXT_MONO_A_X(a)), y = S(LOGIT_GUI_TEXT_MONO_A_Y(a));
        int cell = S(LOGIT_GUI_TEXT_MONO_B_CELL(b)); uint32_t color = (uint32_t)LOGIT_GUI_TEXT_MONO_B_COLOR(b);
        char text[USER_TEXT_MAX];
        if (user_copy_string(text, sizeof text, (const char *)c) < 0) return -1;
        /* The GLYPH size scales with the cell. Scaling only the cell would space
         * the same small glyphs further apart -- a terminal with gaps, not a
         * bigger terminal. */
        fb_target(&w->surf); text_draw_mono_sz(x, y, text, fb_ui_px(), cell, color); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_ICON: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = S(LOGIT_GUI_ICON_A_X(a)), y = S(LOGIT_GUI_ICON_A_Y(a));
        int id = LOGIT_GUI_ICON_B_ID(b), px = S(LOGIT_GUI_ICON_B_PX(b));
        if (px < 1 || px > 512) return -1;   /* icon_draw allocates px*px before rasterizing */
        /* Icons are vector paths in a 0..100 box, so a scaled px is a genuinely
         * re-rasterized icon: the one place where "retina" costs nothing at all. */
        fb_target(&w->surf); icon_draw(id, x, y, px, (uint32_t)c); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_GLASS: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = S(LOGIT_GUI_GLASS_A_X(a)), y = S(LOGIT_GUI_GLASS_A_Y(a));
        int gw = S(LOGIT_GUI_GLASS_A_X(a) + LOGIT_GUI_GLASS_B_W(b)) - x;
        int gh = S(LOGIT_GUI_GLASS_A_Y(a) + LOGIT_GUI_GLASS_B_H(b)) - y;
        int radius = S(LOGIT_GUI_GLASS_C_RADIUS(c));
        uint8_t tr = (uint8_t)LOGIT_GUI_GLASS_C_TR(c), tg = (uint8_t)LOGIT_GUI_GLASS_C_TG(c),
                tb = (uint8_t)LOGIT_GUI_GLASS_C_TB(c), ta = (uint8_t)LOGIT_GUI_GLASS_C_TA(c);
        fb_target(&w->surf); fb_liquid_glass(x, y, gw, gh, radius, tr, tg, tb, ta); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_FLUSH: {
        /* Repaint just this app's window -- its rectangle plus its drop shadow.
         * There is no sub-window damage on this call and deliberately no plan
         * for one: the flush carries no rectangle, so the smallest honest
         * extent an app can be held to is its whole canvas. That is the floor
         * on an app repaint, and it is an ABI limit, not a compositor one. */
        struct win *w = app_window(ap);
        if (w) dirty_win(w); else dirty_full();
        return 0;
    }
    case SYS_POLL_EVENT: {
        struct win *w = app_window(ap); if (!w) return 0;
        struct logit_event *ev = (struct logit_event *)a;
        if (!user_range_ok(ev, sizeof *ev, 1)) return -1;
        /* The struct GREW (mods/button/wheel). An app built against the old
         * three-int layout still writes a three-int buffer here, and this would
         * scribble 12 bytes past it -- which is why sizeof is asserted at
         * compile time on both sides and every app in the tree rebuilds from the
         * one header. There is no ABI version negotiation on this call and there
         * is deliberately not going to be one: the .aex files and the kernel ship
         * as a single image. */
        return evq_pop(&w->ev, ev);
    }
    case SYS_GET_ARG: {
        char *buf = (char *)a; int max = (int)b, i = 0;
        if (max <= 0 || !user_range_ok(buf, (uint64_t)max, 1)) return -1;
        for (; i < max - 1 && ap->arg[i]; i++) buf[i] = ap->arg[i];
        buf[i] = 0;
        return i;
    }
    case SYS_GET_TIME: {
        struct rtc_time t; rtc_now(&t);
        if (!user_range_ok((void *)a, sizeof t, 1)) return -1;
        memcpy((void *)a, &t, sizeof t);
        return 0;
    }
    case SYS_READ_FILE: {
        char path[USER_PATH_MAX];
        int max = (int)c;
        if (max < 0 || user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        if (max > 0 && !user_range_ok((void *)b, (uint64_t)max, 1)) return -1;
        return vfs_read(path, (void *)b, max);
    }
    case SYS_YIELD:
        schedule();
        return 0;
    case SYS_SYSINFO:
        if ((int)b <= 0 || !user_range_ok((void *)a, (uint64_t)(int)b, 1)) return -1;
        return sysinfo_text((char *)a, (int)b);
    case SYS_UI_DARK:                       /* a<0 query; else set system dark mode */
        if ((int)a >= 0) wm_set_dark((int)a);
        return g_ui_dark;
    case SYS_FILE_COUNT:
        return vfs_count("/");
    case SYS_FILE_NAME: {
        int i = (int)a;
        if ((int)c <= 0 || !user_range_ok((void *)b, (uint64_t)(int)c, 1)) return -1;
        scopy((char *)b, vfs_ent_name("/", i), (int)c);
        return vfs_ent_size("/", i);
    }
    case SYS_WRITE_FILE: {
        char path[USER_PATH_MAX];
        int size = (int)c;
        if (size < 0 || user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        if (size > 0 && !user_range_ok((const void *)b, (uint64_t)size, 0)) return -1;
        return vfs_write(path, (const void *)b, size);
    }
    case SYS_DELETE_FILE: {
        char path[USER_PATH_MAX];
        if (user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        return vfs_delete(path);
    }
    case SYS_MKDIR: {
        char path[USER_PATH_MAX];
        if (user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        return vfs_mkdir(path);
    }
    case SYS_DIR_COUNT: {
        char path[USER_PATH_MAX];
        if (user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        return vfs_count(path);
    }
    case SYS_DIR_NAME: {
        char dir[USER_PATH_MAX];
        int i = (int)b;
        if (user_copy_string(dir, sizeof dir, (const char *)a) < 0) return -1;
        if (!user_range_ok((void *)c, 64, 1)) return -1;
        if (i < 0 || i >= vfs_count(dir)) return -1;
        scopy((char *)c, vfs_ent_name(dir, i), 64);
        return vfs_ent_is_dir(dir, i) ? -2 : vfs_ent_size(dir, i);
    }
    /* SYS_NET_* moved to syscall.c so CLI processes (no GUI window) can use them. */
    case SYS_HTTP_GET: {
        /* Fetch only (DNS+TCP+TLS+HTTP, follows redirects). The DOM/CSS/layout
         * pipeline now lives in the ring-3 browser. http_get blocks while pumping
         * net_poll, which needs IF=1, but the int 0x80 gate cleared IF -- so
         * re-enable interrupts across the fetch, then restore for the iretq. */
        char url[USER_URL_MAX];
        if (user_copy_string(url, sizeof url, (const char *)a) < 0) return -1;
        __asm__ volatile ("sti");
        g_net_busy = 1;                          /* we own the net; WM thread must not poll */
        net_busy_t0 = timer_ticks();
        uint64_t t0 = timer_ticks();
        int grc = http_get(url);
        /* Retry only what a retry can fix, and bound the whole thing.
         *
         * This used to retry ANY failure three times. For a host that is simply
         * unreachable -- blocked, black-holed, no route -- that is a
         * deterministic failure, so the retries only multiply the damage: each
         * attempt burns tcp_connect's 5 s, times the redirect chain, times
         * three. And because this fetch runs in ring 0 holding the big kernel
         * lock, the WM thread cannot enter the kernel to drain input for the
         * whole of it: the machine appears hung, not merely slow. Reported from
         * a region where www.google.com does not resolve or connect.
         *
         * HTTP_ERR_CONN and HTTP_ERR_URL are verdicts, not glitches. Only DNS
         * and TLS get a second chance, and only one, under a hard wall-clock
         * cap so no combination of redirects and retries can exceed it.
         *
         * This is mitigation, not the fix. The fix is the non-blocking socket
         * layer (c/net/core/sock.c) with the browser driving it from its own
         * event loop, so an unreachable host costs a spinner instead of the
         * machine. Until browser.c is wired to it, bound the damage. */
        for (int retry = 0; retry < 1; retry++) {
            if (grc >= 0 || grc == HTTP_ERR_CONN || grc == HTTP_ERR_URL) break;
            if (timer_ticks() - t0 > HTTP_FETCH_CAP_TICKS) break;
            grc = http_get(url);
        }
        g_net_busy = 0;
        net_busy_t0 = 0;
        kprintf("[http] get rc=%d status=%d t=%dms\n", grc, http_status(), (int)(timer_ticks()-t0)*10);
        __asm__ volatile ("cli");
        return grc;
    }
    case SYS_HTTP_STATUS:
        return http_status();
    case SYS_HTTP_BODY: {
        char *buf = (char *)a; int max = (int)b;
        if (max <= 0 || !user_range_ok(buf, (uint64_t)max, 1)) return -1;
        int blen; const char *body = http_body(&blen);
        if (!body || blen <= 0) return 0;
        int n = blen < max ? blen : max;
        memcpy(buf, body, (size_t)n);
        return n;
    }
    case SYS_TEXT_MEASURE: {
        const char *s = (const char *)a; int len = (int)b;
        int px = (int)((c >> 1) & 0x7FFFFFFF), mono = (int)(c & 1);
        if (len < 0 || len > USER_TEXT_MAX) return 0;
        if (px < 1 || px > 512) return 0;    /* unbounded px overflows the rasterizer's w*h math */
        char tmp[USER_TEXT_MAX];
        if (len > 0) { if (!user_range_ok(s, (uint64_t)len, 0)) return -1; memcpy(tmp, s, (size_t)len); }
        /* Measure at the size it will actually be DRAWN at, then answer in
         * points. Measuring at the unscaled px instead would be self-consistent
         * arithmetic and still wrong: hinting-free advances do not scale exactly
         * linearly, so a caller that word-wraps on the 1x width would overflow
         * its own box once the 1.5x glyphs landed. */
        return PT(text_measure(tmp, len, S(px), mono));
    }
    case SYS_GUI_TEXT_RUN: {
        struct win *w = app_window(ap); if (!w) return -1;
        struct logit_run r;
        if (!user_range_ok((const void *)a, sizeof r, 0)) return -1;
        memcpy(&r, (const void *)a, sizeof r);
        if (r.px < 1 || r.px > 512) return -1;   /* unbounded px overflows the rasterizer's w*h math */
        int len = r.len; if (len < 0) len = 0; if (len > USER_TEXT_MAX - 1) len = USER_TEXT_MAX - 1;
        char tmp[USER_TEXT_MAX];
        if (len > 0) { if (!user_range_ok(r.s, (uint64_t)len, 0)) return -1; memcpy(tmp, r.s, (size_t)len); }
        tmp[len] = 0;
        fb_target(&w->surf);
        text_draw_run(S(r.x), S(r.y), tmp, len, S(r.px), r.mono, r.color);
        fb_target(NULL);
        return 0;
    }
    case SYS_RES_FETCH: {
        char src[USER_URL_MAX];
        if (user_copy_string(src, sizeof src, (const char *)a) < 0) return -1;
        char *buf = (char *)b; int max = (int)c;
        if (max <= 0 || !user_range_ok(buf, (uint64_t)max, 1)) return -1;
        __asm__ volatile ("sti");
        g_net_busy = 1;
        net_busy_t0 = timer_ticks();
        uint8_t *rb = 0; int rl = 0; int rc = res_fetch(src, &rb, &rl);
        g_net_busy = 0;
        net_busy_t0 = 0;
        __asm__ volatile ("cli");
        if (rc != 0 || !rb) { kprintf("[res] fetch FAILED rc=%d url=%s\n", rc, src); return -1; }
        kprintf("[res] ok %d bytes url=%s\n", rl, src);
        int n = rl < max ? rl : max;
        memcpy(buf, rb, (size_t)n);
        kfree(rb);
        return n;
    }
    case SYS_GUI_BLIT: {
        struct win *w = app_window(ap); if (!w) return -1;
        struct logit_blit bl;
        if (!user_range_ok((const void *)a, sizeof bl, 0)) return -1;
        memcpy(&bl, (const void *)a, sizeof bl);
        if (bl.sw <= 0 || bl.sh <= 0 || bl.sw > 4096 || bl.sh > 4096) return -1;
        if (!user_range_ok(bl.rgba, (uint64_t)bl.sw * (uint64_t)bl.sh * 4, 0)) return -1;
        /* dw/dh come straight from the app: fb_blit_rgba clips its loops to the
         * visible target region, but reject non-positive sizes here. */
        if (bl.w <= 0 || bl.h <= 0) return -1;
        /* The DEST rect scales; the source bitmap does not. fb_blit_rgba already
         * rescales nearest-neighbour, so an image simply covers the same logical
         * area. A bitmap cannot gain detail it never had -- unlike the text and
         * icons around it, which do. */
        int bx = S(bl.x), by = S(bl.y);
        fb_target(&w->surf);
        fb_blit_rgba(bx, by, S(bl.x + bl.w) - bx, S(bl.y + bl.h) - by, bl.rgba, bl.sw, bl.sh);
        fb_target(NULL);
        return 0;
    }
    case SYS_GUI_CLIP: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = S(LOGIT_GUI_CLIP_A_X(a)), y = S(LOGIT_GUI_CLIP_A_Y(a));
        int cw2 = LOGIT_GUI_CLIP_B_W(b), ch2 = LOGIT_GUI_CLIP_B_H(b);
        /* The clip is stored ON the window surface, so target it first; this is
         * why a clip set here can't bleed into another app's surface draws. */
        fb_target(&w->surf);
        if (cw2 == 0 && ch2 == 0) fb_clear_clip();
        else fb_set_clip(x, y, S(LOGIT_GUI_CLIP_A_X(a) + cw2) - x,
                               S(LOGIT_GUI_CLIP_A_Y(a) + ch2) - y);
        fb_target(NULL);
        return 0;
    }
    /* Display geometry. Allowed without a window (see the !ap gate above): it is
     * a read-only query about the machine, exactly like SYS_SYSINFO, and a CLI
     * program that wants to know the desktop size has no window by definition. */
    case SYS_SCREEN_INFO:
        switch ((int)a) {
        case SCREEN_W:     return fb_width_pt();
        case SCREEN_H:     return fb_height_pt();
        case SCREEN_SCALE: return fb_scale();
        case SCREEN_DEV_W: return (int)fb_width();
        case SCREEN_DEV_H: return (int)fb_height();
        }
        return -1;
    case SYS_GUI_WIN_MIN: {
        struct win *w = app_window(ap); if (!w) return -1;
        int mw = LOGIT_GUI_WIN_MIN_A_W(a), mh = LOGIT_GUI_WIN_MIN_A_H(a);
        if (mw < 0 || mh < 0) return -1;
        w->min_w_pt = mw;
        w->min_h_pt = mh;
        /* If the window is already smaller than the floor it just declared,
         * grow it NOW rather than waiting for the next drag to notice: an app
         * that says "I need 400 points" and is 200 wide is broken on screen
         * from this instant, and the WM is the only thing that can fix it. */
        int fw, fh;
        win_min_frame(w, &fw, &fh);
        if (w->w < fw || w->h < fh) win_set_frame(w, w->x, w->y, w->w, w->h);
        return 0;
    }
    case SYS_GUI_WIN_STATE: {
        struct win *w = app_window(ap); if (!w) return -1;
        switch ((int)a) {
        case WINS_W:         return w->cw_pt;
        case WINS_H:         return w->ch_pt;
        case WINS_ZOOMED:    return w->zoomed;
        case WINS_MINIMIZED: return w->minimized;
        case WINS_SET_ZOOM:
            win_set_zoom(w, (int)b < 0 ? !w->zoomed : ((int)b != 0));
            return w->zoomed;
        case WINS_SET_MIN:
            win_set_min(w, (int)b != 0);
            return w->minimized;
        }
        return -1;
    }
    case SYS_OPEN_PATH: {
        char path[USER_PATH_MAX];
        if (user_copy_string(path, sizeof path, (const char *)a) < 0) return -1;
        if (ends_aex(path)) wm_launch(path, "");
        else launch_for_ext(ext_of(path), path);
        return 0;
    }
    }
    return -1;
}

/* Called from proc_exit(): mark the current proc's window dead; the WM reaps it.
 * A CLI/forked process has no window (cur_app() == NULL) -- harmless no-op. */
void wm_app_exit(void)
{
    struct app *ap = cur_app();
    if (ap) ap->alive = 0;
    dirty_full();
}

/* The plain form: a window-level event with no button and no wheel (EV_KEY,
 * EV_CLOSE, EV_THEME). Everything mouse-shaped goes through enqueue_input so
 * `mods`/`button`/`wheel` are filled in one place rather than at each call. */
static void enqueue(struct win *w, int type, int a, int b)
{
    struct logit_event e = { type, a, b, 0, EV_BTN_NONE, 0 };
    evq_push(&w->ev, &e);
}

static void enqueue_input(struct win *w, int type, int a, int b, int mods, int button, int wheel)
{
    struct logit_event e = { type, a, b, mods, button, wheel };
    evq_push(&w->ev, &e);
}

/* Flip the system theme: kernel chrome follows immediately (redrawn each frame);
 * apps are nudged with EV_THEME so they re-query SYS_UI_DARK and repaint. */
static void wm_set_dark(int on)
{
    on = on ? 1 : 0;
    if (on == g_ui_dark) return;
    g_ui_dark = on;
    /* --- settings line: remember it. Committed immediately (the 1), because
     * the user flipping the theme is exactly the change they will expect to
     * still be there after a power cut, and one whole-file write is one
     * LogitFS transaction -- atomic, ~8 KiB, and it happens once per click. */
    settings_set_int("ui.dark", on, 1);
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].kind == WK_APP) enqueue(&wins[i], EV_THEME, 0, 0);
    /* Every pixel of chrome changes colour and every app is about to repaint:
     * a full-screen repaint is the CORRECT answer here, and this is the case
     * the damage tracking has to be shown still producing one. */
    dirty_full();
}

/* ---------- reaping dead apps ---------- */
static void reap(void)
{
    for (int i = 0; i < MAXWIN; i++) {
        if (apps[i].used && !apps[i].alive) {
            int wi = apps[i].win;
            if (wi >= 0 && wins[wi].used) {
                if (wins[wi].surf.px) kfree(wins[wi].surf.px);
                /* Clear the pointer, do not merely free what it pointed at.
                 * The slot is reused by the next SYS_GUI_CREATE, and until that
                 * call installs a new buffer the field is a dangling pointer into
                 * a block the heap has already handed to somebody else. Every
                 * reader guards on `used` today, so nothing dereferences it -- but
                 * "nothing does" is a property of several call sites that can
                 * change, and a stale write of this kind is otherwise diagnosed
                 * somewhere else entirely, later, which is precisely the failure
                 * mode the frame poison map exists to stop. Two stores make the
                 * invariant local to the code that ends the surface's life. */
                wins[wi].surf.px = 0;
                wins[wi].surf.w = wins[wi].surf.h = 0;
                wins[wi].surf_cap = 0;   /* ...and the capacity that block held */
                wins[wi].used = 0;
                remove_win(wi);
                if (dragging == wi) dragging = -1;   /* don't drag a reaped (soon reused) slot */
                if (mouse_capture == wi) mouse_capture = -1;   /* ...nor deliver its drag to the slot's next tenant */
                if (rz_win == wi) { rz_win = -1; rz_edge = 0; }   /* ...nor resize it */
            }
            apps[i].used = 0;
        }
    }
}

/* ---------- input deferral (keyboard/mouse IRQ -> WM thread) ----------
 * THE root-cause fix for the long-standing "opening/using an app sometimes
 * hard-freezes the whole system" bug. Keyboard (IRQ1) and PS/2 mouse (IRQ12)
 * fire in interrupt context and used to call wm_mouse_event/wm_key DIRECTLY,
 * which (a) mutate shared WM state -- order[]/wins[]/apps[]/drag -- that the WM
 * thread is simultaneously reading while it composites (wm_render), so a torn
 * read yields a garbage window rect and fb_round_rect/fb_put runs away in a
 * near-infinite pixel loop; and (b) on a dock click run wm_launch, which does
 * disk I/O + kmalloc/pmm/proc/thread creation -- lock-taking, non-reentrant work
 * that deadlocks/corrupts the allocator if the IRQ preempted a thread mid-alloc.
 * Fix: the IRQ now only ENQUEUES the raw input event (no shared-state writes, no
 * locks); the WM thread drains the queue and does ALL processing in thread
 * context (wm_drain_input -> wm_process_mouse/wm_process_key), serialized with
 * its own compositing. This removes the entire IRQ-vs-render race class. */
/* type 0 = mouse (x,y + button levels + wheel notches); 1 = key (x = code).
 * `mods` is sampled HERE, in the IRQ, not when the WM thread drains: the whole
 * point of an event carrying modifiers is that they were the modifiers at the
 * moment the button went down, and the drain can be a frame later. */
struct inev { int type; int x, y, l, r, m, wheel, mods; };
#define INQ_N 512
static struct inev inq[INQ_N];
static volatile int inq_head, inq_tail;
static void inq_push(const struct inev *e)   /* IRQ-safe: no locks, no shared-state */
{
    int nt = (inq_tail + 1) % INQ_N;
    if (nt == inq_head) return;            /* full: drop (cosmetic under flooding) */
    inq[inq_tail] = *e;
    inq_tail = nt;
}
static void wm_process_mouse(const struct inev *e);   /* fwd: bodies below */
static void wm_process_key(int c, int mods);
/* The IRQ entry points (called from mouse.c / keyboard.c) now ONLY enqueue. */
void wm_mouse_event(int x, int y, int left, int right, int middle, int wheel)
{
    struct inev e = { 0, x, y, left, right, middle, wheel, kbd_mods() };
    inq_push(&e);
}
void wm_key(int c)
{
    struct inev e = { 1, c, 0, 0, 0, 0, 0, kbd_mods() };
    inq_push(&e);
}
static void wm_drain_input(void)           /* WM thread: process all input here, NOT in the IRQ */
{
    for (;;) {
        struct inev e;
        __asm__ volatile ("cli");          /* brief: atomic dequeue vs the producing IRQs */
        int empty = (inq_head == inq_tail);
        if (!empty) { e = inq[inq_head]; inq_head = (inq_head + 1) % INQ_N; }
        __asm__ volatile ("sti");
        if (empty) break;
        if (e.type == 0) wm_process_mouse(&e);
        else             wm_process_key(e.x, e.mods);
    }
}

/* ---------- desktop chrome ---------- */
/* The frosted menu bar + dock now blur the live backdrop per-frame via
 * fb_blur_rect (a fast separable box blur, see fb.c) instead of a baked-once
 * O(r^2) box blur -- so windows slid under them read as true frosted glass. */

/* Dusk wallpaper: a soft lavender -> mauve -> warm-sand vertical gradient
 * (a macOS-Tahoe-ish dusk sky). Cached once into `bg`. */
static uint32_t bg_color(int x, int y)
{
    (void)x;
    int t = y * 1000 / H;
    uint8_t r, g, b;
    if (t < 550) {
        r = (uint8_t)lerp(140, 198, t, 550); g = (uint8_t)lerp(150, 168, t, 550); b = (uint8_t)lerp(198, 184, t, 550);
    } else {
        int u = t - 550;
        r = (uint8_t)lerp(198, 228, u, 450); g = (uint8_t)lerp(168, 192, u, 450); b = (uint8_t)lerp(184, 162, u, 450);
    }
    return rgb(r, g, b);
}

/* Load /wallpaper.png (Apple Desktop Picture, packed at build), decode via the
 * kernel image codec, and scale-blit it to fill the screen. 0 if absent/bad so
 * the caller falls back to the gradient. */
static int draw_wallpaper(void)
{
    /* --- settings line: the user's wallpaper, /wallpaper.png if they have not
     * chosen one. A path naming a file that does not exist, or one that is not
     * a decodable image, falls through to the gradient below exactly as a
     * missing /wallpaper.png always did -- so there is no wallpaper setting
     * that can produce a broken desktop, only a plain one. */
    const char *wp = settings_get_str("ui.wallpaper", "/wallpaper.png");
    int sz = vfs_size(wp);
    if (sz <= 0) return 0;
    uint8_t *file = (uint8_t *)kmalloc((unsigned)sz);
    if (!file) return 0;
    int n = vfs_read(wp, file, sz);
    struct image im;
    int ok = (n > 0 && img_decode(file, n, &im) == 0);
    kfree(file);
    if (!ok) return 0;
    fb_blit_rgba(0, 0, W, H, im.rgba, im.w, im.h);          /* scale to fill */
    img_free(&im);
    return 1;
}

/* The wallpaper only -- baked once into `bg`. The menu bar and dock are now
 * composited per-frame on TOP of the windows (draw_menubar/draw_dock) so their
 * real-time blur frosts the live content behind them (true vibrancy), and a
 * window can slide *under* the dock instead of covering it. */
static void draw_background(void)
{
    if (!draw_wallpaper())
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) fb_put(x, y, bg_color(x, y));
}

/* Frosted menu bar, composited per-frame on top of the windows: real-time blur
 * of whatever is behind it (wallpaper or a window slid up under it). */
static void draw_clock(void);
/* The menu bar is a GLASS PANEL: it frosts whatever is behind it, so it is one
 * of the two things a damage rectangle may not cut in half (see dmg_expand). */
static void menubar_box(struct drect *r) { r->x0 = 0; r->y0 = 0; r->x1 = W; r->y1 = MBH; }
/* The clock is the only thing on an idle desktop that changes, and it changes
 * twice a second. That used to be a full-screen recomposite; it is now this. */
static void dirty_menubar(void) { dirty_rect(0, 0, W, MBH); }
static int menu_tog_x, menu_tog_y, menu_tog_w = 38, menu_tog_h = 18;   /* dark-mode switch */
/* menu_tog_* are DEVICE pixels: they are written here and read by the click
 * handler, which sees device mouse coordinates. Keeping the stored rect in the
 * same space as the thing it is tested against is the whole trick -- the
 * alternative (store points, convert at every comparison) is where a scaled UI
 * with unscaled hit-testing comes from. */
static void draw_menubar(void)
{
    /* Liquid Glass menu bar (thin -> adaptive edge band) */
    if (g_ui_dark) fb_liquid_glass(0, 0, W, MBH, S(2), 24, 24, 32, 150);
    else           fb_liquid_glass(0, 0, W, MBH, S(2), 255, 255, 255, 110);
    fb_blend_rect(0, MBH - S(1), W, S(1), 0, 0, 0, g_ui_dark ? 70 : 28);  /* hairline */
    uint32_t ink = g_ui_dark ? rgb(232, 233, 238) : rgb(40, 40, 48);
    fb_fill_circle(S(16), MBH / 2, S(6), ink);
    fb_text(S(32), S(4), "LogitOS", ink);
    fb_text(S(112), S(4), "File", ink);
    fb_text(S(156), S(4), "Edit", ink);
    fb_text(S(200), S(4), "View", ink);
    /* dark-mode toggle switch: track + knob (knob right = dark) */
    menu_tog_w = S(38); menu_tog_h = S(18);
    menu_tog_x = W - S(210); menu_tog_y = (MBH - menu_tog_h) / 2;
    if (g_ui_dark) fb_round_rect(menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h, menu_tog_h / 2, rgb(94, 150, 255));
    else           fb_round_rect(menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h, menu_tog_h / 2, rgb(198, 200, 208));
    int kr = menu_tog_h / 2 - S(2);
    int kx = g_ui_dark ? menu_tog_x + menu_tog_w - kr - S(3) : menu_tog_x + kr + S(3);
    fb_fill_circle(kx, menu_tog_y + menu_tog_h / 2, kr, rgb(255, 255, 255));
    draw_clock();
}

/* Points; DOCK_ISZ/DOCK_GAP are the device-pixel values draw_dock() derives from
 * them each frame and the click handler tests against. */
#define DOCK_ISZ_PT 50
#define DOCK_GAP_PT 14
static int dock_x0, dock_y0, dock_isz = DOCK_ISZ_PT, dock_gap = DOCK_GAP_PT;

/* The dock's geometry, derived rather than remembered.
 *
 * draw_dock used to be the only thing that knew where the dock was, and it
 * published its answer into the globals above on the way past. That is fine
 * while the dock is drawn every frame; it stops being fine the moment a frame
 * may skip it, because the DAMAGE path has to know where the dock is in order
 * to decide whether this frame touches it -- and asking a global that the
 * skipped draw would have written is a loop. One function computes it. */
static int dock_hover_at(int x, int y);         /* body below, with the drawing */
static void dock_geom(int *x0, int *y0, int *dw, int *dh)
{
    int n = nreg < 1 ? 1 : nreg;
    int isz = S(DOCK_ISZ_PT), gap = S(DOCK_GAP_PT);
    *dw = gap + n * (isz + gap);
    *dh = isz + S(20);
    *x0 = (W - *dw) / 2;
    *y0 = H - *dh - S(12);
}

/* The GLASS PANEL: the rounded slab whose frost samples the live backdrop. This
 * is the rectangle a damage rectangle may not cut in half (see dmg_expand); the
 * rest of the dock's footprint is ordinary read-modify-write drawing that clips
 * exactly and needs no such promise. */
static void dock_panel_box(struct drect *r)
{
    int x0, y0, dw, dh;
    dock_geom(&x0, &y0, &dw, &dh);
    r->x0 = x0 - S(2);       r->y0 = y0;
    r->x1 = x0 + dw + S(2);  r->y1 = y0 + dh + S(10);   /* + the drop shadow below */
}

/* The dock's whole footprint, for a given hovered icon (-1 = none): the panel,
 * plus the launch bounce that lifts an icon S(14) out of the top, plus the
 * magnified hover tile, plus the tooltip above it.
 *
 * The tooltip is centred on the icon and as wide as the app's NAME, so it can
 * overhang the panel -- but its extent is EXACTLY KNOWABLE, and asking for it
 * is worth the four lines. The first version damaged the full screen width to
 * cover it, and because the Terminal's window reaches down into the dock, every
 * repaint of that window then grew to a full-width band: 82% of the screen for
 * a keystroke. Being vague about damage is not free; it is paid for by every
 * unrelated rectangle that happens to touch the vague one. */
static void dock_box_hov(int hov, struct drect *r)
{
    int x0, y0, dw, dh;
    dock_geom(&x0, &y0, &dw, &dh);
    dock_panel_box(r);
    if (y0 - S(16) < r->y0) r->y0 = y0 - S(16);        /* bounce + 1.3x hover tile */
    if (hov >= 0 && hov < nreg) {
        int isz = S(DOCK_ISZ_PT), gap = S(DOCK_GAP_PT);
        int ccx = x0 + gap + hov * (isz + gap) + isz / 2;
        int tw = fb_text_width(reg[hov].name);
        struct drect t = { ccx - tw / 2 - S(10), y0 - S(30),
                           ccx + tw / 2 + tw % 2 + S(10), y0 };
        rect_or(r, &t);
    }
}
static void dock_box(struct drect *r) { dock_box_hov(dock_hover_at(mx, my), r); }

/* Damage the dock as it is about to be drawn. A hover CHANGE has to damage the
 * outgoing tooltip as well -- the icon the pointer just left still has one on
 * screen, and it is nowhere near the icon it moved to. */
static void dirty_dock_hov(int hov)
{ struct drect r; dock_box_hov(hov, &r); dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0); }
static void dirty_dock(void) { dirty_dock_hov(dock_hover_at(mx, my)); }

/* Draw one dock tile of side `sz` centered at (cx,cy): glossy gradient + sheen +
 * the app's vector icon (or its letter). Used at base size and, for the hovered
 * icon, magnified -- the corner radius and icon scale track `sz`. */
static void dock_tile(int i, int cx, int cy, int sz)
{
    int x = cx - sz / 2, y = cy - sz / 2, rad = sz * 12 / 50;
    fb_round_rect_vgrad(x, y, sz, sz, rad, fb_shade(reg[i].color, 38), fb_shade(reg[i].color, -26));
    fb_blend_round_rect(x, y, sz, sz / 2, rad, 255, 255, 255, 32);
    int ic = icon_for_app(reg[i].file, reg[i].ext);
    if (ic >= 0) { int isz = sz * 62 / 100; icon_draw(ic, cx - isz / 2, cy - isz / 2, isz, rgb(255, 255, 255)); }
    else { char ch[2] = { reg[i].icon, 0 }; fb_text(cx - S(FW) / 2, cy - S(FH) / 2, ch, rgb(255, 255, 255)); }
}

/* Upward pixel offset of dock icon `i`'s launch bounce (two decaying parabolic
 * hops over ~0.55s). Clears the timer when finished. Integer-only. */
static int dock_bounce_off(int i)
{
    uint64_t t0 = reg_bounce[i];
    if (!t0) return 0;
    uint64_t e = timer_ticks() - t0, DUR = 55, half = DUR / 2;
    if (e >= DUR) { reg_bounce[i] = 0; return 0; }
    int hop = (e < half) ? 0 : 1, H = S(hop ? 7 : 14);
    int u = (int)(e - (uint64_t)hop * half), d = (int)half;   /* 0..d within the arc */
    return H * 4 * u * (d - u) / (d * d);                      /* parabola, peak mid-arc */
}

/* Which dock icon a device-pixel point hovers, or -1.
 *
 * Split out of draw_dock because the INPUT path has to ask it too. With the
 * pointer on a display plane, plain motion no longer repaints anything -- but
 * the dock magnifies the icon under the cursor, so the one kind of motion that
 * genuinely does change the picture is motion that crosses an icon boundary.
 * The compositor and the input path have to agree about where those boundaries
 * are, and the way to guarantee that is to have one function say so.
 *
 * Valid only after the first draw_dock() has published dock_x0/dock_y0/isz/gap;
 * before that they are the point-valued defaults and this returns -1 for the
 * whole screen, which is correct -- there is no dock on screen yet either. */
static int dock_hover_at(int x, int y)
{
    int dh = dock_isz + S(20);
    if (y < dock_y0 || y >= dock_y0 + dh) return -1;
    for (int i = 0; i < nreg; i++) {
        int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap);
        if (x >= ix && x < ix + dock_isz) return i;
    }
    return -1;
}

static void draw_dock(void)
{
    int dw, dh;
    dock_isz = S(DOCK_ISZ_PT); dock_gap = S(DOCK_GAP_PT);   /* device px, for the click path */
    dock_geom(&dock_x0, &dock_y0, &dw, &dh);
    fb_blend_round_rect(dock_x0 - S(1), dock_y0 + S(7), dw + S(2), dh, S(28), 0, 0, 0, 50);   /* soft drop shadow */
    /* Liquid Glass: frost + rim refraction + specular highlight + body tint */
    if (g_ui_dark) fb_liquid_glass(dock_x0, dock_y0, dw, dh, S(28), 26, 26, 34, 104);
    else           fb_liquid_glass(dock_x0, dock_y0, dw, dh, S(28), 255, 255, 255, 44);

    /* Live hover magnification: the icon under the cursor grows in place (kept
     * inside the panel + gap so it never overlaps a neighbour) and shows its name
     * as a tooltip. Re-evaluated every frame; the input path asks dock_hover_at()
     * the same question and requests a frame when the answer changes, which is
     * what still animates this as the cursor sweeps the dock now that plain
     * motion no longer repaints anything. */
    int ccy = dock_y0 + S(10) + dock_isz / 2, animating = 0;
    int hov = dock_hover_at(mx, my);
    for (int i = 0; i < nreg; i++) {
        if (i == hov) continue;                            /* hovered tile drawn last, on top */
        int b = dock_bounce_off(i); if (b) animating = 1;  /* launch bounce lifts the icon */
        int ccx = dock_x0 + dock_gap + i * (dock_isz + dock_gap) + dock_isz / 2;
        dock_tile(i, ccx, ccy - b, dock_isz);
    }
    if (hov >= 0) {
        int b = dock_bounce_off(hov); if (b) animating = 1;
        int ccx = dock_x0 + dock_gap + hov * (dock_isz + dock_gap) + dock_isz / 2;
        dock_tile(hov, ccx, ccy - b, dock_isz * 130 / 100);  /* 1.3x pop */
        const char *nm = reg[hov].name;                    /* tooltip above the dock */
        int tw = fb_text_width(nm), tx = ccx - tw / 2;     /* already device px (scaled font) */
        fb_blend_round_rect(tx - S(9), dock_y0 - S(28), tw + S(18), S(23), S(7), 28, 28, 34, 225);
        fb_text(tx, dock_y0 - S(25), nm, rgb(244, 244, 248));
    }
    if (animating) dirty_dock();                           /* keep compositing while a bounce runs */
}

static int fmt2(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; return 2; }
static void draw_clock(void)
{
    static const char *wd[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    struct rtc_time t; rtc_now(&t);
    char b[32]; int p = 0;
    const char *w = wd[t.weekday % 7];
    b[p++]=w[0]; b[p++]=w[1]; b[p++]=w[2]; b[p++]=' ';
    b[p++]='0'+(t.year/1000)%10; b[p++]='0'+(t.year/100)%10; b[p++]='0'+(t.year/10)%10; b[p++]='0'+t.year%10;
    b[p++]='-'; p+=fmt2(b+p,t.month); b[p++]='-'; p+=fmt2(b+p,t.day);
    b[p++]=' '; b[p++]=' ';
    p+=fmt2(b+p,t.hour); b[p++]=':'; p+=fmt2(b+p,t.minute); b[p++]=':'; p+=fmt2(b+p,t.second);
    b[p]=0;
    fb_text(W - fb_text_width(b) - S(12), S(4), b, g_ui_dark ? rgb(228, 229, 235) : rgb(40, 40, 46));
}

/* The file browser is now the ring-3 Finder app (src/apps/gui/files.c), launched
 * at boot below; the old in-kernel WK_FINDER window was folded into it. */

/* ---------- window frame + compositing ---------- */
/* A soft drop shadow drawn as thin translucent bands hugging the window edges.
 * The window is opaque and overdraws its interior, so blending the *whole*
 * window-sized rounded rect (as fb_blend_round_rect does) was almost all wasted
 * work -- and it ran on every repaint, so a big window like the browser lagged
 * badly on each flush. Bands touch only ~perimeter*thickness pixels (~30x less). */
static void shadow_band(int x, int y, int w, int h, int t, int a)
{
    fb_blend_rect(x - t, y - t, w + 2*t, t, 0, 0, 0, a);   /* top    */
    fb_blend_rect(x - t, y + h, w + 2*t, t, 0, 0, 0, a);   /* bottom */
    fb_blend_rect(x - t, y, t, h, 0, 0, 0, a);             /* left   */
    fb_blend_rect(x + w, y, t, h, 0, 0, 0, a);             /* right  */
}

/* The window body (rounded bg + gradient titlebar + traffic lights + title) drawn
 * at an explicit rect, so it can be rendered into an off-screen surface at origin
 * for the open-pop scale animation as well as straight into `back`. */
static void draw_frame_body(int x, int y, int ww, int wh, const char *title, int focused)
{
    fb_round_rect(x, y, ww, wh, S(10), g_ui_dark ? rgb(30, 30, 36) : rgb(250, 250, 252));
    uint32_t tbtop, tbbot;
    if (g_ui_dark) { tbtop = focused ? rgb(60, 60, 70) : rgb(40, 40, 48); tbbot = focused ? rgb(44, 44, 52) : rgb(34, 34, 40); }
    else           { tbtop = focused ? rgb(246, 246, 250) : rgb(250, 250, 252); tbbot = focused ? rgb(226, 227, 234) : rgb(240, 240, 244); }
    fb_round_rect_vgrad(x, y, ww, TBH, S(10), tbtop, tbbot);
    fb_fill_rect(x, y + TBH, ww, S(1), g_ui_dark ? rgb(60, 60, 70) : rgb(214, 214, 220));
    uint32_t off = g_ui_dark ? rgb(80, 80, 90) : rgb(205, 205, 210);
    fb_fill_circle(x + S(16), y + S(15), S(6), focused ? rgb(255, 95, 86) : off);  /* close */
    fb_fill_circle(x + S(34), y + S(15), S(6), focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + S(52), y + S(15), S(6), focused ? rgb(40, 200, 64) : off);
    int tw = fb_text_width(title);
    fb_text(x + (ww - tw) / 2, y + S(7), title, g_ui_dark ? rgb(210, 211, 218) : rgb(70, 70, 78));
}

/* Normal (non-animating) window frame: a LIQUID GLASS titlebar that frosts +
 * refracts the live backdrop behind the window's top. Drawn while `back` still
 * holds the backdrop (the app's content surface is blitted afterwards and covers
 * the body), so the glass samples real content, not the window's own fill. The
 * glass strip runs TITLEBAR_H+10 tall so its rounded bottom corners hide under
 * the content surface; the top corners round the window. (draw_frame_body, with a
 * solid gradient titlebar, is used only by the open-pop temp render, which has no
 * backdrop to sample.) */
static void draw_frame(struct win *w, int focused)
{
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    shadow_band(x, y, ww, wh, S(8), focused ? 11 : 7);      /* faint outer fringe */
    shadow_band(x, y, ww, wh, S(4), focused ? 22 : 13);     /* mid */
    shadow_band(x, y, ww, wh, S(2), focused ? 40 : 24);     /* dark edge */
    uint8_t a = focused ? (g_ui_dark ? 150 : 104) : (g_ui_dark ? 180 : 140);
    if (g_ui_dark) fb_liquid_glass(x, y, ww, TBH + S(10), S(10), 30, 30, 40, a);
    else           fb_liquid_glass(x, y, ww, TBH + S(10), S(10), 250, 250, 255, a);
    fb_fill_rect(x, y + TBH, ww, S(1), g_ui_dark ? rgb(60, 60, 70) : rgb(214, 214, 220));
    uint32_t off = g_ui_dark ? rgb(80, 80, 90) : rgb(205, 205, 210);
    fb_fill_circle(x + S(16), y + S(15), S(6), focused ? rgb(255, 95, 86) : off);  /* close */
    fb_fill_circle(x + S(34), y + S(15), S(6), focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + S(52), y + S(15), S(6), focused ? rgb(40, 200, 64) : off);
    int tw = fb_text_width(w->title);
    fb_text(x + (ww - tw) / 2, y + S(7), w->title, g_ui_dark ? rgb(210, 211, 218) : rgb(70, 70, 78));
}

static const char *cursor_bmp[] = {
    "#","##","#.#","#..#","#...#","#....#","#.....#","#......#","#.......#",
    "#........#","#.....####","#..#..#","#.# #..#","##  #..#","#    #..#","     #..#","      ##",
};
#define CURSOR_W 16
#define CURSOR_H 20

/* ---- the pointer as a display plane ---------------------------------------
 *
 * Set once, at init, when the display has a cursor plane. Everything else in
 * this file branches on it: wm_render stops drawing the arrow, and plain motion
 * stops setting `dirty`. When it is 0 the file behaves exactly as it did --
 * that is the fallback for a multiboot LFB, and it is why the removal of the
 * old save-under path is not being undone here. There is still no partial
 * rendering and still no save-under; the pointer simply is not in the frame. */
static int hw_cursor;

/* ---- FIVE pointers, one picture each ---------------------------------------
 *
 * An edge you can drag and a cursor that never changes is an edge nobody finds.
 * The resize band is 8 points wide; without feedback a user learns it exists by
 * accident, and every accidental discovery is preceded by a dozen clicks that
 * did nothing.
 *
 * All five live as 64x64 ARGB planes, built once at init, and BOTH pointer
 * paths read the same array -- the display plane hands it to the device, the
 * LFB fallback blits it into the composite. That identity is not tidiness: a
 * plane cursor that is a different picture from the composited one turns every
 * screenshot comparison into an argument about which path drew it.
 *
 * The arrow is the ASCII bitmap it has always been, one CELL per character so
 * it re-rasterizes with the UI scale (a 16-pixel arrow on a 2560-wide display
 * is a mote). The four resize pointers are GENERATED, because hand-drawing a
 * 45-degree double-headed arrow at four scales is exactly the kind of art that
 * is wrong in one of the corners and nobody notices for a year. */
enum { CUR_ARROW, CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW, CUR_NSHAPES };
#define CUR_PLANE 64
static uint32_t cursor_plane[CUR_NSHAPES][CUR_PLANE * CUR_PLANE];
static int cursor_hot[CUR_NSHAPES][2];
static int cursor_box[CUR_NSHAPES][2];   /* w,h actually used, from the hotspot */
static int cur_shape = CUR_ARROW;

/* Rasterize a double-headed arrow along the unit vector (ux,uy), given in /256
 * fixed point, into `dst`. The shape is a coverage TEST rather than a path: for
 * each pixel, project onto the axis (a) and its perpendicular (b), and ask
 * whether it lands in the bar or inside one of the two heads. That is a dozen
 * lines that are right at every angle, and it is why the diagonals cost nothing
 * extra over the axis-aligned pair. */
static void build_arrow(uint32_t *dst, int ux, int uy)
{
    uint32_t o = 0xFF000000u | rgb(20, 20, 26), f = 0xFF000000u | rgb(255, 255, 255);
    const int c = CUR_PLANE / 2;
    int t = S(1); if (t < 1) t = 1;                 /* outline thickness */
    int R = S(11), bar = S(2), hd = S(6), hw = S(6);
    /* The plane is fixed at 64 and the UI scale is not, so clamp rather than
     * write past it: at scale 300 an unclamped R would be 33 and the arrow's
     * far head would wrap onto the opposite row. */
    if (R + t > c - 1) { int m = c - 1 - t; if (hd > m) hd = m; if (hw > m) hw = m; R = m; }
    if (R < 4) R = 4;
    for (int j = 0; j < CUR_PLANE; j++)
        for (int i = 0; i < CUR_PLANE; i++) {
            int px = i - c, py = j - c;
            int a = (px * ux + py * uy) >> 8;
            int b = (-px * uy + py * ux) >> 8;
            int aa = a < 0 ? -a : a, ab = b < 0 ? -b : b;
            int in = (aa <= R - hd && ab <= bar) ||
                     (aa > R - hd && aa <= R && ab * hd <= (R - aa) * hw);
            if (in) dst[j * CUR_PLANE + i] = f;
        }
    /* Outline by dilation. Testing only for the FILL colour is what stops this
     * cascading -- an outline pixel is never a seed for another one, so the
     * ring is exactly `t` thick however many passes the box test spans. */
    for (int j = 0; j < CUR_PLANE; j++)
        for (int i = 0; i < CUR_PLANE; i++) {
            if (dst[j * CUR_PLANE + i]) continue;
            int near = 0;
            for (int dy = -t; dy <= t && !near; dy++)
                for (int dx = -t; dx <= t; dx++) {
                    int y2 = j + dy, x2 = i + dx;
                    if (x2 < 0 || y2 < 0 || x2 >= CUR_PLANE || y2 >= CUR_PLANE) continue;
                    if (dst[y2 * CUR_PLANE + x2] == f) { near = 1; break; }
                }
            if (near) dst[j * CUR_PLANE + i] = o;
        }
}

/* The tight bounding box of a built plane, measured from its hotspot. The LFB
 * fallback damages this and not the whole 64x64: the arrow uses about 16x20 of
 * it at scale 100, and damaging four times the area on every pointer sample
 * would be a regression paid for by the one path that has no plane to hide
 * behind. */
static void cursor_measure(int s)
{
    int x0 = CUR_PLANE, y0 = CUR_PLANE, x1 = 0, y1 = 0;
    for (int j = 0; j < CUR_PLANE; j++)
        for (int i = 0; i < CUR_PLANE; i++)
            if (cursor_plane[s][j * CUR_PLANE + i]) {
                if (i < x0) x0 = i; if (j < y0) y0 = j;
                if (i + 1 > x1) x1 = i + 1; if (j + 1 > y1) y1 = j + 1;
            }
    if (x1 <= x0) { cursor_box[s][0] = cursor_box[s][1] = 0; return; }
    cursor_box[s][0] = x1; cursor_box[s][1] = y1;   /* extents from the plane origin */
}

static void build_cursors(void)
{
    for (int s = 0; s < CUR_NSHAPES; s++) {
        for (int i = 0; i < CUR_PLANE * CUR_PLANE; i++) cursor_plane[s][i] = 0;
        cursor_hot[s][0] = cursor_hot[s][1] = CUR_PLANE / 2;
    }
    /* The arrow, from the ASCII cells. Its tip is the top-left cell, so the
     * hotspot is (0,0) -- the same relationship the composited arrow always
     * had, which is what keeps hit-testing and the reported pointer position
     * identical across the two paths (and what tests/qmp/qmp_ui.py's
     * locate_cursor depends on). */
    uint32_t o = 0xFF000000u | rgb(20, 20, 26), f = 0xFF000000u | rgb(255, 255, 255);
    int rows = (int)(sizeof cursor_bmp / sizeof cursor_bmp[0]);
    for (int r = 0; r < rows; r++) {
        int y0 = S(r), y1 = S(r + 1);
        if (y0 >= CUR_PLANE) break;
        if (y1 > CUR_PLANE) y1 = CUR_PLANE;
        for (int c = 0; cursor_bmp[r][c]; c++) {
            char p = cursor_bmp[r][c];
            if (p != '#' && p != '.') continue;
            uint32_t col = (p == '#') ? o : f;
            int x0 = S(c), x1 = S(c + 1);
            if (x0 >= CUR_PLANE) break;
            if (x1 > CUR_PLANE) x1 = CUR_PLANE;
            for (int j = y0; j < y1; j++)
                for (int i = x0; i < x1; i++) cursor_plane[CUR_ARROW][j * CUR_PLANE + i] = col;
        }
    }
    cursor_hot[CUR_ARROW][0] = cursor_hot[CUR_ARROW][1] = 0;
    /* 181/256 is sin(45 degrees) to within a quarter of a percent, which at a
     * 22-pixel arrow is a tenth of a pixel -- the diagonals are diagonal. */
    build_arrow(cursor_plane[CUR_EW],   256, 0);
    build_arrow(cursor_plane[CUR_NS],   0,   256);
    build_arrow(cursor_plane[CUR_NWSE], 181, 181);
    build_arrow(cursor_plane[CUR_NESW], 181, -181);
    for (int s = 0; s < CUR_NSHAPES; s++) cursor_measure(s);
}

/* Without a cursor plane the arrow is pixels in the frame, so moving it damages
 * where it was and where it now is -- two small rectangles instead of a screen.
 * This is the LFB fallback path; on virtio-gpu the pointer is not in the frame
 * at all and neither of these is ever called. */
static void dirty_cursor(int x, int y)
{
    int s = cur_shape;
    dirty_rect(x - cursor_hot[s][0], y - cursor_hot[s][1],
               cursor_box[s][0] + 1, cursor_box[s][1] + 1);
}

/* The cursor is composited into `back` on top of everything else, then presented
 * with the rest of the frame -- no save-under overlay (that restored a stale
 * `back` and smeared garbage as the cursor moved). */
static void draw_cursor_back(int x, int y)
{
    const uint32_t *p = cursor_plane[cur_shape];
    int hx = cursor_hot[cur_shape][0], hy = cursor_hot[cur_shape][1];
    int bw = cursor_box[cur_shape][0], bh = cursor_box[cur_shape][1];
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            uint32_t v = p[j * CUR_PLANE + i];
            if (v & 0xFF000000u) fb_put(x + i - hx, y + j - hy, v & 0x00FFFFFFu);
        }
}

/* Adopt a pointer shape. Only ever called when the shape CHANGES: on the plane
 * path a redefine is a 64x64 transfer plus a control-queue round trip, which is
 * nothing once but is not free at pointer rate; on the LFB path it is a repaint
 * of where the old picture was. */
static void set_cursor(int s)
{
    if (s < 0 || s >= CUR_NSHAPES || s == cur_shape) return;
    if (!hw_cursor) dirty_cursor(mx, my);        /* erase the outgoing picture */
    cur_shape = s;
    if (hw_cursor)
        fb_cursor_image(cursor_plane[s], CUR_PLANE, CUR_PLANE,
                        cursor_hot[s][0], cursor_hot[s][1]);
    else
        dirty_cursor(mx, my);                    /* ...and draw the incoming one */
}

/* The pointer for a set of grabbed edges. A corner names two edges, and the
 * two diagonals are told apart by whether those edges are on the same side of
 * the window's diagonal: left+top and right+bottom both pull along the NW-SE
 * axis, the other two along NE-SW. */
static int cursor_for_edge(int e)
{
    if (!e) return CUR_ARROW;
    int horiz = (e & (RZ_L | RZ_R)) != 0, vert = (e & (RZ_T | RZ_B)) != 0;
    if (horiz && vert)
        return ((e & RZ_L) && (e & RZ_T)) || ((e & RZ_R) && (e & RZ_B))
               ? CUR_NWSE : CUR_NESW;
    return horiz ? CUR_EW : CUR_NS;
}

/* Open "pop" animation: 0.85 -> 1.0 scale over ~0.14s (easeOutCubic). Returns the
 * scale in /256 (256 = full), or 0 when settled / not animating. */
static uint32_t *anim_buf;
static int anim_buf_n;
static int win_open_scale(struct win *w)
{
    if (!w->open_t0) return 0;
    uint64_t e = timer_ticks() - w->open_t0, DUR = 16;
    if (e >= DUR) { w->open_t0 = 0; return 0; }
    int t = (int)(e * 256 / DUR), inv = 256 - t;
    int eased = 256 - inv * inv * inv / (256 * 256);   /* easeOutCubic */
    return 216 + (256 - 216) * eased / 256;            /* 0.84x -> 1.0x over ~0.16s */
}

/* Grow each damage rectangle until every GLASS PANEL it touches is contained
 * whole, then re-merge.
 *
 * This is the price of frosted chrome, and it is not optional. fb_liquid_glass
 * blurs and refracts the live backdrop UNDER the panel: to compute one output
 * pixel it samples up to ~24 px around it. Inside a damage rectangle that
 * backdrop was just re-laid from the wallpaper; outside it, `back` holds last
 * frame's finished composite -- which already has the glass in it. Frosting a
 * frosted pixel is visibly different from frosting the backdrop, so a panel cut
 * by a damage rectangle grows a seam along the cut. Containing the panel
 * removes the possibility rather than hiding it.
 *
 * The re-merge afterwards is not tidiness either: two rectangles that both grew
 * to contain the dock would each draw the dock, and the second would do it over
 * pixels the first had already presented. */
static int dmg_expand(struct drect *r, int n)
{
    int grew = 1;
    for (int pass = 0; pass < 8 && grew; pass++) {
        grew = 0;
        for (int i = 0; i < n; i++) {
            struct drect p;
            menubar_box(&p);
            if (rect_hit(&r[i], &p) && !rect_in(&p, &r[i])) { rect_or(&r[i], &p); grew = 1; }
            dock_panel_box(&p);          /* the SLAB, not the whole footprint */
            if (rect_hit(&r[i], &p) && !rect_in(&p, &r[i])) { rect_or(&r[i], &p); grew = 1; }
            for (int k = 0; k < norder; k++) {           /* each window's glass titlebar */
                struct win *w = &wins[order[k]];
                if (!w->used || w->minimized) continue;  /* not on screen, no glass */
                p.x0 = w->x; p.y0 = w->y;
                p.x1 = w->x + w->w; p.y1 = w->y + TBH + S(10);
                if (rect_hit(&r[i], &p) && !rect_in(&p, &r[i])) { rect_or(&r[i], &p); grew = 1; }
            }
        }
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; )
                if (rect_hit(&r[i], &r[j])) { rect_or(&r[i], &r[j]); r[j] = r[--n]; grew = 1; }
                else j++;
    }
    /* Still growing after eight passes means the expansion did not converge,
     * and a panel is probably still cut. Say "whole screen" rather than ship a
     * seam: the fallback has to be the SAFE answer, not the fast one. */
    return grew ? -1 : n;
}

/* Does this rectangle contain any part of a window that is HALF DRAWN?
 *
 * If it does, compositing it here and now would put an erased or partly
 * repainted window on the display. The honest answer is not to draw the
 * rectangle at all: `back` still holds the last complete composite of it, and a
 * rectangle that is not composited is not presented either, so the display
 * keeps the picture it already has until the app flushes.
 *
 * A minimized window is skipped for the same reason render_region skips it --
 * its canvas is not on screen, so it cannot tear.
 *
 * `*late` is set when a window is over its deadline and is being let through
 * anyway. It is an OUT-PARAMETER rather than a counter bumped in here because
 * this runs once per damage rectangle per frame: counting inside would report
 * how often the question was ASKED, which is a number nobody can act on. */
#if WM_MIDFRAME_GUARD
static int rect_blocked(const struct drect *R, uint64_t now, int *late)
{
    for (int i = 0; i < norder; i++) {
        struct win *w = &wins[order[i]];
        if (!w->used || w->minimized || !w->drawing || !w->surf.px) continue;
        struct drect b; win_box(w, &b);
        if (!rect_hit(&b, R)) continue;
        if (now - w->draw_t0 >= WM_MIDFRAME_MAX_MS) { *late = 1; continue; }
        return 1;
    }
    return 0;
}
#endif

/* Composite ONE damage rectangle: wallpaper, every window that reaches into it,
 * the frosted chrome that overlaps it, and (without a cursor plane) the arrow --
 * all clipped to it -- then push exactly that rectangle to the display.
 *
 * Every layer is drawn in the same order it always was, and the rectangle is
 * re-laid from the wallpaper up: that is what makes a partial frame produce the
 * same pixels a full one would. Returns 1 if an animation wants another frame. */
static int render_region(const struct drect *R)
{
    int animating = 0;
    int rw = R->x1 - R->x0, rh = R->y1 - R->y0;
    fb_target(NULL);
    fb_set_clip(R->x0, R->y0, rw, rh);
    for (int y = R->y0; y < R->y1; y++)    /* wallpaper (baked in bg) */
        blit(back + (long)y * W + R->x0, bg + (long)y * W + R->x0, rw);
    int focus_wi = top_visible();          /* NOT order[norder-1]: that may be minimised */
    for (int i = 0; i < norder; i++) {     /* windows, back-to-front */
        struct win *w = &wins[order[i]];
        if (!w->used || w->minimized) continue;
        struct drect b; win_box(w, &b);
        if (!rect_hit(&b, R)) continue;    /* nothing of this window is in the rect */
        int focused = (order[i] == focus_wi);
        int s = win_open_scale(w);         /* open pop in progress? */
        if (s) {
            int need = w->w * w->h;
            if (need > anim_buf_n) { if (anim_buf) kfree(anim_buf); anim_buf = kmalloc((unsigned)need * 4); anim_buf_n = anim_buf ? need : 0; }
            if (anim_buf) {
                animating = 1;
                struct surface tmp = { .px = anim_buf, .w = w->w, .h = w->h };  /* clip_on = 0: scratch is unclipped */
                fb_target(&tmp);           /* render the whole window into scratch... */
                draw_frame_body(0, 0, w->w, w->h, w->title, focused);
                blit_content(w, 0, TBH, w->w, w->h - TBH);
                fb_target(NULL);
                int dw = w->w * s / 256, dh = w->h * s / 256;     /* ...scale to back */
                fb_blit_surface_scaled(w->x + (w->w - dw) / 2, w->y + (w->h - dh) / 2, dw, dh, &tmp);
                continue;
            }
        }
        draw_frame(w, focused);
        /* The canvas is allowed to be a size behind the frame mid-drag, so the
         * content is stretched rather than left as a hole -- see blit_content
         * and RESIZE_APPLY_MS. */
        blit_content(w, w->x, w->y + TBH, w->w, w->h - TBH);
    }
    { struct drect p; menubar_box(&p);                  /* frosted chrome ON TOP: */
      if (rect_hit(&p, R)) draw_menubar(); }            /* real-time vibrancy      */
    { struct drect p; dock_box(&p);
      if (rect_hit(&p, R)) draw_dock(); }
    /* WM-HOOK 4/6: the notification overlay -- above every window, below the
     * pointer. No rect_hit guard: the fb clip is already this rectangle and
     * every primitive notify_compose uses is clip-exact (it is deliberately not
     * glass, so it needs no entry in dmg_expand). See notify.h. */
    notify_compose();
    if (!hw_cursor) draw_cursor_back(mx, my);   /* no plane: arrow into the composite */
    fb_clear_clip();

    uint64_t t_pres = time_mono_ns();
    fb_present_rect(R->x0, R->y0, rw, rh);
    perf_present_ns += time_mono_ns() - t_pres;
    perf_cpx += (uint64_t)rw * (uint64_t)rh;
    return animating;
}

/* Composite the damage -- background + windows + frosted chrome + the cursor --
 * into `back`, then present it. A frame is one pass per damage rectangle, or a
 * single whole-screen pass when the damage is a full repaint (a theme flip, a
 * new window, a z-order change) or has grown past three quarters of the screen.
 *
 * `back` is a correct composite of the ENTIRE screen when this returns -- see
 * the invariant at the top of the file. Nothing else in here is allowed to be
 * true only sometimes. */
void wm_render(void)
{
    reap();
    fb_target(NULL);
    if (!back || !bg) return;              /* wm_init OOM fallback: nothing to composite into */

    /* An open "pop" rescales a whole window every frame and mutates its own
     * animation state as it reads it; one whole-screen pass per frame is both
     * cheaper than tracking that and impossible to get subtly wrong. It lasts
     * about a sixth of a second. */
    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used && wins[i].open_t0) dirty_all = 1;

    struct drect r[NDMG];
    int nr = 0, full = dirty_all || ndmg == 0;
    if (!full) {
        for (int i = 0; i < ndmg; i++) r[i] = dmg[i];
        nr = dmg_expand(r, ndmg);
        if (nr < 0) full = 1;
    }
    if (full) { r[0].x0 = 0; r[0].y0 = 0; r[0].x1 = W; r[0].y1 = H; nr = 1; }
    /* Clear BEFORE drawing: damage recorded from here on (a dock bounce still
     * running, an app flushing from another thread) belongs to the NEXT frame,
     * not to the one being composited. */
    dirty_all = 0; ndmg = 0;

    uint64_t t_start = time_mono_ns();
    int animating = 0;
#if WM_MIDFRAME_GUARD
    struct drect defer[NDMG];
    int ndef = 0, late = 0;
    uint64_t now_ms = time_mono_ms();
#endif
    for (int k = 0; k < nr; k++) {
#if WM_MIDFRAME_GUARD
        if (rect_blocked(&r[k], now_ms, &late)) {
            defer[ndef++] = r[k];
            perf_defer++;
            continue;
        }
#endif
        animating |= render_region(&r[k]);
    }
    if (animating) dirty_full();           /* keep compositing until the pop settles */
#if WM_MIDFRAME_GUARD
    if (late) perf_late++;
    /* Put the held-back rectangles back on the list AFTER the frame, never
     * before: DROPPING DAMAGE IS HOW STALE PIXELS HAPPEN, and this path obeys
     * that rule exactly as hard as every other caller in this file. It re-arms
     * `dirty`, so the WM loop comes straight back -- on the next timer tick at
     * the latest -- by which time the app has usually flushed. */
    for (int i = 0; i < ndef; i++)
        dirty_rect(defer[i].x0, defer[i].y0,
                   defer[i].x1 - defer[i].x0, defer[i].y1 - defer[i].y0);
#endif

    uint64_t dt = time_mono_ns() - t_start;
    perf_composites++;
    perf_comp_ns += dt;
    perf_rects += (uint64_t)nr;
    if (full) perf_full++;
    if (dt > perf_comp_ns_max) perf_comp_ns_max = dt;
}

/* ---------- input ---------- */
static int in_rect(int px, int py, int x, int y, int w, int h)
{ return px >= x && px < x + w && py >= y && py < y + h; }

/* ---- system shortcuts ------------------------------------------------------
 *
 * THE CLAIM RULE, stated once and enforced in one place: the window manager
 * intercepts a CLOSED LIST of Cmd combinations before the focused app sees
 * them, and forwards everything else -- including Cmd combinations not on the
 * list, with EV_MOD_SUPER set so an app can use them.
 *
 * An app cannot take a claimed one back, and that asymmetry is the point. A
 * shortcut any app can swallow is not a system shortcut: the first text field
 * that treats Cmd+W as "delete word" leaves a window the keyboard cannot
 * close. The compensating promise is that the list is CLOSED and small, and
 * that Cmd is a modifier nothing in this tree had already spent -- so no app
 * loses a keystroke it used to receive, and "which Cmd keys are mine" has an
 * answer an app author can read rather than discover.
 *
 * Returns 1 if the WM consumed the key.
 *
 *   Cmd+W          close the focused window
 *   Cmd+Q          quit the focused app  (see below)
 *   Cmd+M          minimise the focused window
 *   Cmd+Tab        next window           Cmd+Shift+Tab  previous
 *   Cmd+`          next window OF THE FOCUSED APP
 *
 * Cmd+Q sends EV_CLOSE to every window the focused app owns, which today is
 * exactly one -- SYS_GUI_CREATE refuses a second -- so Q and W currently do the
 * same thing to the same pixels. They are still two shortcuts, because the
 * distinction is one of SCOPE and not of implementation: the day an app opens a
 * second window they diverge without either being rewritten. Naming them the
 * same shortcut today would be the choice that has to be undone later.
 *
 * It is deliberately NOT a kill. Terminating a process from outside its own
 * control flow lives in c/kernel/exec/proc.c and belongs to another line; a
 * window manager that reached in there to make Cmd+Q feel stronger would be
 * coupling a keystroke to process teardown it does not own. */
static int wm_shortcut(int c, int mods)
{
    int wi = top_visible();
    struct win *w = wi >= 0 ? &wins[wi] : NULL;
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';      /* Cmd+Shift+W is still close */

    switch (c) {
    case 'w':
        if (w && w->kind == WK_APP) { enqueue(w, EV_CLOSE, 0, 0); return 1; }
        return 1;                                     /* claimed even with no window */
    case 'q':
        if (w && w->app) {
            struct app *ap = w->app;
            for (int i = 0; i < MAXWIN; i++)
                if (wins[i].used && wins[i].kind == WK_APP && wins[i].app == ap)
                    enqueue(&wins[i], EV_CLOSE, 0, 0);
        }
        return 1;
    case 'm':
        if (w) win_set_min(w, 1);
        return 1;
    case '\t': {
        /* NO ON-SCREEN SWITCHER, and the reason is a missing event rather than
         * a missing panel. A hold-to-browse switcher has to know when Cmd comes
         * UP -- that is what commits the choice -- and this machine has no such
         * event: the keyboard driver enqueues make codes for mapped keys only,
         * and a modifier produces no wm_key() at all. Inventing one is a change
         * to the input path three other lines are building on this week.
         *
         * So each press switches immediately, and the two directions are exact
         * inverses: forward raises the BOTTOM-most window (a rotation that
         * reaches every window, unlike "raise the one below the top", which
         * ping-pongs between the top two forever and can never reach a third),
         * backward sends the top one to the bottom. Press forward n times with
         * n windows and you are back where you started. */
        if (norder < 2) return 1;
        if (mods & EV_MOD_SHIFT) {
            int top = top_visible();
            if (top >= 0) lower_win(top);
        } else {
            int bot = -1;
            for (int i = 0; i < norder; i++) {
                struct win *cw = &wins[order[i]];
                if (cw->used) { bot = order[i]; break; }
            }
            if (bot >= 0) { win_set_min(&wins[bot], 0); raise_win(bot); }
        }
        dirty_full();      /* a z-order change re-stacks every overlap on screen */
        return 1;
    }
    case '`':
    case '~':
        /* Rotate among the windows of ONE app. With SYS_GUI_CREATE refusing a
         * second window per app this is a no-op on today's data, and it is
         * written against the model rather than against the data on purpose:
         * the alternative is a shortcut that silently does nothing on the day
         * the model changes, which is a bug nobody goes looking for. */
        if (w && w->app) {
            struct app *ap = w->app;
            for (int i = 0; i < norder; i++) {
                struct win *cw = &wins[order[i]];
                if (cw->used && cw->app == ap && order[i] != wi) {
                    win_set_min(cw, 0);
                    raise_win(order[i]);
                    dirty_full();
                    return 1;
                }
            }
        }
        return 1;
    }
    return 0;      /* not ours: the app gets it, with EV_MOD_SUPER set */
}

static void wm_process_key(int c, int mods)
{
    if ((mods & EV_MOD_SUPER) && wm_shortcut(c, mods)) return;
    int wi = top_visible();       /* NOT order[norder-1]: that may be minimised */
    if (wi < 0) return;
    struct win *w = &wins[wi];
    if (w->kind == WK_APP) {
        /* `a` is unchanged -- Ctrl+S still arrives as 0x13, because a decade of
         * terminal habit lives on that mapping and TextEdit reads it. `mods` is
         * additional information, not a replacement encoding. */
        enqueue_input(w, EV_KEY, c, 0, mods, EV_BTN_NONE, 0);
        dirty_win(w);
    }
}

/* Topmost app window whose CONTENT area (below the titlebar) contains (x,y),
 * or -1. Pointer events that are not part of a drag go to what is visibly under
 * the cursor, which is not necessarily the focused window -- that is what makes
 * :hover work on a background window the way it does everywhere else. */
static int win_content_at(int x, int y)
{
    for (int i = norder - 1; i >= 0; i--) {
        struct win *w = &wins[order[i]];
        if (!w->used || w->minimized || w->kind != WK_APP) continue;
        if (in_rect(x, y, w->x, w->y + TBH, w->w, w->h - TBH)) return order[i];
    }
    return -1;
}

/* The pointer shape the CURRENT situation calls for. During a resize it is the
 * drag's own shape and nothing else may change it -- a drag that walks the
 * pointer over another window's edge must not switch cursors halfway through,
 * because the gesture in progress is still the one the user started. */
static void update_cursor_shape(int x, int y)
{
    if (rz_win >= 0) { set_cursor(cursor_for_edge(rz_edge)); return; }
    if (dragging >= 0 || mouse_capture >= 0) { set_cursor(CUR_ARROW); return; }
    int e = 0;
    int wi = resize_hit(x, y, &e);
    set_cursor(wi >= 0 ? cursor_for_edge(e) : CUR_ARROW);
}

/* Two presses on the SAME window's titlebar, close together in time and in
 * space. The space test earns its place as much as the time one: a fast hand
 * clicking two different controls is not double-clicking either of them.
 *
 * A third click does not read as a second double-click -- the state is cleared
 * on a hit -- because otherwise holding a key down on a mouse and clicking four
 * times would zoom, restore, zoom, restore. */
static int titlebar_double_click(int wi, int x, int y)
{
    static int last_wi = -1, last_x, last_y;
    static uint64_t last_ms;
    uint64_t now = time_mono_ms();
    int slop = S(6);
    int hit = (last_wi == wi && now - last_ms <= 400 &&
               x - last_x <= slop && last_x - x <= slop &&
               y - last_y <= slop && last_y - y <= slop);
    last_wi = hit ? -1 : wi;
    last_x = x; last_y = y; last_ms = now;
    return hit;
}

static void wm_process_mouse(const struct inev *in)
{
    int x = in->x, y = in->y, left = in->l, right = in->r, middle = in->m;
    int mods = in->mods;
    int moved = (x != mx || y != my);
    int old_hov = dock_hover_at(mx, my);   /* before the pointer moves */
    int omx = mx, omy = my;                /* ...and where the arrow was */
    mx = x; my = y;
    if (moved) perf_motions++;

    if (left && !mleft && in_rect(x, y, menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h)) {
        wm_set_dark(!g_ui_dark);             /* menu-bar dark-mode switch (on top of all) */
        mleft = left; mright = right; mmiddle = middle;
        return;
    }
    /* WM-HOOK 6/6: a notification is chrome drawn on top of everything, so like
     * the switch above it must win the click -- otherwise the window under it
     * swallows the dismissal and the card cannot be got rid of. Same idiom as
     * the dark-mode switch: consume the press, keep the button levels in step,
     * return. See notify.h. */
    if (left && !mleft && notify_click(x, y)) {
        mleft = left; mright = right; mmiddle = middle;
        return;
    }
    if (left && !mleft) {
        /* The dock is chrome drawn ON TOP of every window (hover tooltip already
         * resolves it regardless of overlap), so it must win the click too:
         * checking windows first lets a tall window (e.g. Code Studio, whose
         * bottom edge reaches into the dock strip) silently SWALLOW dock clicks
         * as content clicks -- the icon you see is not the icon you hit. */
        int docked = 0;
        for (int i = 0; i < nreg; i++) {
            int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap), iy = dock_y0 + 10;
            if (in_rect(x, y, ix, iy, dock_isz, dock_isz)) {
                wm_launch(reg[i].file, "");
                dirty_dock();            /* the launch bounce starts in the dock */
                docked = 1; break;
            }
        }
        /* A RESIZE GRAB OUTRANKS EVERYTHING BELOW IT, including the titlebar
         * drag and the app's own content, because the band overlaps both. It
         * loses to the dock and the menu-bar switch above for the same reason
         * those beat a window: they are drawn on top, and the thing you can see
         * is the thing you must hit. */
        int rz_e = 0, rz_hit = docked ? -1 : resize_hit(x, y, &rz_e);
        if (rz_hit >= 0) {
            struct win *w = &wins[rz_hit];
            int prev_top = top_visible();
            raise_win(rz_hit);
            dirty_win(w);
            if (prev_top >= 0 && prev_top != rz_hit && wins[prev_top].used)
                dirty_win(&wins[prev_top]);
            rz_win = rz_hit; rz_edge = rz_e;
            rz_x0 = w->x; rz_y0 = w->y; rz_x1 = w->x + w->w; rz_y1 = w->y + w->h;
            rz_mx = x; rz_my = y;
            rz_apply_ms = time_mono_ms();
            docked = 1;                  /* consumed: fall past the window hit-test */
        }
        if (!docked) {
        int hitorder = -1;
        for (int i = norder - 1; i >= 0; i--) {
            struct win *w = &wins[order[i]];
            if (w->used && !w->minimized && in_rect(x, y, w->x, w->y, w->w, w->h)) { hitorder = i; break; }
        }
        if (hitorder >= 0) {
            int wi = order[hitorder];
            struct win *w = &wins[wi];
            /* Raising repaints BOTH windows, and forgetting the second one is
             * the classic partial-render bug: the window that just lost focus
             * still shows its coloured traffic lights, in its own titlebar,
             * nowhere near the rectangle that was damaged. */
            int prev_top = top_visible();
            raise_win(wi);
            dirty_win(w);
            if (prev_top >= 0 && prev_top != wi && wins[prev_top].used)
                dirty_win(&wins[prev_top]);
            int cx = x - w->x, cy = y - w->y;
            if (cy < TBH) {
                /* The three lights, at last all three doing something. The
                 * yellow and green ones have been drawn since M8 and have been
                 * decoration for as long -- which is worse than absent, because
                 * a control that looks like a control and does nothing teaches
                 * a user that this desktop's chrome is a picture of a desktop. */
                int r2 = S(8) * S(8);
                int dyc = cy - S(15);
                if ((cx - S(16)) * (cx - S(16)) + dyc * dyc <= r2) {
                    if (w->kind == WK_APP) enqueue(w, EV_CLOSE, 0, 0);  /* close */
                } else if ((cx - S(34)) * (cx - S(34)) + dyc * dyc <= r2) {
                    win_set_min(w, 1);                                  /* minimise */
                } else if ((cx - S(52)) * (cx - S(52)) + dyc * dyc <= r2) {
                    win_set_zoom(w, !w->zoomed);                        /* zoom */
                } else if (titlebar_double_click(wi, x, y)) {
                    /* Double-click the titlebar: the same toggle as the green
                     * light. Two ways in for one action, because the light is
                     * discoverable and the double-click is what a hand already
                     * trained on another desktop will try first. */
                    win_set_zoom(w, !w->zoomed);
                } else {
                    dragging = wi; drag_dx = cx; drag_dy = cy;
                }
            } else if (w->kind == WK_APP) {
                enqueue_input(w, EV_MOUSE, PT(cx), PT(cy - TBH), mods, EV_BTN_LEFT, 0);
                mouse_capture = wi;     /* this window owns the pointer until the button comes up */
            }
        }
        }
    }
    if (right && !mright) {
        for (int i = norder - 1; i >= 0; i--) {
            struct win *w = &wins[order[i]];
            if (!w->used || !in_rect(x, y, w->x, w->y, w->w, w->h)) continue;
            int cx = x - w->x, cy = y - w->y;
            if (cy >= TBH && w->kind == WK_APP) {
                int prev_top = norder ? order[norder - 1] : -1;
                raise_win(order[i]);    /* focus + bring to front (like left-click) so the menu shows on top */
                dirty_win(w);
                if (prev_top >= 0 && prev_top != order[i] && wins[prev_top].used)
                    dirty_win(&wins[prev_top]);
                enqueue_input(w, EV_MOUSE_R, PT(cx), PT(cy - TBH), mods, EV_BTN_RIGHT, 0);
                mouse_capture = order[i];
            }
            break;
        }
    }
    /* Middle press. It does NOT raise the window: a middle click is "do the
     * thing under the pointer" (paste, open-in-background), and raising would
     * hide the thing it was aimed at. Delivered as EV_MOUSE with button=MIDDLE
     * -- see the EV_BTN_* note in logit_abi.h for why there is no EV_MOUSE_M. */
    if (middle && !mmiddle) {
        int wi = win_content_at(x, y);
        if (wi >= 0) {
            struct win *w = &wins[wi];
            enqueue_input(w, EV_MOUSE, PT(x - w->x), PT(y - w->y - TBH), mods, EV_BTN_MIDDLE, 0);
            mouse_capture = wi;
            /* No damage: a middle press deliberately does not raise, so the WM
             * changed nothing. Whatever the app does about it arrives as its
             * own SYS_GUI_FLUSH, which is the honest report of that change. */
        }
    }

    /* Releases. Every press that reached an app gets its matching EV_MOUSE_UP,
     * delivered to the window that CAPTURED the press even if the pointer has
     * since left it -- so window-local coordinates here can be negative or past
     * the window's size, and that is information (how far outside the drag went),
     * not an error to clamp away. An app that never sees the up is an app stuck
     * mid-drag forever, which is the bug this exists to prevent. */
    {
        /* All three, not the first one found: a packet reports every button's
         * level at once, so letting go of two buttons together is one packet and
         * a chain of else-ifs would swallow the second release. */
        int was[3] = { mleft, mright, mmiddle }, now[3] = { left, right, middle };
        int id[3] = { EV_BTN_LEFT, EV_BTN_RIGHT, EV_BTN_MIDDLE };
        for (int k = 0; k < 3; k++) {
            if (!was[k] || now[k]) continue;
            int wi = mouse_capture >= 0 ? mouse_capture : win_content_at(x, y);
            if (wi < 0 || !wins[wi].used || wins[wi].kind != WK_APP) continue;
            struct win *w = &wins[wi];
            enqueue_input(w, EV_MOUSE_UP, PT(x - w->x), PT(y - w->y - TBH), mods, id[k], 0);
        }
    }
    if (!left && !right && !middle) mouse_capture = -1;   /* all buttons up: release the pointer */

    if (!left) {
        dragging = -1;
        /* Letting go ends the resize AND forces the canvas to catch up, with no
         * throttle. Everything else about the drag is best-effort by design;
         * the FINAL size is not, because that is the size the app keeps. */
        if (rz_win >= 0) { rz_win = -1; rz_edge = 0; wm_apply_sizes(); }
    }
    /* A drag changes TWO regions: where the window was and where it now is.
     * Reporting only the destination is what leaves a window-shaped hole
     * trailing behind the drag, and it is the single most visible way to get
     * damage tracking wrong. */
    if (dragging >= 0 && left) {
        struct win *w = &wins[dragging];
        int nx = x - drag_dx, ny = y - drag_dy;
        if (nx != w->x || ny != w->y) win_break_zoom(w);   /* movement, not the press */
        win_set_frame(w, nx, ny, w->w, w->h);
    }

    /* The resize drag. The new frame is computed from the ANCHOR, not from the
     * previous sample: accumulating deltas drifts, and a resize that drifts
     * will not return to the size it started at when the hand returns to where
     * it started -- which is the one thing a user checks when they overshoot.
     *
     * Both the clamp to the minimum and the clamp to the screen are applied by
     * win_set_frame, but the LEFT and TOP edges need their anchor adjusted here
     * as well: clamping the width alone while dragging the left edge would pin
     * x and grow the window to the RIGHT, which is the opposite of the edge
     * under the hand. */
    if (rz_win >= 0 && left && wins[rz_win].used) {
        struct win *w = &wins[rz_win];
        int nx0 = rz_x0, ny0 = rz_y0, nx1 = rz_x1, ny1 = rz_y1;
        if (rz_edge & RZ_L) nx0 = rz_x0 + (x - rz_mx);
        if (rz_edge & RZ_R) nx1 = rz_x1 + (x - rz_mx);
        if (rz_edge & RZ_T) ny0 = rz_y0 + (y - rz_my);
        if (rz_edge & RZ_B) ny1 = rz_y1 + (y - rz_my);
        int mw, mh;
        win_min_frame(w, &mw, &mh);
        if (nx1 - nx0 < mw) { if (rz_edge & RZ_L) nx0 = nx1 - mw; else nx1 = nx0 + mw; }
        if (ny1 - ny0 < mh) { if (rz_edge & RZ_T) ny0 = ny1 - mh; else ny1 = ny0 + mh; }
        if ((rz_edge & RZ_T) && ny0 < MBH) ny0 = MBH;
        if (nx0 != w->x || ny0 != w->y || nx1 - nx0 != w->w || ny1 - ny0 != w->h)
            win_break_zoom(w);        /* movement, not the press -- see win_break_zoom */
        win_set_frame(w, nx0, ny0, nx1 - nx0, ny1 - ny0);
    }
    if (rz_win >= 0 && (!wins[rz_win].used || !left)) { rz_win = -1; rz_edge = 0; }

    /* Motion. Goes to the capture target while a button is held, else to
     * whatever window is visibly under the pointer -- including an unfocused
     * one, which is what makes :hover behave. Suppressed while the WM itself is
     * dragging a titlebar: the pointer belongs to the compositor then, and
     * feeding the app a stream of moves it must ignore is just ring pressure.
     *
     * This is the flood the ring's coalescing exists for: one PS/2 packet per
     * motion sample, an app that polls once per painted frame, and no upper
     * bound on how far apart those two rates can drift. evq_push merges
     * consecutive moves, so motion occupies at most ONE slot and can never evict
     * a queued click. */
    if (moved && dragging < 0 && rz_win < 0) {
        int wi = mouse_capture >= 0 ? mouse_capture : win_content_at(x, y);
        if (wi >= 0 && wins[wi].used && wins[wi].kind == WK_APP) {
            struct win *w = &wins[wi];
            enqueue_input(w, EV_MOUSE_MOVE, PT(x - w->x), PT(y - w->y - TBH), mods, EV_BTN_NONE, 0);
        }
    }

    /* Wheel. Same routing as motion; `wheel` is notches, not pixels, and is
     * never coalesced -- three notches are three notches and merging them would
     * silently shorten the scroll. */
    if (in->wheel) {
        int wi = mouse_capture >= 0 ? mouse_capture : win_content_at(x, y);
        if (wi >= 0 && wins[wi].used && wins[wi].kind == WK_APP) {
            struct win *w = &wins[wi];
            enqueue_input(w, EV_WHEEL, PT(x - w->x), PT(y - w->y - TBH), mods, EV_BTN_NONE, in->wheel);
            /* Same as the middle press: the scroll is the app's to draw. */
        }
    }

    mleft = left;
    mright = right;
    mmiddle = middle;

    /* THE POINT OF THIS WORK.
     *
     * Motion used to imply a recomposite unconditionally, because the arrow was
     * pixels in the frame. It is a display plane now, so moving it changes
     * nothing the compositor drew -- with one real exception: the dock magnifies
     * the icon under the pointer, so motion that crosses an icon boundary DOES
     * change the picture. That is a content change that happens to be caused by
     * motion, and it is asked for by name rather than by repainting the screen
     * on the chance that it happened.
     *
     * Everything else that a moving pointer can change is already covered: an
     * app that highlights on hover receives EV_MOUSE_MOVE and flushes, and a
     * flush sets `dirty` itself; a titlebar drag moves a window and sets
     * `content`. Without a plane (LFB fallback) the arrow is still in the frame,
     * so motion still means a recomposite -- the old behaviour, unchanged. */
    if (moved) {
        if (!hw_cursor) { dirty_cursor(omx, omy); dirty_cursor(x, y); }
        else {
            int hov = dock_hover_at(x, y);
            if (hov != old_hov) { dirty_dock_hov(old_hov); dirty_dock_hov(hov); }
        }
    }
    /* Last, and after the drag state above has settled, because the shape is a
     * function of that state: asking before the release has been processed
     * would leave a resize pointer standing over a window nobody is resizing.
     * This runs on every packet but set_cursor is a no-op unless the shape
     * actually changed, so a sweep across a window costs two device commands,
     * not one per sample. */
    update_cursor_shape(x, y);
}

/* ---------- registry + init ---------- */
static void scan_apps(void)
{
    int n = vfs_count("/");
    for (int i = 0; i < n && nreg < MAXWIN; i++) {
        char nm[64];
        scopy(nm, vfs_ent_name("/", i), sizeof nm);
        if (!ends_aex(nm)) continue;
        /* logitfs reads whole-file (errors if the buffer is smaller), so size the
         * buffer to the file -- the JS app's .aex is ~1 MiB, far over any header
         * scratch. We only need the header, but read it all then free. */
        int sz = vfs_size(nm);
        if (sz < AEX_HDR_SIZE) continue;    /* aex_info reads the 64-byte header */
        char *buf = kmalloc(sz);
        if (!buf) continue;
        if (vfs_read(nm, buf, sz) <= 0) { kfree(buf); continue; }
        char name[32], ext[8];
        if (aex_info(buf, name, ext) == 0) {
            struct aex_header *h = (struct aex_header *)buf;
            scopy(reg[nreg].file, nm, sizeof reg[nreg].file);
            scopy(reg[nreg].name, name, sizeof reg[nreg].name);
            scopy(reg[nreg].ext, ext, sizeof reg[nreg].ext);
            reg[nreg].icon = h->icon ? (char)h->icon : reg[nreg].name[0];
            static const uint8_t pal[7][3] = {
                {80,140,255},{55,200,120},{255,92,92},{255,170,40},
                {170,110,255},{40,200,220},{255,120,170} };
            reg[nreg].color = (h->icon_r || h->icon_g || h->icon_b)
                ? rgb(h->icon_r, h->icon_g, h->icon_b)
                : rgb(pal[nreg % 7][0], pal[nreg % 7][1], pal[nreg % 7][2]);
            nreg++;
        }
        kfree(buf);
    }
}

void wm_init(void)
{
    W = (int)fb_width();
    H = (int)fb_height();
    int count = W * H;
    uint64_t pages = ((uint64_t)count * 4 + 4095) / 4096;
    back = (uint32_t *)pmm_alloc_contig(pages);
    bg   = (uint32_t *)pmm_alloc_contig(pages);
    if (!back || !bg) {
        /* OOM: bail instead of handing wm_render a NULL buffer to blit from/to.
         * screen.px stays NULL, so all fb_put/fb_blit_surface draws no-op safely;
         * the machine keeps running (blank screen) rather than faulting here. */
        kprintf("[wm] init: out of memory (%ux%u)\n", fb_width(), fb_height());
        back = bg = NULL;
        return;
    }
    fb_set_backbuffer(back);
    mx = W / 2; my = H / 2;

    /* --- settings line: the theme the user last chose ----------------------
     * Set directly, not through wm_set_dark(): there are no windows yet to be
     * nudged with EV_THEME and nothing has been composited, so the first frame
     * is simply drawn in the right theme rather than drawn light and repainted.
     * settings_get_int() range-checks, so a settings file saying `ui.dark =
     * banana` lands on 0 here and says so on the serial log. */
    g_ui_dark = settings_get_int("ui.dark", 0) ? 1 : 0;
    /* The one line that says what "the resolution" now means. A test that wants
     * to assert the scale reads this off the serial log; a human reading a
     * screenshot argument needs it to know whether 1920x1200 is four times the
     * desk space or the same desk space at 2.25x the pixels. */
    kprintf("[wm] display %ux%u px, scale %d%%, desktop %dx%d pt\n",
            fb_width(), fb_height(), fb_scale(), fb_width_pt(), fb_height_pt());

    /* Ask the display for a pointer plane. Everything downstream branches on
     * the answer, and the answer is worth a line of its own: "the pointer is
     * free to move" and "every mouse sample repaints the screen" are the same
     * desktop from a screenshot, and this is the only place they differ. */
    build_cursors();
    hw_cursor = fb_cursor_image(cursor_plane[CUR_ARROW], CUR_PLANE, CUR_PLANE,
                                cursor_hot[CUR_ARROW][0], cursor_hot[CUR_ARROW][1]) == 0;
    if (hw_cursor) fb_cursor_move(mx, my);
    kprintf("[wm] pointer: %s\n",
            hw_cursor ? "display cursor plane (motion does not composite)"
                      : "composited into the frame (no cursor plane)");

    scan_apps();
    /* The dock's contents, in the order they are drawn.
     *
     * The dock is CENTRED, so adding one app moves every icon half a slot and
     * every hard-coded coordinate in every QMP driver lands on the wallpaper --
     * which does nothing at all and looks exactly like the app failing to open.
     * tests/qmp/qmp_ui.py warns about this in a comment and then carries the
     * app count as a constant, which went stale the day a settings app was
     * packed (10 -> 11) and silently broke the dock coordinates of every driver
     * in the tree.
     *
     * A constant that has to be maintained in step with the disk image is not a
     * fact, it is a hope. This is the fact, from the thing that scanned the
     * disk, and a driver that reads it cannot rot. */
    kprintf("[wm] dock %d apps:", nreg);
    for (int i = 0; i < nreg; i++) kprintf(" %s", reg[i].file);
    kprintf("\n");

    /* The Finder is now the ring-3 file-manager app, launched in wm_run(). */
    draw_background();          /* wallpaper -> bg; menu bar + dock are per-frame now */
    blit(bg, back, count);
}

void wm_render_first(void) { wm_render(); }

/* Push the pointer to the display plane, and tell the outside world where it
 * ended up.
 *
 * ONE command per loop iteration, not one per PS/2 packet: a drain can hand us
 * a dozen motion samples and only the last position is on screen, so the rest
 * would be a dozen virtqueue round-trips producing an arrow nobody ever saw.
 *
 * The serial line is not decoration. With the pointer on a plane it is no
 * longer in the scanout, so a screendump cannot be asked where the cursor is --
 * and a QMP `rel` event is a delta the harness can only dead-reckon from, which
 * on a large desktop silently stops short and reads as a hit-testing bug in
 * whatever is being tested. This is the guest's own account, printed once when
 * the pointer SETTLES (so a sweep is one line, not five hundred). */
static void wm_pointer_sync(void)
{
    static int cx = -1, cy = -1, reported = 1;
    static uint64_t settle_ms;
    if (mx != cx || my != cy) {
        cx = mx; cy = my;
        if (hw_cursor) {
            uint64_t t0 = time_mono_ns();
            fb_cursor_move(cx, cy);
            perf_cursor_ns += time_mono_ns() - t0;
            perf_cursor_moves++;
        }
        settle_ms = time_mono_ms();
        reported = 0;
    }
    if (!reported && time_mono_ms() - settle_ms >= 100) {
        kprintf("[wm] ptr %d %d\n", cx, cy);
        reported = 1;
    }
}

/* What the windows on this machine actually are, printed when they SETTLE.
 *
 * The same argument as `[wm] ptr` above, for the same reason. A test that wants
 * to know whether a drag on the bottom-right corner resized a window can either
 * ask the guest or go hunting through a screendump for the traffic lights and
 * work backwards through S() -- and the second one is pixel archaeology that
 * breaks the day the titlebar is restyled. It is also unable to see the two
 * numbers that matter most here, the CONTENT size and the zoom state, because
 * neither is a colour anywhere on screen.
 *
 * Settled, not live: a resize drag is a stream of geometries and only the one
 * the hand stopped on is a fact. 150 ms of quiet, then one line per changed
 * window. That silence is load-bearing -- this console is also /bin/sh's
 * stdout, and a compositor that narrated every pointer sample would interleave
 * itself into `make test-shell`'s expected bytes. */
struct gsnap { int x, y, w, h, z, m, cw, ch; };

static void wm_geom_report(void)
{
    /* TWO snapshots, and the difference between them is the whole function.
     * `seen` is what the windows were on the previous pass and answers "has
     * anything moved since I last looked"; `said` is what has been printed and
     * answers "is this news". Collapsing them into one array -- comparing
     * against what was last REPORTED -- makes `changed` true forever the moment
     * anything moves, because the thing it is compared against only advances
     * when a report happens, which the same flag is preventing. */
    static struct gsnap seen[MAXWIN], said[MAXWIN];
    static int inited, pending;
    static uint64_t quiet_ms;
    int changed = 0;
    for (int i = 0; i < MAXWIN; i++) {
        struct win *w = &wins[i];
        struct gsnap c = { w->used ? w->x : -1, w->used ? w->y : -1,
                           w->used ? w->w : -1, w->used ? w->h : -1,
                           w->zoomed, w->minimized, w->cw_pt, w->ch_pt };
        if (c.x != seen[i].x || c.y != seen[i].y || c.w != seen[i].w ||
            c.h != seen[i].h || c.z != seen[i].z || c.m != seen[i].m ||
            c.cw != seen[i].cw || c.ch != seen[i].ch) { seen[i] = c; changed = 1; }
    }
    /* An empty desktop is the state this starts in, not a transition into it. */
    if (!inited) { inited = 1; for (int i = 0; i < MAXWIN; i++) said[i] = seen[i]; return; }
    if (changed) { quiet_ms = time_mono_ms(); pending = 1; return; }
    if (!pending || time_mono_ms() - quiet_ms < 150) return;
    pending = 0;
    for (int i = 0; i < MAXWIN; i++) {
        struct win *w = &wins[i];
        if (seen[i].x == said[i].x && seen[i].y == said[i].y &&
            seen[i].w == said[i].w && seen[i].h == said[i].h &&
            seen[i].z == said[i].z && seen[i].m == said[i].m &&
            seen[i].cw == said[i].cw && seen[i].ch == said[i].ch) continue;
        said[i] = seen[i];
        int x = seen[i].x, y = seen[i].y, ww = seen[i].w, wh = seen[i].h;
        int z = seen[i].z, m = seen[i].m;
        if (!w->used) { kprintf("[wm] win %d gone\n", i); continue; }
        /* Both units on one line, deliberately. The frame is device pixels
         * because that is what a screendump is measured in; the content size is
         * points because that is what the app was told and what it draws in.
         * A reader who has to convert between them is a reader who will one day
         * convert with the wrong scale. */
        kprintf("[wm] win %d frame %d %d %d %d content %d %d pt zoom %d min %d %s\n",
                i, x, y, ww, wh, w->cw_pt, w->ch_pt, z, m, w->title);
    }
}

/* One line per second of ACTIVITY, and silence otherwise.
 *
 * Silence is the load-bearing half. This console is also `/bin/sh`'s stdout --
 * `make test-shell` reads command output off it -- so a compositor that
 * chattered once a second would interleave itself into another test's expected
 * bytes. An idle desktop composites twice a second to move the clock and moves
 * the pointer never, so "did a hand touch this machine" is a question the
 * counters can answer: report only when the pointer moved, or when composites
 * ran far above the idle 2 Hz floor. */
static void wm_perf_report(void)
{
    static uint64_t next_ms, last_comp, last_mot, last_torn, last_def;
    uint64_t ms = time_mono_ms();
    if (ms < next_ms) return;
    next_ms = ms + 1000;
    uint64_t dc = perf_composites - last_comp, dm = perf_motions - last_mot;
    last_comp = perf_composites; last_mot = perf_motions;
    /* A torn frame or a held-back rectangle is activity too, and it is the
     * activity this report exists for: the idle gate must not swallow the one
     * line that says a window was composited half drawn. */
    uint64_t dt_torn = perf_torn - last_torn, dt_def = perf_defer - last_def;
    last_torn = perf_torn; last_def = perf_defer;
    if (dm == 0 && dc <= 20 && dt_torn == 0 && dt_def == 0) return;   /* idle */
    kprintf("[wm] perf t=%lu composites=%lu ns=%lu max=%lu motions=%lu curmoves=%lu "
            "curns=%lu full=%lu rects=%lu cpx=%lu fpx=%lu presns=%lu "
            "torn=%lu defer=%lu late=%lu drawmax=%lu\n",
            (unsigned long)ms, (unsigned long)perf_composites,
            (unsigned long)perf_comp_ns, (unsigned long)perf_comp_ns_max,
            (unsigned long)perf_motions, (unsigned long)perf_cursor_moves,
            (unsigned long)perf_cursor_ns,
            (unsigned long)perf_full, (unsigned long)perf_rects,
            (unsigned long)perf_cpx, (unsigned long)fb_present_px(),
            (unsigned long)perf_present_ns,
            (unsigned long)perf_torn, (unsigned long)perf_defer,
            (unsigned long)perf_late, (unsigned long)perf_drawmax);
}

void wm_run(void)
{
    __asm__ volatile ("mov $0x10, %%ax\n\tmov %%ax,%%ds\n\tmov %%ax,%%es\n\t"
                      "mov %%ax,%%fs\n\tmov %%ax,%%gs" ::: "ax");
    serial_puts("\n[wm] desktop live; launching apps as ring-3 processes\n");

    sched_init();
    smp_mark_sched_ready();   /* release parked APs into the scheduler now the ring exists */

    /* SMP BKL discipline: the WM is a ring-0 thread that does kernel work (the
     * compositor) directly, so it must hold the BKL while doing it (vs APs that
     * touch fb via syscalls). Enter the kernel-held state. */
    spin_lock(&g_bkl);
    this_cpu()->in_kernel = 1;

    /* The WM runs as a ring-0 thread; it MUST keep interrupts enabled so the
     * timer/mouse/keyboard keep firing even when no app is running (otherwise
     * closing the last app would leave nothing with IF=1 and freeze input).
     * A timer IRQ that fires here hits in_kernel=1 -> nested -> EOI+return only
     * (no re-acquire, no re-schedule); the BKL is dropped around the idle hlt
     * below so a non-nested timer IRQ can schedule app threads. */
    __asm__ volatile ("sti");

    /* One window at boot: the Finder. The comment that used to sit here said the
     * clock was launched "so something is alive on screen", which stopped being
     * a reason the moment the Finder was launched above it -- after that the
     * clock was just a second window every user had to close. A desktop opens
     * with the file manager and nothing else. */
    wm_launch("files.aex", "");

    /* init: launch the shell on the serial console (stdin/stdout/stderr = tty) */
    { char *sh_argv[] = { "sh", 0 }; proc_spawn("/bin/sh", sh_argv); }

    uint64_t last = 0;
    for (;;) {
#ifdef WM_CHURN_STRESS
        /* Churn stress (make CHURN=1): hammer the real wm_launch + EV_CLOSE
         * close path -- the repro harness for the app-churn heap corruption.
         * Heartbeat "[s]" to serial every 32 steps; if it stops, we froze. */
        {
            /* terminal-heavy: it fork+execve's /bin/sh over two pipes; closing it
             * orphans the sh -> exercises fork/exec/pipe/orphan-reap teardown. */
            static const char *FILE[3] = { "terminal.aex", "clock.aex", "monitor.aex" };
            static const char *NAME[3] = { "Terminal", "Clock", "Monitor" };
            static int sk; static uint64_t snext; static unsigned scnt;
            uint64_t tnow = timer_ticks();
            if (tnow >= snext) {
                snext = tnow + 4;           /* ~40ms/step */
                int k = sk++ % 3;
                struct app *liv = find_live_app(NAME[k]);   /* match by NAME, not filename */
                if (liv && liv->win >= 0) enqueue(&wins[liv->win], EV_CLOSE, 0, 0);
                else if (!liv) wm_launch(FILE[k], "");
                if ((++scnt & 31) == 0) serial_puts("[s]\n");
            }
        }
#endif
        wm_drain_input();             /* process ALL keyboard/mouse input here, NOT in the IRQ */
        /* Canvases catch up with frames HERE, once per pass -- not inside the
         * drain. A drain can hand us twenty pointer packets and every one of
         * them moves the frame; reallocating a 9 MB canvas twenty times to
         * arrive at one size is the difference between a resize that is
         * throttled and a resize that is unbounded. See RESIZE_APPLY_MS. */
        wm_apply_sizes();
        wm_pointer_sync();            /* one cursor-plane command per loop, not per packet */
        proc_reap();                  /* free zombie processes (GUI apps + orphans) */
        notify_tick();                /* WM-HOOK 5/6: expire notifications (see notify.h) */
        /* net busy watchdog: a fetch legitimately blocks for seconds, but if its
         * thread died mid-fetch the flag is stuck -- expire it after 100 s. */
        if (g_net_busy && net_busy_t0 && timer_ticks() - net_busy_t0 > 10000) {
            g_net_busy = 0; net_busy_t0 = 0;
            serial_puts("[wm] net_busy watchdog expired\n");
        }
        if (!g_net_busy) net_poll();  /* drive RX -- unless a blocking fetch owns the net */
        uint64_t now = timer_ticks();
        /* Composite on DAMAGE, and on nothing else. The ~2 Hz tick no longer
         * asks for a frame -- it says what changed (the clock, in the menu bar)
         * and lets the same path handle it as everything else. An idle desktop
         * therefore repaints a 24-point strip twice a second instead of the
         * whole screen, and there is no periodic full repaint left to quietly
         * cover for a caller that under-reported its damage. */
        if (now - last >= 50) { last = now; dirty_menubar(); }
        if (dirty) {
            dirty = 0;
            wm_render();
        }
        wm_perf_report();
        wm_geom_report();     /* window frames, when they stop moving */
        /* Idle until the next interrupt instead of spinning schedule(): the timer
         * IRQ (100 Hz) preempts + round-robins the app threads, and mouse/keyboard
         * IRQs wake us immediately. This stops the whole system busy-spinning --
         * critical under QEMU's TCG, where every emulated spin-iteration costs host
         * CPU. `sti; hlt` is the race-free idle idiom.
         * SMP: DROP the BKL around the idle hlt so a timer IRQ on the BSP arrives
         * NON-nested -> acquires the BKL -> schedule()s an app thread (which runs,
         * eventually preempted back here). The compositor work above runs holding
         * the BKL (safe vs APs touching fb via syscalls). */
        /* BOTH the release window (in_kernel=0 .. spin_unlock) and the re-acquire
         * window (spin_lock .. in_kernel=1) must run with IF=0, or a timer IRQ in
         * either gap reads nested=0 and re-acquires the BKL this core holds ->
         * self-deadlock (the flaky whole-system freeze). `hlt` returns via iretq
         * with IF=1, so cli AFTER hlt too; re-enable IF for the loop body at the end. */
        __asm__ volatile ("cli");
        this_cpu()->in_kernel = 0;
        spin_unlock(&g_bkl);
        __asm__ volatile ("sti\n\thlt\n\tcli");
        spin_lock(&g_bkl);
        this_cpu()->in_kernel = 1;
        __asm__ volatile ("sti");
    }
}
