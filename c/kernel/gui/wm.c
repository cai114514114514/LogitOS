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
#include "elf.h"        /* struct elf_image: wm_launch reports what the loader did */
#include "pcache.h"     /* the app's own file, so two instances share their text */
#include "blkdev.h"
#include "img.h"
#include "gfx.h"        /* Open Logit: build_arrow's fill+stroke, below */
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
#include "vfs_cred.h"   /* vfs_cred_session(): who, if anyone, has logged in */
/* Path-qualified for the same reason syscall.c and file.c are: mini-libc
 * ships a sys/wait.h that sorts first in INCDIRS. */
#include "kernel/core/wait.h"   /* SYS_WAIT_EVENT: an idle app sleeps */
#include "power.h"      /* kernel_poweroff/kernel_reboot -- the LogitOS menu's Shut Down/Restart */

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
/* Declared HERE rather than down in the damage section it belongs to, because
 * `struct win` now stores one (see anim_prev) and a member cannot have an
 * incomplete type. The damage machinery that owns it is still the only thing
 * that manipulates it; this is a forward move, not a second home. */
struct drect { int x0, y0, x1, y1; };          /* half-open, DEVICE pixels */

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
    /* Whoever is blocked in SYS_WAIT_EVENT on this window. One queue per
     * window, not one for the machine: waking every app because one of them
     * got a keystroke is the same waste as polling, moved into the kernel. */
    struct waitq evwq;
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
    /* ---- the dock fly (minimise / restore) --------------------------------
     * `minimized` is set the INSTANT the gesture starts, not when the flight
     * lands: focus, hit-testing and the keyboard must stop treating the window
     * as present the moment the user asks for that, and only the PICTURE is
     * allowed to take 180 ms to agree. win_draw_rect below reads min_t0 first
     * for exactly that reason -- a window in flight is hidden by state and
     * visible by animation at the same time, and that is not a contradiction.
     *
     * THE APP KEEPS RUNNING. This is not process freezing and there is nothing
     * here that stops a thread: a minimised app still gets its timer slice,
     * still draws into its retained canvas, still has its network sockets. The
     * only thing that changed is that the compositor stops putting that canvas
     * on the screen. Freezing a process because its window is hidden would
     * silently break every app that does work in the background, and this WM
     * has no business making that decision for them. */
    uint64_t min_t0;          /* tick the dock fly began (0 = settled) */
    int  min_dir;             /* 1 = flying to the dock, 0 = flying back out */
    int  min_slot;            /* dock icon index the flight is aimed at */
    /* The on-screen box this window occupied on the previous animated frame.
     * An animation's damage is (where it was | where it now is), and the first
     * half of that is not derivable from the current tick -- so it is kept.
     * Seeded when an animation STARTS; a zeroed one would union in the origin
     * and quietly turn every animated frame into a top-left-anchored repaint. */
    struct drect anim_prev;
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

/* ===========================================================================
 * THE LOCK -- the desktop does not come up until somebody has authenticated.
 *
 * /bin/login gave the SERIAL CONSOLE a person. This file was the half that had
 * not been done: wm_run() launched files.aex before init ran at all, so the
 * machine reached a live desktop showing the previous user's home in 2.7
 * seconds with nothing in between. The console login was real and the desktop
 * went around it.
 *
 * WHAT IS ACTUALLY BEING RELIED ON, in one sentence: while `g_locked` is set,
 * wm_launch() REFUSES to start any program except the greeter. Everything else
 * here -- not drawing the dock, not drawing the menu bar, not drawing a window
 * frame, routing every key to the one window -- is what makes the screen look
 * right, and none of it is what makes the machine safe. That distinction is
 * the reason a greeter was chosen over an overlay on a running desktop: an
 * overlay's security is its z-order, and a launcher's refusal cannot be lost
 * to a compositing mistake. See the header of c/apps/gui/greeter.c.
 *
 * WHEN IT IS SET: at boot, if and only if /etc/passwd exists. A machine with
 * no accounts has nobody to authenticate and behaves EXACTLY as it did before
 * this existed -- which is also what keeps the other sixty boot harnesses in
 * this tree working unchanged.
 *
 * WHEN IT CLEARS: when the LOGIN SESSION becomes non-root. That is checked
 * against c/fs/vfs_cred.c rather than against a message from the greeter,
 * because it is the same fact the filesystem is enforcing against -- there is
 * no second notion of "logged in" here to drift out of step with the first.
 *
 * A CONSEQUENCE, STATED RATHER THAN HIDDEN: authenticating on the serial
 * console also unlocks the desktop. There is one seat, one session and one
 * /etc/passwd; the console asks for the same password with the same PBKDF2, so
 * this is the same person arriving through the other door, not a bypass. If
 * this machine ever grows two seats, the session stops being a global and this
 * check becomes per-seat.
 * ======================================================================== */
#define GREETER_AEX "/sbin/greeter.aex"
static int g_locked;                   /* 1 = nobody has authenticated yet   */
static int g_greeter_ai = -1;          /* apps[] slot of the greeter, or -1  */
static int g_desktop_started;          /* files.aex has been launched (once) */

/* The greeter's WINDOW, derived from the app slot every time rather than
 * cached: a window index outlives the window that owned it (the slot is reused
 * by the next SYS_GUI_CREATE), and a stale one here would composite whatever
 * opened next full-screen with no chrome. */
static int greeter_win(void);

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
/* struct drect is defined above struct win -- see the note there. */
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

/* ===========================================================================
 * MOTION -- Expose, and the dock fly.
 *
 * THE ONE RULE THIS WHOLE SECTION IS BUILT ON: a window that is not where its
 * frame says it is must still have exactly ONE answer to "where are you on
 * screen", and every reader has to get that same answer. There are three
 * readers and they fail in three different ways when they disagree:
 *
 *   win_box()      -> dirty_win() -> the damage list.  Disagreeing here leaves
 *                     the window's previous position standing on the wallpaper.
 *   rect_blocked() -> the mid-frame guard.  Disagreeing here holds back the
 *                     wrong rectangle, or fails to hold back the right one.
 *   render_region()-> the pixels.  Disagreeing here is the picture itself.
 *
 * So win_draw_rect() below is the single definition site, in the same spirit
 * as WSH_* (one shadow geometry) and dock_geom() (one dock position). The
 * animations do not each carry their own copy of "and also damage this" --
 * they move a rectangle, and the rest of the file follows it for free.
 *
 * WHY TICKS AND NOT MILLISECONDS. Every animation here is driven off
 * timer_ticks(), the same 100 Hz PIT counter the dock's launch bounce already
 * reads. That is deliberate copying, not laziness: the bounce is a shipped,
 * working per-frame animation on this machine, its cadence is known to be
 * survivable under TCG, and an animation subsystem with its own timer would be
 * a second thing that can be out of step with the compositor's idea of a frame.
 * 10 ms of resolution over a 180 ms gesture is 18 steps, which is more than the
 * eye resolves in a motion that short.
 *
 * NO FLOATS ANYWHERE. The kernel builds -msse2 now, but the easing is integer
 * because these curves are evaluated per window per frame and a fixed-point
 * quadratic is exact, reproducible, and diffable in a screenshot test. */

/* 180 ms at 100 Hz. Long enough to read as motion rather than a cut, short
 * enough that it never becomes the thing you are waiting for. */
#define EX_DUR_TICKS    18
#define MINFLY_TICKS    18
#define OPEN_DUR_TICKS  16      /* the open pop, as it always was */

#define EX_GUTTER_PT    26      /* between cells, and to the screen edge */
#define EX_TITLE_PT     20      /* reserved under each cell for the title */
/* How dark the desktop goes behind the grid. The windows have to read as THE
 * CONTENT and the wallpaper as backdrop; at this depth a photograph is still
 * legible underneath, which is what says "your desktop is still there" rather
 * than "you are in a different application". */
#define EX_DIM_ALPHA    120
/* THE HOT CORNER, in points from the top-right. Reachable without aiming --
 * the pointer clamps at (W-1, 0), so shoving it up and right always lands
 * inside -- and 18pt is small enough that crossing the corner on the way to
 * somewhere else does not sit in it. */
#define EX_CORNER_PT    18
#define EX_DWELL_TICKS  15      /* 150 ms parked before it fires */

/* Quadratic ease-out over 0..256: fast out of the gate, settling into the end.
 * Same curve family as gfx_shadow_falloff, and the same reason -- deceleration
 * is what makes a moving rectangle look like it has mass instead of being
 * teleported in equal steps. */
static int ease_out(int t)
{
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    int inv = 256 - t;
    return 256 - inv * inv / 256;
}

/* ---- Expose ---------------------------------------------------------------
 *
 * `ex_on` is the INTENT (the mode is up, or is being entered); `ex_t0` is the
 * transition in flight. Both together, because leaving is a state where the
 * intent is already "off" while the pixels are still on their way home --
 * ex_state() is the question every other reader actually wants to ask. */
static int ex_on;                      /* the picker is up (or arriving)      */
static uint64_t ex_t0;                 /* tick the transition began; 0 = still */
static int ex_hov = -1;                /* grid slot under the pointer, or -1  */
static int ex_n;                       /* windows in the grid                 */
static int ex_wi[MAXWIN];              /* wins[] index per grid slot          */
static struct drect ex_cell[MAXWIN];   /* the settled thumbnail rect per slot */

static int ex_state(void) { return ex_on || ex_t0 != 0; }

/* THE TRIGGER. Two of them, and the reasoning for each is the input machinery
 * that already exists rather than a preference.
 *
 * A HOT CORNER, because the WM already tracks the pointer continuously and
 * already runs a per-pass tick (the dock's launch bounce is driven from it), so
 * "has the pointer been parked in the top-right for 150 ms" costs one compare
 * per loop pass and no new machinery at all. Top-RIGHT specifically: the menu
 * bar's left end is the app menu region and its right end is the clock, which
 * is text -- nothing there is clickable, so a pointer resting in that corner is
 * not on its way to anything. The dwell is what stops a pointer merely CROSSING
 * the corner from firing it, and `armed` is what stops it re-firing while the
 * pointer stays parked after the mode is dismissed.
 *
 * AND Cmd+E, because wm_shortcut already owns a closed, documented list of Cmd
 * chords (W/Q/M/Tab/`) and E was free -- so this costs one case label and takes
 * no keystroke away from any app. A gesture that only exists as a hot corner is
 * one a keyboard cannot reach. */
static uint64_t ex_corner_t0;          /* tick the pointer parked in the corner */
static int ex_corner_armed = 1;        /* leave the corner to re-arm the trigger */

/* Where a window IS on screen this frame -- position, size and opacity -- or 0
 * if it is not on screen at all. PURE: it reads the clock, it never expires a
 * timer. (win_box calls it from the input path, and an animation that advanced
 * itself because somebody asked where a window was would be a timer nobody can
 * reason about. wm_anim_tick() is the one place a timer ends.) */
static int win_draw_rect(const struct win *w, int *ox, int *oy, int *ow, int *oh, int *oa);

/* THE DROP SHADOW'S GEOMETRY, defined once.
 *
 * It is used in two places that must agree -- draw_frame paints it, win_box
 * declares the damage it occupies -- and when they disagree the symptom is a
 * dark ghost of the window's previous position. That is not a hypothetical:
 * win_box used to hardcode S(8) because draw_frame's widest band happened to be
 * S(8), and nothing connected the two numbers. Anyone enlarging the shadow
 * would have shipped the ghost.
 *
 * An unfocused window sits lower: less offset, less blur, less opacity. That is
 * the whole depth cue, and it is why focus is legible from across the room. */
/* RETUNED (unit F, "the geometry the numbers indict"): the gate measured the
 * old constants (DY 8/2, BLUR 18/9, ALPHA 130-62/90-40) at 14px of falloff and
 * ~21% peak darkening on a FOCUSED window -- tight and shallow next to a real
 * macOS window shadow, which reads as noticeably wider, deeper and lower-
 * offset. The ceiling on BLUR is not taste: fb_shadow's corner tile is
 * `blur + radius` on a side, radius here is the window's own S(10) corner, and
 * gfx_mask_corner refuses any tile past GFX_MASK_MAX=72 device px (fb.c
 * pre-clamps rather than refuse, silently tightening the shadow past the
 * ceiling -- see fb_shadow's own comment in fb.c). And S() SCALES: 32+10 = 42
 * device px at 100%, 63 at 150% -- inside the ceiling -- but 84 at 200%, PAST
 * it, where fb_shadow's pre-clamp tightens the focused blur to an effective
 * ~26pt. That degradation is deliberate, consistent (the corner tile and the
 * edge strips clamp together inside fb_shadow, so no seam) and COUNTED
 * (fb_shadow_clamp_count in fb.h); shrinking the 100% shadow to protect a
 * 200% ceiling would be backwards. If HiDPI shadows ever matter more than
 * this, the ceiling itself (GFX_MASK_MAX) is the thing to raise.
 * DY is kept strictly less than BLUR on both sides so the top
 * edge (the blur-dy rows that clear the offset, see win_box below) never
 * closes to nothing the way it would if DY caught up to BLUR. */
#define WSH_DY(f)    ((f) ? S(14) : S(3))
#define WSH_BLUR(f)  ((f) ? S(32) : S(14))
#define WSH_ALPHA(f) ((f) ? (g_ui_dark ? 165 : 95) : (g_ui_dark ? 90 : 40))

/* A window's real footprint: its rectangle PLUS the drop shadow, taken at the
 * FOCUSED extent always -- a window that loses focus while moving must not
 * declare the smaller box. Damage that stops at the window's own edge leaves
 * the old shadow standing when the window moves, which is exactly the kind of
 * artefact that makes people distrust a partial-render path. */
/* NOW READS win_draw_rect, so a window being flown to the dock or scaled into
 * an Expose cell declares the box it is ACTUALLY drawn in. Taking w->x/w->w
 * here instead would report a footprint the window has not occupied since the
 * gesture began -- the exact shape of the bug WM_DAMAGE_LIE fakes, arrived at
 * honestly. An off-screen window reports an EMPTY box, which dirty_rect drops
 * on the floor by design (see its "nothing on screen changed" guard); the
 * frames where that transition happens are damaged explicitly by the animation
 * that caused it, in wm_anim_tick, and not left to this function to infer. */
static void win_box(const struct win *w, struct drect *r)
{
    int b = WSH_BLUR(1) + 1, dy = WSH_DY(1);
    int x, y, ww, wh, a;
    if (!win_draw_rect(w, &x, &y, &ww, &wh, &a)) { r->x0 = r->y0 = r->x1 = r->y1 = 0; return; }
    r->x0 = x - b;            r->y0 = y - b;
    r->x1 = x + ww + b;       r->y1 = y + wh + b + dy;
    /* THE EXPOSE TITLE overhangs a narrow thumbnail, and its extent is exactly
     * knowable -- same argument as the dock's tooltip in dock_box_hov(), and
     * the same refusal to be vague about it. A title wider than the window it
     * names is not an edge case here: "Terminal" under a 200pt-wide thumbnail
     * scaled to a third is most of them. */
    if (ex_state()) {
        int tw = fb_text_width(w->title), cx = x + ww / 2;
        if (cx - tw / 2 - S(6) < r->x0) r->x0 = cx - tw / 2 - S(6);
        if (cx + tw / 2 + S(6) > r->x1) r->x1 = cx + tw / 2 + S(6);
    }
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
static void dock_icon_box(int slot, struct drect *r);        /* ditto: one icon's tile */
static void dirty_dock(void);
static int in_rect(int px, int py, int x, int y, int w, int h);
static void enqueue(struct win *w, int type, int a, int b);
static void raise_win(int wi);
static int win_open_scale(const struct win *w);

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

/* ===========================================================================
 * THE DOCK FLY -- minimise and restore, as motion.
 *
 * WHAT THIS IS NOT, said here because it is the first thing anyone will look
 * for: it is NOT the genie. The genie warp bends a window around a curve by
 * resampling every COLUMN of it independently against a different vertical
 * scale and a different horizontal offset -- a per-column resample. This
 * compositor has no primitive for that and building one is a rasterizer, not
 * an animation: fb.c's scaled blits map a destination pixel to exactly one
 * source pixel through a single affine ratio, which is the wrong shape of
 * arithmetic entirely. What is built here is a scale-with-fade, which is what
 * macOS itself ships as the OTHER option in System Settings ("Minimise windows
 * using: Scale effect"), and calling it that is more honest than calling a
 * shrink a genie. If the warp is ever wanted, the thing to ask for is a
 * column-wise resample in c/lib/gfx or fb.c, not more code in this file.
 * ======================================================================== */

/* Which dock icon this window's app owns. Matched by the aex header NAME, the
 * same key wm_launch's single-instance branch and the running-dot indicator
 * both use -- not by file path, which reg[] and apps[] spell differently.
 * A window with no dock icon (none exists today, but a WK_FINDER or a future
 * app launched by path would be one) flies to the middle of the dock rather
 * than to slot 0, which reads as "into the dock" instead of as "into Clock". */
static int dock_slot_for(const struct win *w)
{
    if (w->app)
        for (int i = 0; i < nreg; i++)
            if (streq(reg[i].name, w->app->name)) return i;
    return nreg > 0 ? nreg / 2 : 0;
}

/* Progress of the dock fly, 0..256, where 0 is the window's own frame and 256
 * is its dock icon. Settled windows answer from `minimized` alone, which is
 * what makes this safe to call on every window every frame. */
static int min_prog(const struct win *w)
{
    if (!w->min_t0) return w->minimized ? 256 : 0;
    uint64_t e = timer_ticks() - w->min_t0;
    if (e >= MINFLY_TICKS) return w->min_dir ? 256 : 0;
    int eased = ease_out((int)(e * 256 / MINFLY_TICKS));
    return w->min_dir ? eased : 256 - eased;
}

/* The whole area a fly sweeps, end to end: the window's frame box unioned with
 * its dock icon box, both taken at the FOCUSED shadow extent. Used once, on the
 * frame the flight lands, to re-lay the entire path in a single rectangle --
 * see wm_anim_tick for why a per-frame union is not sufficient on its own. */
static void win_fly_sweep(const struct win *w, struct drect *r)
{
    int b = WSH_BLUR(1) + 1, dy = WSH_DY(1);
    struct drect ic;
    dock_icon_box(w->min_slot, &ic);
    r->x0 = w->x - b;             r->y0 = w->y - b;
    r->x1 = w->x + w->w + b;      r->y1 = w->y + w->h + b + dy;
    if (ic.x0 - b < r->x0) r->x0 = ic.x0 - b;
    if (ic.y0 - b < r->y0) r->y0 = ic.y0 - b;
    if (ic.x1 + b > r->x1) r->x1 = ic.x1 + b;
    if (ic.y1 + b + dy > r->y1) r->y1 = ic.y1 + b + dy;
}

/* ---- Expose: the grid ----------------------------------------------------
 *
 * Rebuilt at the moment the gesture starts and then held FIXED for the life of
 * the mode. Not recomputed per frame, and deliberately not recomputed when the
 * user picks a window: the grid somebody is looking at is the grid the windows
 * have to fly home from, and a layout that re-flowed under the pointer at the
 * instant of the click would move the target out from under the click that
 * chose it. */
static void ex_layout(void)
{
    ex_n = 0;
    /* Back-to-front, so slot order matches stacking order: the window you last
     * used lands in the last cell, and that stays true from one invocation to
     * the next as long as you have not restacked anything. A picker whose
     * items move for no reason the user can see is a picker you have to read
     * every time instead of aiming at. */
    for (int i = 0; i < norder && ex_n < MAXWIN; i++)
        if (wins[order[i]].used) ex_wi[ex_n++] = order[i];
    if (!ex_n) return;

    /* rows = ceil(sqrt(N)). This machine's world is NAPPS ~ 11 and one window
     * per app, so N is single digits and the packing question that a real
     * Expose has to answer (longest-side-first bin packing, so a wide window
     * and a tall one do not both get a square cell) does not arise: at N <= 9
     * a uniform grid wastes cell area, never legibility. The aspect fit below
     * is what actually makes a 16:9 browser and a square clock both readable. */
    int cols = 1;
    while (cols * cols < ex_n) cols++;
    int rows = (ex_n + cols - 1) / cols;

    int g = S(EX_GUTTER_PT), th = S(EX_TITLE_PT);
    int dx0, dy0, dw, dh;
    dock_geom(&dx0, &dy0, &dw, &dh);
    (void)dx0; (void)dw;
    int ax0 = g, ay0 = MBH + g, ax1 = W - g, ay1 = dy0 - g;
    if (ay1 - ay0 < S(120)) ay1 = H - g;         /* a dock taller than the screen */

    int cw = (ax1 - ax0) / cols, ch = (ay1 - ay0) / rows;
    for (int k = 0; k < ex_n; k++) {
        struct win *w = &wins[ex_wi[k]];
        int r = k / cols, c = k - r * cols;
        /* The LAST row is CENTRED. Seven windows in a 3x3 leave two holes, and
         * a hole on the right of the bottom row reads as a layout bug rather
         * than as a count -- the grid should look built for what is in it. */
        int inrow = (r == rows - 1) ? ex_n - r * cols : cols;
        int rowx = ax0 + (ax1 - ax0 - inrow * cw) / 2;
        int bx = rowx + c * cw, by = ay0 + r * ch;
        int fitw = cw - g, fith = ch - g - th;
        if (fitw < 1) fitw = 1;
        if (fith < 1) fith = 1;
        /* PRESERVE ASPECT, and never magnify past 1:1. A 200x120 utility window
         * blown up to fill a 400x300 cell is a blurred lie about its own size,
         * and the single thing a picker must get right is which window is
         * which -- size is half of how that is recognised. */
        int s = 256;
        if (w->w > 0 && fitw * 256 / w->w < s) s = fitw * 256 / w->w;
        if (w->h > 0 && fith * 256 / w->h < s) s = fith * 256 / w->h;
        if (s > 256) s = 256;
        if (s < 16) s = 16;
        int tw = w->w * s / 256, thh = w->h * s / 256;
        if (tw < 1) tw = 1;
        if (thh < 1) thh = 1;
        int cx = bx + cw / 2, cy = by + (ch - th) / 2;
        ex_cell[k].x0 = cx - tw / 2;
        ex_cell[k].y0 = cy - thh / 2;
        ex_cell[k].x1 = ex_cell[k].x0 + tw;
        ex_cell[k].y1 = ex_cell[k].y0 + thh;
    }
}

/* Grid slot of a wins[] index, or -1. Answers -1 whenever the mode is down, so
 * every caller can ask unconditionally. */
static int ex_slot(int wi)
{
    if (!ex_state()) return -1;
    for (int k = 0; k < ex_n; k++) if (ex_wi[k] == wi) return k;
    return -1;
}

/* Eased progress of the Expose transition: 0 = windows at their own frames,
 * 256 = windows in their cells. Leaving runs the same curve backwards, so a
 * cancelled gesture retraces the path it came in on rather than taking a
 * second, different route home. */
static int ex_prog(void)
{
    if (!ex_t0) return ex_on ? 256 : 0;
    uint64_t e = timer_ticks() - ex_t0;
    if (e >= EX_DUR_TICKS) return ex_on ? 256 : 0;
    int eased = ease_out((int)(e * 256 / EX_DUR_TICKS));
    return ex_on ? eased : 256 - eased;
}

/* THE SINGLE DEFINITION SITE. See the block comment above win_box. */
static int win_draw_rect(const struct win *w, int *ox, int *oy, int *ow, int *oh, int *oa)
{
    int x = w->x, y = w->y, ww = w->w, wh = w->h, a = 255;
    int p, k;

    if (w->min_t0) {
        /* In flight to or from the dock. Checked FIRST and ahead of the hidden
         * test, because `minimized` is already set the whole way down. */
        struct drect ic;
        dock_icon_box(w->min_slot, &ic);
        p = min_prog(w);
        x  = w->x + (ic.x0 - w->x) * p / 256;
        y  = w->y + (ic.y0 - w->y) * p / 256;
        ww = w->w + ((ic.x1 - ic.x0) - w->w) * p / 256;
        wh = w->h + ((ic.y1 - ic.y0) - w->h) * p / 256;
        /* THE FADE IS THE SECOND HALF ONLY, and that is a cost decision as much
         * as a look. Constant-alpha compositing here is per-pixel over the
         * DESTINATION rect (see anim_blit_fade), against a row copy for the
         * opaque path -- so holding the window solid while it is still large
         * and dissolving it once it is small keeps the expensive pixels to the
         * frames that have few of them. It also happens to be what the eye
         * expects: the window shrinks, and vanishes as it arrives. */
        a = p <= 128 ? 255 : 255 - 255 * (p - 128) / 128;
        if (a < 0) a = 0;
    } else if ((k = ex_slot((int)(w - wins))) >= 0) {
        /* In the picker. A MINIMISED window is shown too, dimmer -- it is a
         * window you own and cannot otherwise see, which is precisely what a
         * window picker is for -- and it flies out of its DOCK ICON rather than
         * out of the frame it is not occupying, so the motion says where it
         * actually came from. */
        struct drect home;
        if (w->minimized) dock_icon_box(dock_slot_for(w), &home);
        else { home.x0 = w->x; home.y0 = w->y; home.x1 = w->x + w->w; home.y1 = w->y + w->h; }
        p = ex_prog();
        const struct drect *c = &ex_cell[k];
        x  = home.x0 + (c->x0 - home.x0) * p / 256;
        y  = home.y0 + (c->y0 - home.y0) * p / 256;
        ww = (home.x1 - home.x0) + ((c->x1 - c->x0) - (home.x1 - home.x0)) * p / 256;
        wh = (home.y1 - home.y0) + ((c->y1 - c->y0) - (home.y1 - home.y0)) * p / 256;
        if (w->minimized) a = 255 - 105 * p / 256;
    } else if (w->minimized) {
        return 0;                        /* hidden: not composited, not hit-tested */
    } else {
        int s = win_open_scale(w);       /* the open pop, a scale about the centre */
        if (s) { ww = w->w * s / 256; wh = w->h * s / 256;
                 x = w->x + (w->w - ww) / 2; y = w->y + (w->h - wh) / 2; }
    }
    if (ww < 1) ww = 1;
    if (wh < 1) wh = 1;
    *ox = x; *oy = y; *ow = ww; *oh = wh; *oa = a;
    return 1;
}

/* Is this window drawn the ORDINARY way this frame -- at its own frame, full
 * size, fully opaque, and not in the picker?
 *
 * That is the ONLY case that gets a real liquid-glass titlebar sampling the
 * live backdrop. Every other case goes through a scratch surface, whose
 * titlebar is draw_frame_body's solid gradient and samples nothing. ONE
 * predicate, asked by the renderer (which path to take) and by dmg_expand
 * (whether there is a glass panel here to protect), so the two cannot drift --
 * and drift here is not cosmetic: dmg_expand grows any damage rectangle that
 * touches a glass panel until it contains the whole panel, so a window
 * claiming glass at a full-width frame it is not occupying would grow every
 * rectangle near it and turn the per-window damage list back into a
 * full-screen repaint, invisibly. */
static int win_drawn_direct(const struct win *w, int x, int y, int ww, int wh, int a)
{
    return x == w->x && y == w->y && ww == w->w && wh == w->h && a >= 255
           && ex_slot((int)(w - wins)) < 0;
}

/* The glass titlebar panel dmg_expand must contain whole, or 0 if this window
 * has no glass on screen this frame. */
static int win_glass_box(const struct win *w, struct drect *p)
{
    int x, y, ww, wh, a;
    if (!w->used) return 0;
    if (!win_draw_rect(w, &x, &y, &ww, &wh, &a)) return 0;
    if (!win_drawn_direct(w, x, y, ww, wh, a)) return 0;
    p->x0 = x; p->y0 = y;
    p->x1 = x + ww; p->y1 = y + TBH + S(10);
    return 1;
}

/* Minimise / restore. The state flips NOW and the picture takes MINFLY_TICKS
 * to agree -- see the comment on min_t0 in struct win. */
static void win_set_min(struct win *w, int on)
{
    on = on ? 1 : 0;
    if (on == w->minimized && !w->min_t0) return;
    dirty_win(w);                    /* wherever it is at this instant */
    w->minimized = on;
    w->min_dir = on;
    w->min_slot = dock_slot_for(w);
    w->min_t0 = timer_ticks();
    if (!w->min_t0) w->min_t0 = 1;   /* 0 means "settled", so never store it */
    if (!on) raise_win((int)(w - wins));
    win_box(w, &w->anim_prev);       /* seed the per-frame damage union */
    dirty_win(w);
}

/* Put a window where the fly would have left it, immediately. Used when a
 * second gesture arrives on top of a running one: two animations moving the
 * same rectangle would each believe they own its position, and the damage
 * union would be computed against whichever of them wrote anim_prev last. */
static void win_fly_settle(struct win *w)
{
    if (!w->min_t0) return;
    struct drect s;
    win_fly_sweep(w, &s);
    w->min_t0 = 0;
    dirty_rect(s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0);
}

/* Damage one grid slot -- its thumbnail, its shadow and its title, via the same
 * win_box every other damage path uses. */
static void ex_dirty_slot(int k)
{
    if (k < 0 || k >= ex_n) return;
    struct drect r;
    win_box(&wins[ex_wi[k]], &r);
    dirty_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
}

/* The grid slot under a point, or -1. Only while the grid is STILL: hit-testing
 * a moving target is how a click lands on the window that happened to be
 * passing through, and the mode is only pickable once it has settled anyway. */
static int ex_hover_at(int x, int y)
{
    if (!ex_on || ex_t0) return -1;
    for (int k = ex_n - 1; k >= 0; k--) {
        const struct drect *c = &ex_cell[k];
        if (x >= c->x0 && x < c->x1 && y >= c->y0 && y < c->y1) return k;
    }
    return -1;
}

static void ex_enter(void)
{
    if (ex_state()) return;
    /* A dock fly still running would be a second animation moving the same
     * rectangle; land it first rather than interleave two owners. */
    for (int i = 0; i < MAXWIN; i++) if (wins[i].used) win_fly_settle(&wins[i]);
    ex_layout();
    if (ex_n == 0) return;                 /* nothing to pick between */
    ex_on = 1;
    ex_t0 = timer_ticks();
    if (!ex_t0) ex_t0 = 1;
    ex_hov = -1;
    for (int k = 0; k < ex_n; k++) win_box(&wins[ex_wi[k]], &wins[ex_wi[k]].anim_prev);
    /* ONE full-screen frame, for the dim -- which lands everywhere at once and
     * is honestly whole-screen damage. Everything after this frame pays only
     * for the windows that move; see wm_anim_tick. */
    dirty_full();
    /* The grid itself, once: a harness can then assert that a mid-flight rect
     * lies strictly BETWEEN the window's own frame and the cell it is aimed at,
     * which is the actual claim "it animated" makes. Without the destination
     * printed, an intermediate rectangle is just an unexplained number. */
    kprintf("[wm] expose on n=%d\n", ex_n);
    for (int k = 0; k < ex_n; k++)
        kprintf("[wm] expose cell %d win %d rect %d %d %d %d\n", k, ex_wi[k],
                ex_cell[k].x0, ex_cell[k].y0,
                ex_cell[k].x1 - ex_cell[k].x0, ex_cell[k].y1 - ex_cell[k].y0);
}

/* Leave the picker. `pick` is a wins[] index to bring to the front, or -1 to
 * leave the stacking exactly as it was. */
static void ex_leave(int pick)
{
    if (!ex_on) return;
    if (pick >= 0 && wins[pick].used) {
        struct win *w = &wins[pick];
        /* Picking a minimised window un-minimises it, and does so WITHOUT a
         * dock fly: it is already on screen, in a cell, in front of the user.
         * Flying it to the dock and back out again would animate a journey it
         * is not making. */
        w->minimized = 0;
        w->min_t0 = 0;
        raise_win(pick);
    }
    ex_on = 0;
    ex_t0 = timer_ticks();
    if (!ex_t0) ex_t0 = 1;
    ex_hov = -1;
    for (int k = 0; k < ex_n; k++) win_box(&wins[ex_wi[k]], &wins[ex_wi[k]].anim_prev);
    dirty_full();          /* the raise re-stacks every overlap on screen */
    kprintf("[wm] expose off pick=%d\n", pick);
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

static int greeter_win(void)
{
    if (g_greeter_ai < 0) return -1;
    struct app *ap = &apps[g_greeter_ai];
    if (!ap->used || !ap->alive) return -1;
    return ap->win;
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
    /* THE GATE. Everything else about the lock is presentation; this line is
     * the mechanism. It is here and not at the call sites deliberately: there
     * are five of them today (boot, the dock, a Finder double-click, a file
     * association, SYS_OPEN_PATH from any app) and the next one will not
     * remember to ask. See the block comment on g_locked. */
    if (g_locked && !streq(aex_file, GREETER_AEX)) {
        serial_puts("[wm] locked: refusing to launch ");
        serial_puts(aex_file);
        serial_puts("\n");
        return;
    }
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
    /* The page-cache handle for the app's own file, opened BEFORE the cli
     * below because it stats the path and the block drivers here poll with
     * interrupts on. With it, the loader maps the app's read-only whole pages
     * straight out of the cache instead of copying them, so a second instance
     * of the same .aex -- or the same one relaunched -- shares its text rather
     * than getting a private copy of every byte. -1 (no slot, no real inode)
     * is not an error: the loader copies, exactly as it did before. */
    int fh = pcache_file_open(aex_file);

    uint64_t prev_cr3, fl;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(fl) :: "memory");   /* save IF, then off */
    __asm__ volatile ("mov %%cr3, %0" : "=r"(prev_cr3));
    vmm_switch(space);
    uint64_t img_top = 0;
    struct elf_image wei;
    /* aex_load_image_ex, not aex_load_ex, only so exec_note_load() below gets
     * the page counts -- a launch from the Dock is a load like any other and
     * the loader's line on the serial log has to cover it, or the number reads
     * as if the desktop's apps were exempt. entry/top are the same two fields
     * aex_load_ex would have returned. */
    uint64_t entry = aex_load_image_ex(img, (uint64_t)bytes, name, ext, &wei, fh) == 0
                         ? wei.entry : 0;
    if (entry) { img_top = wei.top; exec_note_load(aex_file, &wei); }
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
        /* THE HINT IN THE HEADER, which the build system has been writing and
         * this line has been ignoring since it was added.
         *
         * It used to read `streq(name, "browser.aex") ? 2048 : 1024`, and that
         * comparison CAN NEVER BE TRUE: `name` comes from aex_info() -> the
         * header's name field, and tools/mkaex.py writes the DISPLAY name
         * there -- "Browser", from Makefile:890. So the browser has been
         * running on 1024 pages (4 MiB), which is exactly the size the comment
         * above says is not enough for it.
         *
         * Every piece of the intended mechanism was already built and none of
         * it was connected: the Makefile passes `--stack-pages 2048`, mkaex.py
         * packs it at offset 52, aex.h declares the field, and
         * aex_stack_pages() exists to read it -- with ZERO callers in the
         * tree. This is that call.
         *
         * A missing or zero hint keeps the 1024-page default, so every app
         * built before the flag existed is unaffected, and an app that wants
         * more asks for it in the one place that already knows: its own
         * build rule. */
        uint16_t hint = aex_stack_pages(img, (uint64_t)bytes);
        int stk_pages = hint ? (int)hint : 1024;
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
    /* The loader's VMAs took their own references; this is the transient one
     * this function opened, and it is put on BOTH paths -- the failure return
     * below would otherwise leave a live file entry pinning a slot for the
     * rest of the boot (32 of them exist). Put after the sti, because
     * pcache_file_put can purge pages and reach the frame allocator. */
    if (fh >= 0) pcache_file_put(fh);
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
    /* Remembered by SLOT, from the path that was launched -- not by the app's
     * display name, which comes out of the .aex header and is a string anybody
     * packing a disk could reuse. */
    if (streq(aex_file, GREETER_AEX)) g_greeter_ai = ai;

    /* Every app is a process now. The proc (not the struct app) is the thread's
     * payload; it carries the address space + fd table, and points back at the
     * window owner via ->gui. GUI apps are launched by the WM (ppid 0). */
    struct proc *p = proc_create(space, ap, ap->name, 0);
    if (!p) { serial_puts("[wm] launch: proc table full\n"); ap->used = ap->alive = 0; vmm_free_space(space); kfree(img); return; }
    /* M28: THE GRANT. proc_create() deliberately defaults every new slot to
     * DENY so that each creation site states what it trusts, out loud -- and
     * this site stated nothing, which was fine for exactly as long as the
     * syscall gate wasn't wired. The day it was (24130fcef), every
     * Dock-launched app started holding caps=0x0: the browser's first
     * sock_open returned -1, "session restored 0 tabs" was the same refusal
     * through SYS_READ_FILE, and the scoreboard could not load its own
     * self-test page -- found by the J unit's startup probe printing
     * `caps=0x0`. GUI apps launched by the WM are the desktop: they are the
     * same trust root the console shell is (proc_spawn grants it CAP_ALL,
     * exec.c:369), and a per-app manifest narrowing this is future work that
     * must arrive as a manifest, not as a silent zero. */
    p->caps = CAP_ALL; p->fs_prefix[0] = 0;
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
        num != SYS_HTTP_BODY && num != SYS_SYSINFO && num != SYS_SCREEN_INFO &&
        num != SYS_OPEN_PATH) {
        /* Window ADOPTION: a CLI process (e.g. /bin/as running a script) gets a
         * window on its first SYS_GUI_CREATE -- allocate an app slot and bind it
         * to the proc, then fall through to the normal create. Exit/teardown
         * reuses the standard path: proc_exit -> wm_app_exit (alive=0) -> reap.
         * HTTP fetch/body/status are process-safe non-GUI services and also pass
         * this gate, as does SYSINFO -- it is a read-only query about the system
         * with no window semantics at all, and gating it meant /bin/sh and every
         * CLI program could not ask the machine about itself. Any other GUI call
         * without a window stays an error.
         *
         * SYS_OPEN_PATH JOINS THAT LIST, and by the same argument rather than a
         * new one: it draws nothing, it owns no window, and it touches no
         * per-app state -- it names a file and asks the system to open it with
         * whatever is registered for it. Requiring a window to make that call
         * meant a CLI process could not do the one thing `open(1)` does on
         * every other desktop, and the failure was silent in the worst way:
         * wm_gui_syscall returns -1 here, so `open_path()` from a shell script
         * answered "no" with nothing on the console saying why. It is still
         * gated -- syscall_cap_class() classifies SYS_OPEN_PATH as CAP_FS, so a
         * process without filesystem capability never reaches this function --
         * and it grants no power a CLI process lacks, since the same process
         * can already fork+execve. Found by the chat window's launcher
         * (fsroot/as/examples/chlaunch.as), which is a GUI app's only door from
         * a shell when the app is packed under /bin and the Dock never sees it. */
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
        /* WAKE the old tenant's waiters, do NOT waitq_init here. waitq_init
         * assigns a fresh SPINLOCK_INIT over the queue's lock -- ticket and
         * serving back to zero -- and a core queued on that lock at that
         * moment is holding a ticket number that will never be served again.
         * wins[] is static, so the lock is already valid; what a reused slot
         * needs is for anyone still parked on it to be released, and they
         * re-test their predicate and find the window gone. */
        evq_reset(&w->ev); waitq_wake_all(&w->evwq); w->wants_close = 0;
        /* A REUSED SLOT MUST NOT INHERIT the previous tenant's window state.
         * `used` guards the readers, but zoomed/minimized/min_* are read the
         * moment the new window is composited -- a slot whose last occupant was
         * minimised would open a window that is not on screen, and nothing
         * would say why. */
        w->min_w_pt = w->min_h_pt = 0;
        w->zoomed = w->minimized = 0;
        /* ...and that now includes the dock fly. A slot whose last occupant was
         * halfway to the dock would open its new window mid-flight, at a scale
         * and an opacity nobody asked for, shrinking into an icon belonging to
         * an app that has exited. anim_prev is cleared with it: a stale one
         * would union this window's first animated frame with a rectangle from
         * the previous tenant, damaging a region neither of them occupies. */
        w->min_t0 = 0; w->min_dir = 0; w->min_slot = 0;
        w->anim_prev.x0 = w->anim_prev.y0 = w->anim_prev.x1 = w->anim_prev.y1 = 0;
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
    case SYS_WAIT_EVENT: {
        /* SYS_POLL_EVENT without the spin. See the note in logit_abi.h for the
         * measurement that motivated it: 98% of this machine's kernel entries
         * were an app asking whether anything had happened yet.
         *
         * The BKL is HELD on entry and the wait must not hold it -- an app
         * asleep with the global lock stops the machine. wait_event_timeout
         * drops it across the park and re-takes it on resume (block_self does,
         * in one place), which is exactly the discipline bkl_hlt_wait already
         * uses for the console.
         *
         * There is no lost wakeup here and the reason is the BKL, not luck:
         * the predicate is evaluated while this core still holds it, and the
         * only writer (enqueue/enqueue_input, on the WM thread) needs the same
         * lock to push. So nothing can be queued between the test and the
         * park. */
        struct win *w = app_window(ap); if (!w) return 0;
        struct logit_event *ev = (struct logit_event *)a;
        /* ev == NULL: WAIT WITHOUT CONSUMING. That convention is what lets an
         * existing app adopt this by changing one line -- its `while
         * (poll_event(&e))` drain stays exactly as written, and only the
         * sys_yield() at the bottom of the loop becomes a sleep. Handing back
         * an event here instead would mean every caller restructuring its loop
         * to handle the first event separately from the rest, which is how a
         * mechanical change becomes twelve behavioural ones. */
        if (ev) {
            if (!user_range_ok(ev, sizeof *ev, 1)) return -1;
            if (evq_pop(&w->ev, ev)) return 1;      /* fast path: already there */
        } else if (!evq_empty(&w->ev)) {
            return 1;
        }
        int ms = (int)b;
        int ok = 0;
        if (ms > 0)
            wait_event_timeout(&w->evwq, !evq_empty(&w->ev) || w->wants_close, ms, ok);
        else
            wait_event(&w->evwq, !evq_empty(&w->ev) || w->wants_close);
        return ev ? evq_pop(&w->ev, ev) : !evq_empty(&w->ev);
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
        if (rc != 0 || !rb) {
            /* rc == -2 is the 15 s wall-clock cap res_fetch now applies to its
             * own redirect loop (c/net/http/http.c). Named separately because
             * "the server refused" and "we gave up" send a reader to different
             * places, and this line is the only trace either leaves. */
            kprintf("[res] fetch %s rc=%d url=%s\n",
                    rc == -2 ? "TIMED OUT (15 s cap)" : "FAILED", rc, src);
            return -1;
        }
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
    /* WAKE AFTER THE PUSH, never before: the sleeper's predicate is "the ring
     * is not empty", and a wake that arrives first is a wake the sleeper
     * re-tests and goes back to sleep through. Waking with nothing queued is
     * harmless (wait_event re-tests) but it is also the bug that turns a
     * blocking wait back into a poll, so the order is the point. */
    waitq_wake_one(&w->evwq);
}

static void enqueue_input(struct win *w, int type, int a, int b, int mods, int button, int wheel)
{
    struct logit_event e = { type, a, b, mods, button, wheel };
    evq_push(&w->ev, &e);
    waitq_wake_one(&w->evwq);
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

/* THE COMMENT THAT USED TO BE HERE SAID "IRQ-safe: no locks, no shared-state",
 * and it was true of one producer. THERE ARE THREE.
 *
 * wm_key comes from IRQ 1, wm_mouse_event from IRQ 12, and BOTH again from
 * usb_isr (c/drivers/usb/usb_core.c) by way of hid_poll -- the xHCI interrupt
 * decodes a HID report and posts it through the same two entry points. Three
 * vectors, one ring. They cannot overlap today only because every interrupt
 * takes the big kernel lock on the way in -- which is precisely what
 * step 3 of the BKL removal takes away for exactly these vectors, since after
 * the input-deferral fix none of the handlers does anything else. Unlocked,
 * two cores read the same inq_tail, write the same slot and store the same nt:
 * one event silently gone, under exactly the flood the ring was sized for.
 *
 * The comment was not wrong; it was UNQUALIFIED. "No locks" is a true statement
 * about this function, and the property that made it sufficient lived in a
 * caller three files away.
 *
 * WHY A LOCK AND NOT A CAS. Claiming a slot with a CAS on the tail advances
 * inq_tail BEFORE inq[t] is written, so the drain can observe a tail past a
 * slot still holding the previous tenant's event -- the standard multi-producer
 * hazard, which one atomic does not close. A per-slot ready flag closes it and
 * puts a spin in the CONSUMER to save six instructions in an IRQ. Two queues,
 * one per vector, is genuinely lock-free and fits the shape -- but it loses the
 * order between a keystroke and a click, and struct inev has no timestamp to
 * restore it. That order is not obviously disposable: `mods` is sampled in the
 * IRQ for the express purpose of reflecting the instant of the press.
 *
 * So: a lock over the producers only. The critical section is a bounds check, a
 * struct copy and a store, on a path that already did a port read. The CONSUMER
 * is untouched -- wm_drain_input writes only inq_head, and one reader against
 * one serialised writer is the single-producer case it was always written for.
 *
 * Inert until step 3: the BKL still serialises both IRQs, so this lock is never
 * contended and nothing about the machine's behaviour changes. That is the
 * point of landing it first -- declaring the vectors BKL-free then becomes one
 * line against a safe queue instead of two changes at once against an unsafe
 * one.
 *
 * NOTE for whoever does step 3: usb_core.c's comment above usb_isr() ends
 * "Nothing sleeps, nothing allocates, nothing takes a lock." The last clause
 * stops being true here. A spinlock in an ISR is fine -- it is the sleeping and
 * the allocating that are not -- but the sentence has to say so rather than be
 * quietly falsified. */
static spinlock_t inq_lock = SPINLOCK_INIT;

static void inq_push(const struct inev *e)
{
    uint64_t f = spin_lock_irqsave(&inq_lock);
    int nt = (inq_tail + 1) % INQ_N;
    if (nt != inq_head) {                  /* full: drop (cosmetic under flooding) */
        inq[inq_tail] = *e;
        inq_tail = nt;
    }
    spin_unlock_irqrestore(&inq_lock, f);
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

/* ============================================================================
 * THE MENU BAR COMES ALIVE.
 *
 * "LogitOS  File  View" used to be three fb_text calls and nothing else --
 * paint of a menu bar, not one. This section is the machinery: hit rects for
 * the titles, a dropdown panel drawn like the dock (fresh from state every
 * time it is asked for, never a retained surface), and the handful of real
 * actions the WM can actually perform. The rest of it -- the dropdown's
 * items, the About/Shut Down/Restart panel -- lives further down, after
 * draw_clock; only the TITLE layout has to live here, because draw_menubar()
 * below is the one thing that paints it.
 *
 * "Edit" is gone on purpose. The WM cannot reach inside an app -- it has no
 * text buffer to cut or paste -- so a Cut/Copy/Paste menu here would be three
 * items that do nothing, which is worse than no menu: it teaches a user this
 * desktop's chrome is decoration. File and View both survive because both
 * gained something real: File routes to the close button and the minimise
 * gesture the traffic lights already perform; View is the on-screen home for
 * Expose, which otherwise has no menu presence at all (hot corner + Cmd+E,
 * neither of them discoverable by looking at the screen).
 *
 * ONE FUNCTION SAYS WHERE A TITLE IS -- same discipline win_draw_rect enforces
 * for a window's footprint. menu_bar_layout() publishes g_menu_tx/g_menu_tw;
 * draw_menubar() (paint) and menu_title_hit() (click + hover) both read them,
 * so a click and the word it lands on cannot disagree about where the word is.
 * Valid only after the first draw_menubar() -- the same "not on screen yet
 * either" caveat dock_hover_at documents for dock_x0/dock_y0. */
#define NMENU 3
static const char *g_menu_title[NMENU] = { "LogitOS", "File", "View" };
static int g_menu_tx[NMENU], g_menu_tw[NMENU];   /* device px, published each frame */

static void menu_title_hit(int i, struct drect *r)
{
    r->y0 = 0; r->y1 = MBH;
    r->x0 = (i == 0) ? S(8) : g_menu_tx[i] - S(14);   /* item 0's box also covers the logo dot */
    r->x1 = g_menu_tx[i] + g_menu_tw[i] + S(14);
}

/* Reported ONCE, the first time the geometry is computed -- same reasoning as
 * "[wm] win N frame ..." and "[wm] expose cell ...": a test driver has no
 * business re-deriving AA-font text width in Python to find where "File" is
 * clickable, and a driver that guessed a pixel offset here is exactly the kind
 * of thing that rots silently the day the font or the title text changes. */
static void menu_bar_layout(void)
{
    static int reported;
    int x = S(32);                            /* unchanged from the old hardcoded "LogitOS" x */
    for (int i = 0; i < NMENU; i++) {
        g_menu_tx[i] = x;
        g_menu_tw[i] = fb_text_width(g_menu_title[i]);
        x += g_menu_tw[i] + S(44);            /* the old File-Edit gap, kept for the same rhythm */
    }
    if (!reported) {
        reported = 1;
        for (int i = 0; i < NMENU; i++) {
            struct drect t; menu_title_hit(i, &t);
            kprintf("[wm] menu title %d %s x0 %d y0 %d x1 %d y1 %d\n",
                    i, g_menu_title[i], t.x0, t.y0, t.x1, t.y1);
        }
    }
}

static void draw_menubar(void)
{
    /* Liquid Glass menu bar (thin -> adaptive edge band) */
    /* Top/left/right are SCREEN edges -- the slab is cut there, not ended, so
     * no bevel (see fb_liquid_glass_cut in fb.h; this call is why it exists).
     * The bottom edge keeps its rim: that hairline is the menu bar's only
     * separation from the wallpaper below it. */
    unsigned mbcut = GLASS_CUT_TOP | GLASS_CUT_LEFT | GLASS_CUT_RIGHT;
    if (g_ui_dark) fb_liquid_glass_cut(0, 0, W, MBH, S(2), 24, 24, 32, 150, mbcut);
    else           fb_liquid_glass_cut(0, 0, W, MBH, S(2), 255, 255, 255, 110, mbcut);
    fb_blend_rect(0, MBH - S(1), W, S(1), 0, 0, 0, g_ui_dark ? 70 : 28);  /* hairline */
    uint32_t ink = g_ui_dark ? rgb(232, 233, 238) : rgb(40, 40, 48);
    fb_fill_circle(S(16), MBH / 2, S(6), ink);
    menu_bar_layout();                        /* publishes g_menu_tx/tw for the click path */
    for (int i = 0; i < NMENU; i++) fb_text(g_menu_tx[i], S(4), g_menu_title[i], ink);
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
/* Vertical breathing room, top AND bottom, between an icon and the panel edge.
 * RETUNED (unit F): this used to be a bare `S(20)` / `S(10)` split three ways
 * across dock_geom, dock_hover_at and the click hit-test in the mouse handler
 * below, with no name tying them together -- exactly the trap the WSH_ and
 * DOCKSH_ single-definition macros elsewhere in this file exist to avoid, and
 * the reason the icons visibly crowded the top/bottom edges was that all
 * three copies agreed with EACH OTHER, just not with what "enough padding"
 * should have been. One constant: isz=50 in a dh=isz+2*DOCK_PAD_PT panel now
 * leaves 18pt each side instead of 10 (a panel 50/86=58% icon by height
 * instead of 50/70=71%), close to a real macOS dock's proportion instead of
 * the icons nearly touching the rim. */
#define DOCK_PAD_PT 18
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
    *dh = isz + 2 * S(DOCK_PAD_PT);
    *x0 = (W - *dw) / 2;
    *y0 = H - *dh - S(12);
}

/* ONE dock icon's tile, in device pixels -- the target a minimised window flies
 * into and out of.
 *
 * Derived from dock_geom() and S(DOCK_PAD_PT), which is the same arithmetic
 * draw_dock() places the icon with and the same the click hit-test uses. That
 * is not tidiness: this file has already been bitten once by three copies of
 * the dock's vertical padding agreeing with each other and not with the
 * drawing (see DOCK_PAD_PT's comment), and a fly that lands next to the icon
 * instead of on it is that bug in its most visible possible form. */
static void dock_icon_box(int slot, struct drect *r)
{
    int x0, y0, dw, dh;
    dock_geom(&x0, &y0, &dw, &dh);
    (void)dw; (void)dh;
    int isz = S(DOCK_ISZ_PT), gap = S(DOCK_GAP_PT);
    if (slot < 0) slot = 0;
    if (nreg > 0 && slot >= nreg) slot = nreg - 1;
    r->x0 = x0 + gap + slot * (isz + gap);
    r->y0 = y0 + S(DOCK_PAD_PT);
    r->x1 = r->x0 + isz;
    r->y1 = r->y0 + isz;
}

/* The GLASS PANEL: the rounded slab whose frost samples the live backdrop. This
 * is the rectangle a damage rectangle may not cut in half (see dmg_expand); the
 * rest of the dock's footprint is ordinary read-modify-write drawing that clips
 * exactly and needs no such promise. */
/* Same single-definition rule as WSH_* above: the dock's shadow is painted in
 * draw_dock and its extent is declared here, and if the two disagree a damage
 * rectangle clips the shadow in half. */
#define DOCKSH_DY   S(6)
#define DOCKSH_BLUR S(16)

static void dock_panel_box(struct drect *r)
{
    int x0, y0, dw, dh;
    dock_geom(&x0, &y0, &dw, &dh);
    int b = DOCKSH_BLUR + 1;
    r->x0 = x0 - b;          r->y0 = y0 - b;
    r->x1 = x0 + dw + b;     r->y1 = y0 + dh + b + DOCKSH_DY;   /* + the drop shadow */
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
    int dh = dock_isz + 2 * S(DOCK_PAD_PT);
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
    /* Called "soft drop shadow" and it was neither: a solid black rounded slab
     * at a flat alpha 50, offset down S(7), with no falloff anywhere -- a hard
     * edge all the way round, under a panel translucent enough to show it. */
    fb_shadow(dock_x0, dock_y0, dw, dh, S(28), DOCKSH_DY, DOCKSH_BLUR, 56);
    /* Liquid Glass: frost + rim refraction + specular highlight + body tint */
    if (g_ui_dark) fb_liquid_glass(dock_x0, dock_y0, dw, dh, S(28), 26, 26, 34, 104);
    else           fb_liquid_glass(dock_x0, dock_y0, dw, dh, S(28), 255, 255, 255, 44);

    /* Live hover magnification: the icon under the cursor grows in place (kept
     * inside the panel + gap so it never overlaps a neighbour) and shows its name
     * as a tooltip. Re-evaluated every frame; the input path asks dock_hover_at()
     * the same question and requests a frame when the answer changes, which is
     * what still animates this as the cursor sweeps the dock now that plain
     * motion no longer repaints anything. */
    int ccy = dock_y0 + S(DOCK_PAD_PT) + dock_isz / 2, animating = 0;
    int hov = dock_hover_at(mx, my);
    for (int i = 0; i < nreg; i++) {
        if (i == hov) continue;                            /* hovered tile drawn last, on top */
        int b = dock_bounce_off(i); if (b) animating = 1;  /* launch bounce lifts the icon */
        int ccx = dock_x0 + dock_gap + i * (dock_isz + dock_gap) + dock_isz / 2;
        dock_tile(i, ccx, ccy - b, dock_isz);
    }
    /* RUNNING-APP INDICATOR: a small dot under each app that has a live
     * process -- the cheapest recognisable macOS dock trait there is, and the
     * WM already has the answer for free (find_live_app is the exact check
     * wm_launch makes to decide "focus the existing window" vs. "start a new
     * one", keyed the same way: by the aex header NAME reg[] and apps[] both
     * carry, not by file path). Anchored to the panel's own bottom edge and
     * the BASE (non-magnified, non-bounced) icon column -- not `ccy - b` and
     * not the 1.3x hover size -- so the dot sits still while the icon above
     * it bounces or magnifies.
     *
     * DRAWN BEFORE THE MAGNIFIED HOVER TILE, and the order is the fix for a
     * measured overlap: the enlarged tile's bottom edge (ccy + 0.65*isz)
     * clears the dot row (dh - S(8)) by only S(8) - 0.15*isz -- under one
     * point at today's icon size, negative past isz = 53pt. An earlier
     * version drew the dots last and CLAIMED 2.5pt of clearance in this very
     * comment; the adversarial pass measured the dot punched into the icon.
     * Painting the icon over the dot instead makes the near-miss harmless at
     * every icon size, and matches what the eye expects: the icon in front,
     * the indicator behind it. */
    for (int i = 0; i < nreg; i++) {
        if (!find_live_app(reg[i].name)) continue;
        int ccx = dock_x0 + dock_gap + i * (dock_isz + dock_gap) + dock_isz / 2;
        fb_fill_circle(ccx, dock_y0 + dh - S(8), S(2), g_ui_dark ? rgb(235, 236, 240) : rgb(58, 58, 64));
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

/* -1 = closed; else an index into g_menu_title[]/g_menu_items[]. */
static int g_menu_open = -1;
static int g_menu_item_hov = -1;

/* ---- the "About" / "Shut Down" / "Restart" modal panel -------------------
 * A second, independent overlay rather than a fourth menu-item kind, because
 * it needs its own input capture the way Expose does: once it is up, a click
 * is either a button or a dismissal, never a window or dock click that leaks
 * through. Mutually exclusive with an open menu -- every path that opens one
 * closes the other first. */
enum { OV_NONE, OV_ABOUT, OV_CONFIRM_SHUTDOWN, OV_CONFIRM_RESTART };
static int g_overlay = OV_NONE;
static int g_overlay_btn_hov = -1;   /* -1 none, 0 cancel, 1 the destructive action */

#define AB_W S(320)
#define AB_H S(190)
#define CF_W S(300)
#define CF_H S(130)

static void overlay_box(struct drect *r)
{
    int w, h;
    switch (g_overlay) {
    case OV_ABOUT:                      w = AB_W; h = AB_H; break;
    case OV_CONFIRM_SHUTDOWN:
    case OV_CONFIRM_RESTART:            w = CF_W; h = CF_H; break;
    default: r->x0 = r->y0 = r->x1 = r->y1 = 0; return;
    }
    r->x0 = (W - w) / 2; r->y0 = (H - h) / 2;
    r->x1 = r->x0 + w;   r->y1 = r->y0 + h;
}

static void overlay_buttons(struct drect *cancel, struct drect *ok)
{
    struct drect p; overlay_box(&p);
    int bw = S(112), bh = S(30), gap = S(14);
    int by = p.y1 - bh - S(16);
    ok->x1 = p.x1 - S(16); ok->x0 = ok->x1 - bw; ok->y0 = by; ok->y1 = by + bh;
    cancel->x1 = ok->x0 - gap; cancel->x0 = cancel->x1 - bw; cancel->y0 = by; cancel->y1 = by + bh;
}

static int overlay_button_at(int x, int y)
{
    if (g_overlay != OV_CONFIRM_SHUTDOWN && g_overlay != OV_CONFIRM_RESTART) return -1;
    struct drect c, o; overlay_buttons(&c, &o);
    if (in_rect(x, y, c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0)) return 0;
    if (in_rect(x, y, o.x0, o.y0, o.x1 - o.x0, o.y1 - o.y0)) return 1;
    return -1;
}

/* Dirty wherever it IS, THEN change state -- the same order win_set_min uses
 * and for the same reason: the box a moment ago cannot be re-derived once
 * g_overlay has already moved on. */
static void overlay_close(void)
{
    if (g_overlay == OV_NONE) return;
    struct drect p; overlay_box(&p);
    g_overlay = OV_NONE; g_overlay_btn_hov = -1;
    dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
}

static void overlay_open(int kind)
{
    g_overlay = kind; g_overlay_btn_hov = -1;
    struct drect p; overlay_box(&p);
    dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
}

/* ---- the dropdown's items -------------------------------------------------
 * `label == NULL` is a separator: drawn as a hairline, never hit-tested. One
 * array per menu, walked by both the painter and the hit-tester below so a
 * row's picture and its hitbox are the same computation. */
struct mi { const char *label; };
static const struct mi MI_LOGO[] = {
    { "About This Machine" }, { "Settings..." }, { NULL },
    { "Shut Down..." }, { "Restart..." },
};
static const struct mi MI_FILE[] = { { "Close Window" }, { "Minimize" } };
static const struct mi MI_VIEW[] = { { "Show All Windows" } };
static const struct mi *g_menu_items[NMENU] = { MI_LOGO, MI_FILE, MI_VIEW };
static const int g_menu_nitems[NMENU] = {
    (int)(sizeof MI_LOGO / sizeof MI_LOGO[0]),
    (int)(sizeof MI_FILE / sizeof MI_FILE[0]),
    (int)(sizeof MI_VIEW / sizeof MI_VIEW[0]),
};

#define MI_ROW_H S(22)
#define MI_SEP_H S(9)
#define MI_PAD_X S(14)
#define MI_PAD_Y S(6)
static int menu_row_h(const struct mi *it) { return it->label ? MI_ROW_H : MI_SEP_H; }

/* Same discipline as menu_title_hit: the dropdown's geometry is computed once
 * here and read by the painter, the hit-tester and dmg_expand, so none of the
 * three can disagree about where the panel is. */
static void menu_dropdown_box(struct drect *r)
{
    if (g_menu_open < 0) { r->x0 = r->y0 = r->x1 = r->y1 = 0; return; }
    const struct mi *items = g_menu_items[g_menu_open];
    int n = g_menu_nitems[g_menu_open], maxw = 0;
    for (int i = 0; i < n; i++)
        if (items[i].label) { int w = fb_text_width(items[i].label); if (w > maxw) maxw = w; }
    int panel_w = maxw + 2 * MI_PAD_X;
    if (panel_w < S(170)) panel_w = S(170);
    int panel_h = 2 * MI_PAD_Y;
    for (int i = 0; i < n; i++) panel_h += menu_row_h(&items[i]);
    int x0 = g_menu_tx[g_menu_open] - S(10);
    if (x0 < 0) x0 = 0;
    if (x0 + panel_w > W) x0 = W - panel_w;
    r->x0 = x0; r->y0 = MBH;
    r->x1 = x0 + panel_w; r->y1 = MBH + panel_h;
}

/* One report, one call site each for "just opened" and "just switched" (see
 * both callers below) -- same "ask the guest, don't re-derive its geometry in
 * Python" reasoning as menu_bar_layout()'s report above: the panel's exact
 * pixel box depends on font metrics a test driver has no business computing. */
static void menu_report_open(void)
{
    struct drect p; menu_dropdown_box(&p);
    kprintf("[wm] menu open %d x0 %d y0 %d x1 %d y1 %d\n", g_menu_open, p.x0, p.y0, p.x1, p.y1);
}

/* The item under (px,py) in menu `mi`, or -1 (outside the panel, or a
 * separator). `*out_row` (if given) comes back as that row's own rect, which
 * is what the hover highlight paints. */
static int menu_item_at(int mi, int px, int py, struct drect *out_row)
{
    if (mi != g_menu_open) return -1;
    struct drect p; menu_dropdown_box(&p);
    if (px < p.x0 || px >= p.x1 || py < p.y0 || py >= p.y1) return -1;
    int y = p.y0 + MI_PAD_Y;
    const struct mi *items = g_menu_items[mi];
    int n = g_menu_nitems[mi];
    for (int i = 0; i < n; i++) {
        int h = menu_row_h(&items[i]);
        if (items[i].label && py >= y && py < y + h) {
            if (out_row) { out_row->x0 = p.x0; out_row->x1 = p.x1; out_row->y0 = y; out_row->y1 = y + h; }
            return i;
        }
        y += h;
    }
    return -1;
}

/* File's two items need a real focused app window; everything else is always
 * available (Expose is claimed even with zero windows -- see wm_shortcut). */
static int menu_item_enabled(int mi, int idx)
{
    (void)idx;
    if (mi == 1) {
        int wi = top_visible();
        return wi >= 0 && wins[wi].used && wins[wi].kind == WK_APP;
    }
    return 1;
}

static void menu_close(void)
{
    if (g_menu_open < 0) return;
    struct drect p; menu_dropdown_box(&p);
    g_menu_open = -1; g_menu_item_hov = -1;
    dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
    kprintf("[wm] menu closed\n");        /* a test syncs on this rather than guessing a delay */
}

/* The one place a menu item's action actually happens. Closes the menu on
 * every reachable path -- a fired action always dismisses, exactly like a
 * real menu -- so no caller has to remember to. */
static void menu_item_fire(int mi, int idx)
{
    if (mi == 0) {                                        /* LogitOS */
        switch (idx) {
        case 0: menu_close(); overlay_open(OV_ABOUT); return;
        case 1: menu_close(); wm_launch("settings.aex", ""); return;
        case 3: menu_close(); overlay_open(OV_CONFIRM_SHUTDOWN); return;
        case 4: menu_close(); overlay_open(OV_CONFIRM_RESTART); return;
        default: menu_close(); return;                    /* the separator: unreachable, but safe */
        }
    } else if (mi == 1) {                                 /* File */
        int wi = top_visible();
        menu_close();
        if (wi < 0 || !wins[wi].used || wins[wi].kind != WK_APP) return;
        struct win *w = &wins[wi];
        if (idx == 0) enqueue(w, EV_CLOSE, 0, 0);          /* same event the traffic light sends */
        else if (idx == 1) win_set_min(w, 1);
    } else {                                               /* View */
        menu_close();
        if (idx == 0) ex_enter();
    }
}

static void draw_menu_dropdown(void)
{
    struct drect p; menu_dropdown_box(&p);
    int w = p.x1 - p.x0, h = p.y1 - p.y0;
    fb_shadow(p.x0, p.y0, w, h, S(10), S(6), S(16), g_ui_dark ? 150 : 70);
    if (g_ui_dark) fb_liquid_glass(p.x0, p.y0, w, h, S(10), 30, 30, 38, 165);
    else           fb_liquid_glass(p.x0, p.y0, w, h, S(10), 250, 250, 255, 190);

    unsigned ac = settings_get_color("ui.accent", 0x5E96FF);
    uint8_t ar = (uint8_t)(ac >> 16), ag = (uint8_t)(ac >> 8), ab = (uint8_t)ac;

    const struct mi *items = g_menu_items[g_menu_open];
    int n = g_menu_nitems[g_menu_open];
    int y = p.y0 + MI_PAD_Y;
    for (int i = 0; i < n; i++) {
        int rh = menu_row_h(&items[i]);
        if (!items[i].label) {
            fb_fill_rect(p.x0 + S(8), y + rh / 2, w - S(16), S(1),
                         g_ui_dark ? rgb(70, 70, 80) : rgb(214, 214, 220));
        } else {
            int enabled = menu_item_enabled(g_menu_open, i);
            int hov = enabled && i == g_menu_item_hov;
            if (hov) fb_blend_round_rect(p.x0 + S(4), y, w - S(8), rh, S(6), ar, ag, ab, 200);
            uint32_t ink;
            if (!enabled) ink = g_ui_dark ? rgb(112, 112, 120) : rgb(180, 180, 186);
            else if (hov)  ink = rgb(255, 255, 255);
            else           ink = g_ui_dark ? rgb(228, 229, 235) : rgb(40, 40, 48);
            fb_text(p.x0 + MI_PAD_X, y + S(3), items[i].label, ink);
        }
        y += rh;
    }
}

/* ---- decimal formatting for the About panel -------------------------------
 * No printf in the kernel's draw path; two tiny helpers, same spirit as fmt2
 * above but unbounded (a frame count or a byte total is not two digits). */
static int str_copy(char *d, const char *s) { int i = 0; while (s[i]) { d[i] = s[i]; i++; } return i; }
static int fmt_u(char *d, unsigned long v)
{
    char tmp[24]; int n = 0;
    if (v == 0) { d[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) d[i] = tmp[n - 1 - i];
    return n;
}

static void draw_overlay_panel(void)
{
    struct drect p; overlay_box(&p);
    int w = p.x1 - p.x0, h = p.y1 - p.y0;
    fb_shadow(p.x0, p.y0, w, h, S(14), S(10), S(28), g_ui_dark ? 170 : 90);
    if (g_ui_dark) fb_liquid_glass(p.x0, p.y0, w, h, S(14), 28, 28, 36, 195);
    else           fb_liquid_glass(p.x0, p.y0, w, h, S(14), 250, 250, 255, 210);
    uint32_t ink = g_ui_dark ? rgb(232, 233, 238) : rgb(40, 40, 48);
    uint32_t dim = g_ui_dark ? rgb(172, 173, 182) : rgb(96, 96, 104);

    if (g_overlay == OV_ABOUT) {
        int tx = p.x0 + S(24), ty = p.y0 + S(20);
        fb_fill_circle(tx + S(7), ty + S(7), S(14), rgb(94, 150, 255));
        fb_text(tx + S(34), ty + S(2), "LogitOS", ink);
        ty += S(34);
        fb_text(tx, ty, "About This Machine", ink); ty += S(26);
        char line[64]; int n;
        fb_text(tx, ty, "Version: M27", dim); ty += S(20);
        n = str_copy(line, "Memory: ");
        n += fmt_u(line + n, (unsigned long)(pmm_total_bytes() / (1024 * 1024)));
        n += str_copy(line + n, " MB"); line[n] = 0;
        fb_text(tx, ty, line, dim); ty += S(20);
        n = str_copy(line, "Resolution: ");
        n += fmt_u(line + n, (unsigned long)W);
        n += str_copy(line + n, " x ");
        n += fmt_u(line + n, (unsigned long)H); line[n] = 0;
        fb_text(tx, ty, line, dim); ty += S(20);
        n = str_copy(line, "Processors: ");
        n += fmt_u(line + n, (unsigned long)smp_cpu_count()); line[n] = 0;
        fb_text(tx, ty, line, dim);
        return;
    }

    const char *msg = g_overlay == OV_CONFIRM_SHUTDOWN ? "Shut Down the machine now?"
                                                         : "Restart the machine now?";
    fb_text(p.x0 + (w - fb_text_width(msg)) / 2, p.y0 + S(30), msg, ink);
    struct drect c, o; overlay_buttons(&c, &o);
    int hb = g_overlay_btn_hov;
    uint32_t cbg = hb == 0 ? (g_ui_dark ? rgb(72, 72, 82) : rgb(220, 220, 226))
                           : (g_ui_dark ? rgb(56, 56, 64) : rgb(236, 236, 240));
    fb_round_rect(c.x0, c.y0, c.x1 - c.x0, c.y1 - c.y0, S(7), cbg);
    int ctw = fb_text_width("Cancel");
    fb_text(c.x0 + ((c.x1 - c.x0) - ctw) / 2, c.y0 + S(7), "Cancel", ink);

    uint32_t okbg = hb == 1 ? rgb(255, 82, 72) : rgb(230, 60, 52);
    fb_round_rect(o.x0, o.y0, o.x1 - o.x0, o.y1 - o.y0, S(7), okbg);
    const char *olabel = g_overlay == OV_CONFIRM_SHUTDOWN ? "Shut Down" : "Restart";
    int otw = fb_text_width(olabel);
    fb_text(o.x0 + ((o.x1 - o.x0) - otw) / 2, o.y0 + S(7), olabel, rgb(255, 255, 255));
}

/* The file browser is now the ring-3 Finder app (src/apps/gui/files.c), launched
 * at boot below; the old in-kernel WK_FINDER window was folded into it. */

/* ---------- window frame + compositing ---------- */
/* The three nested constant-alpha bands that used to be the window shadow are
 * gone; fb_shadow() is the one shadow in the system now, and it keeps the
 * perimeter-only property those bands existed for -- see the comment on it in
 * fb.c, which carries their reasoning forward. */

/* THE TITLEBAR/CONTENT SEAM (unit F, item 5). Both draw_frame_body and
 * draw_frame used to fill this hairline THEMSELVES, before their caller's
 * blit_content -- which starts at the exact same row, y+TBH, and is opaque.
 * The hairline was therefore painted and immediately overpainted, every
 * frame, and had never once reached the screen: what actually drew the
 * boundary was the raw jump from the titlebar's glass/gradient to the
 * content surface's flat fill, an accidental edge rather than a deliberate
 * one, and a vibrant material meeting a flat one with literally nothing
 * between them is exactly the "hard step" this item names. One function, and
 * BOTH call sites now invoke it AFTER blit_content -- on top of the
 * content's own first row, which is the only way to draw a pixel that a
 * same-frame full-opacity blit will not immediately erase -- so the pixel
 * that was always intended to be there finally is. */
static void draw_titlebar_sep(int x, int y, int ww)
{
    fb_fill_rect(x, y, ww, S(1), g_ui_dark ? rgb(60, 60, 70) : rgb(214, 214, 220));
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
    /* NOT the separator -- see draw_titlebar_sep above; the caller draws it
     * after blit_content, or it is covered before it is ever seen. */
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
    fb_shadow(x, y, ww, wh, S(10), WSH_DY(focused), WSH_BLUR(focused), WSH_ALPHA(focused));
    uint8_t a = focused ? (g_ui_dark ? 150 : 104) : (g_ui_dark ? 180 : 140);
    if (g_ui_dark) fb_liquid_glass(x, y, ww, TBH + S(10), S(10), 30, 30, 40, a);
    else           fb_liquid_glass(x, y, ww, TBH + S(10), S(10), 250, 250, 255, a);
    /* NOT the separator -- see draw_titlebar_sep above; the caller draws it
     * after blit_content, or it is covered before it is ever seen. */
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

/* Scratch surface for build_arrow(), reused across all four calls (they run
 * back to back at boot, never concurrently). This is exactly the size of one
 * cursor_plane entry (64x64x4) and it is `static`, not a local, on purpose:
 * this codebase has already been bitten once by a buffer this size on the
 * kernel stack -- the M11 note on the old 16 KiB TLS plaintext buffer records
 * the deeper HTTPS redirect path overflowing THE STACK INTO THE PAGE TABLES,
 * silently, because `stack_bottom` sits just above `pd_table` in boot.asm. The
 * fix there was the same one applied here before it has a chance to be needed
 * again: kernel .bss, not a kernel stack frame. */
static unsigned char arrow_scratch[CUR_PLANE * CUR_PLANE * 4];

/* Build a double-headed arrow along the unit vector (ux,uy), given in /256
 * fixed point, as a gfx PATH rather than a per-pixel coverage test -- "two
 * triangles and a bar" is a literal description of the ten vertices below,
 * not a metaphor. (a,b) are the same axis/perpendicular coordinates the old
 * per-pixel test projected each pixel INTO; here they go the other way,
 * placing each VERTEX by the same projection run backwards. Because a,b are
 * plain device-pixel integers and ux,uy are already /256 fixed point,
 * `a*ux - b*uy` lands directly in 24.8 device fixed point with no separate
 * scale step -- turning the test inside out like this is what makes the
 * shape a path instead of a bespoke rasterizer, and it is why the diagonals
 * still cost nothing extra over the axis-aligned pair. */
static void build_arrow(uint32_t *dst, int ux, int uy)
{
    const int c = CUR_PLANE / 2;
    int t = S(1); if (t < 1) t = 1;                 /* outline thickness */
    int R = S(11), bar = S(2), hd = S(6), hw = S(6);
    /* The plane is fixed at 64 and the UI scale is not, so clamp rather than
     * write past it: at scale 300 an unclamped R would be 33 and the arrow's
     * far head would wrap onto the opposite row. Unchanged from the deleted
     * per-pixel version, including its one imprecision: a mitred corner (see
     * the outline below) can reach a hair past R+t at the tips, exactly as
     * the old Chebyshev dilation could at a diagonal corner -- neither
     * version budgeted for that, and in practice t is 1-3 device px, so the
     * overshoot is sub-pixel and clips into nothing a screenshot shows. */
    if (R + t > c - 1) { int m = c - 1 - t; if (hd > m) hd = m; if (hw > m) hw = m; R = m; }
    if (R < 4) R = 4;
    int L = R - hd;

    int pts[32], subs[2];
    struct gfx_path arrow;
    gfx_path_init(&arrow, pts, 16, subs, 2);
    int cx = GFX_PX(c), cy = GFX_PX(c);
#define APT(a, b) (cx + (a) * ux - (b) * uy), (cy + (a) * uy + (b) * ux)
    gfx_move_to(&arrow, APT(R, 0));      /* tip+                              */
    gfx_line_to(&arrow, APT(L, hw));     /* head+ base, +b side               */
    gfx_line_to(&arrow, APT(L, bar));    /* shoulder+ (head base -> bar edge) */
    gfx_line_to(&arrow, APT(-L, bar));   /* straight across the bar top       */
    gfx_line_to(&arrow, APT(-L, hw));    /* head- base, +b side               */
    gfx_line_to(&arrow, APT(-R, 0));     /* tip-                              */
    gfx_line_to(&arrow, APT(-L, -hw));   /* head- base, -b side               */
    gfx_line_to(&arrow, APT(-L, -bar));  /* shoulder-, -b side                */
    gfx_line_to(&arrow, APT(L, -bar));   /* straight across the bar bottom    */
    gfx_line_to(&arrow, APT(L, -hw));    /* head+ base, -b side               */
    /* Close by repeating the FIRST point, not by relying on gfx_close() alone
     * -- gfx_close() (gfx_path.c) emits no point, so a fill would treat this
     * subpath as closed either way, but gfx_stroke_path (gfx_stroke.c's file
     * comment) tells closed from open ONLY by "last point equals first".
     * Without this line the stroke below would see a 10-point OPEN polyline
     * and cap both ends instead of ringing the outline -- a trap documented
     * in that file and worth repeating here since this is the first caller. */
    gfx_line_to(&arrow, APT(R, 0));
    gfx_close(&arrow);
#undef APT

    struct gfx_surface surf;
    gfx_surface_init(&surf, arrow_scratch, CUR_PLANE, CUR_PLANE, CUR_PLANE * 4);
    gfx_surface_clear(&surf);

    /* The outline: ONE stroke of the arrow's own path, centred (width 2t, t
     * in and t out) rather than two strokes or a hand-built dilated polygon.
     * It is painted BEFORE the fill (below), so the fill's opaque interior
     * covers exactly the inner half -- what survives is a t-wide ring OUTSIDE
     * the fill, the same halo width the old box dilation produced, but with a
     * real mitred corner at the tips instead of a Chebyshev-square
     * approximation of one. MITER, not ROUND: every turn in this outline (tip
     * and shoulder alike) is close to 90 degrees by construction -- see the
     * (a,b) layout above -- nowhere near the near-180-degree turns a limit of
     * 4 would ever fall back to bevel on, so the halo stays exactly as
     * pointed as the fill it traces. */
    int spts[128], ssubs[4];
    struct gfx_path outline;
    gfx_path_init(&outline, spts, 64, ssubs, 4);
    struct gfx_stroke sk = { 0 };
    sk.width = 2 * t * GFX_ONE;
    sk.join = GFX_JOIN_MITER;
    sk.miter_limit = 4 << 16;            /* SVG default; every corner here clears it */
    struct gfx_paint outp;
    gfx_paint_solid(&outp, GFX_RGB(20, 20, 26), 255);
    /* A refusal here (out-path too small) leaves the halo unpainted rather
     * than corrupt -- it should never fire: a 10-vertex closed miter outline
     * needs on the order of 2x that many points, well under the 64-point/
     * 4-subpath budget above. */
    if (gfx_stroke_path(&outline, &arrow, &sk))
        gfx_fill(&surf, &outline, GFX_NONZERO, &outp, NULL);

    struct gfx_paint fillp;
    gfx_paint_solid(&fillp, GFX_RGB(255, 255, 255), 255);
    gfx_fill(&surf, &arrow, GFX_NONZERO, &fillp, NULL);

    /* gfx_fill composited straight RGBA (R,G,B,A byte order -- gfx.h's
     * surface comment) through the engine's own Porter-Duff gfx_over, which
     * is what makes the fill's antialiased edge blend smoothly into the
     * outline underneath instead of stair-stepping between two binary masks.
     * Repack into the plane's own ARGB word, through the SAME rgb() (=
     * fb_rgb(), device-native channel order) the deleted version used, so
     * the two cursor-plane consumers below need no format change at all. */
    for (int j = 0; j < CUR_PLANE; j++)
        for (int i = 0; i < CUR_PLANE; i++) {
            const unsigned char *px = arrow_scratch + (j * CUR_PLANE + i) * 4;
            dst[j * CUR_PLANE + i] = px[3] ? ((uint32_t)px[3] << 24) | rgb(px[0], px[1], px[2]) : 0;
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

/* Which byte lane fb_rgb() (c/kernel/gui/fb.c) packs each channel into.
 * fb.c keeps red_pos/green_pos/blue_pos to itself, so this asks fb_rgb()
 * itself, on pure primaries, rather than duplicating the multiboot2 FB tag
 * parse: whichever byte a channel's 0xFF lands in IS its shift. That is
 * exactly what fb.c's own (private) unpack() assumes too -- one full
 * byte-aligned lane per channel, no fractional field width -- so this is not
 * a new assumption, just one this file did not previously need to state.
 * Cached after the first call: draw_cursor_back() below calls it once per
 * cursor pixel with any fringe alpha, and the device's channel layout is
 * fixed for the life of the boot. */
static int chan_shift(uint32_t probe)
{
    if (probe & 0x0000FFu) return 0;
    if (probe & 0x00FF00u) return 8;
    return 16;
}
static void device_shifts(int *rs, int *gs, int *bs)
{
    static int have, r_, g_, b_;
    if (!have) {
        r_ = chan_shift(rgb(255, 0, 0));
        g_ = chan_shift(rgb(0, 255, 0));
        b_ = chan_shift(rgb(0, 0, 255));
        have = 1;
    }
    *rs = r_; *gs = g_; *bs = b_;
}

/* The cursor is composited into `back` on top of everything else, then presented
 * with the rest of the frame -- no save-under overlay (that restored a stale
 * `back` and smeared garbage as the cursor moved).
 *
 * Fractional alpha is real now (build_arrow() is a gfx fill+stroke, not a
 * binary coverage test), so a pixel that only ever went fully-transparent or
 * fully-opaque before now also lands on every value between -- the whole
 * antialiased fringe the new build_arrow buys. Painting the source colour at
 * full strength wherever alpha is merely NONZERO -- what this function did
 * before the pixels it reads could BE fractional -- would grow the fringe
 * into a ring of full-strength pixels instead of blending it, which is a
 * different-looking staircase, not antialiasing. So: blend, using `back`
 * directly (the same buffer fb_put's default target already is -- see the
 * `back` global above) because there is no exported "read one screen pixel"
 * and device_shifts() above is what makes unpacking it possible without one.
 * Full alpha keeps the cheap direct fb_put -- no read, no blend math -- since
 * that is still the overwhelming common case (the shape's solid interior). */
static void draw_cursor_back(int x, int y)
{
    const uint32_t *p = cursor_plane[cur_shape];
    int hx = cursor_hot[cur_shape][0], hy = cursor_hot[cur_shape][1];
    int bw = cursor_box[cur_shape][0], bh = cursor_box[cur_shape][1];
    int rs, gs, bs;
    device_shifts(&rs, &gs, &bs);
    for (int j = 0; j < bh; j++)
        for (int i = 0; i < bw; i++) {
            uint32_t v = p[j * CUR_PLANE + i];
            int a = (int)(v >> 24);
            if (!a) continue;
            int px = x + i - hx, py = y + j - hy;
            if (a >= 255 || !back || px < 0 || py < 0 || px >= W || py >= H) {
                fb_put(px, py, v & 0x00FFFFFFu);
                continue;
            }
            uint32_t dc = back[py * W + px];
            int sr = (int)((v >> rs) & 0xFF), sg = (int)((v >> gs) & 0xFF), sb = (int)((v >> bs) & 0xFF);
            int dr = (int)((dc >> rs) & 0xFF), dg = (int)((dc >> gs) & 0xFF), db = (int)((dc >> bs) & 0xFF);
            int nr = (sr * a + dr * (255 - a)) / 255;
            int ng = (sg * a + dg * (255 - a)) / 255;
            int nb = (sb * a + db * (255 - a)) / 255;
            fb_put(px, py, rgb((uint8_t)nr, (uint8_t)ng, (uint8_t)nb));
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
/* PURE NOW -- it used to clear open_t0 on the frame it expired, which made
 * "where is this window" a question that changed the answer to the next one.
 * wm_anim_tick() owns every timer's end; see the note on win_draw_rect. */
static int win_open_scale(const struct win *w)
{
    if (!w->open_t0) return 0;
    uint64_t e = timer_ticks() - w->open_t0;
    if (e >= OPEN_DUR_TICKS) return 0;
    int t = (int)(e * 256 / OPEN_DUR_TICKS), inv = 256 - t;
    int eased = 256 - inv * inv * inv / (256 * 256);   /* easeOutCubic */
    return 216 + (256 - 216) * eased / 256;            /* 0.84x -> 1.0x over ~0.16s */
}

/* ---- WHY THE ANIMATED PATH IS BILINEAR ------------------------------------
 *
 * An Expose thumbnail is a MINIFICATION of three to four times, and that is
 * exactly the regime where nearest-neighbour stops being a sampling choice and
 * starts deleting information: at 1/3 scale it keeps one row in three, so a
 * window's text is not blurred, it is GONE in stripes -- and two different
 * windows can come out looking like the same window, which is the one thing a
 * picker may not do.
 *
 * fb_blit_surface_scaled_bl (c/kernel/gui/fb.c) is the bilinear variant, and
 * its own header prices it at ~4.3x nearest per pixel and scopes it to "the ONE
 * window currently under an open/close pop or a live resize drag ... not for
 * every window a compositor redraws every frame regardless of motion". This use
 * is inside that scope and not an exception to it: the 4x is paid ONLY by
 * windows that are moving, only while they are moving, and only over the
 * DESTINATION rectangle -- which during Expose is a quarter-size thumbnail, so
 * the four taps are charged against a sixteenth of the area they would cover at
 * full size. A window sitting still never reaches this line; it takes the
 * direct path in render_region and is blitted 1:1 as it always was. */
#define wm_scaled_blit fb_blit_surface_scaled_bl

/* ---- constant-alpha scaled blit, and why it is HERE ------------------------
 *
 * fb.c offers an opaque scaled blit and an RGBA blit whose alpha must be IN the
 * source bytes. The dock fly needs neither: one image, one scale, and one alpha
 * that changes every frame. Expressing a single number by rewriting an alpha
 * byte into every pixel of a 750x544 window, 18 times, is the wrong shape of
 * work -- and fb.c belongs to another line this week, so the composite is done
 * against `back`, which IS the target fb_target(NULL) selects (see wm_init's
 * fb_set_backbuffer(back)).
 *
 * IT TAKES ITS CLIP EXPLICITLY rather than reading fb.c's per-surface scissor.
 * The caller already has it -- render_region is handed the damage rectangle and
 * every primitive it calls is clipped to exactly that -- and passing it in
 * means this cannot be called from somewhere that has not thought about it.
 *
 * The channel shifts are PROBED from fb_rgb() rather than assumed to be
 * 0x00RRGGBB: fb.c carries rpos/gpos/bpos from the multiboot tag and does not
 * export them, and a hardcoded layout here would be a colour-swap bug on the
 * one machine whose framebuffer disagrees. Three calls, once, at first use. */
static int fade_rsh, fade_gsh, fade_bsh, fade_probed;
static void fade_probe(void)
{
    if (fade_probed) return;
    fade_probed = 1;
    uint32_t rm = fb_rgb(255, 0, 0), gm = fb_rgb(0, 255, 0), bm = fb_rgb(0, 0, 255);
    while (fade_rsh < 24 && !((rm >> fade_rsh) & 1)) fade_rsh++;
    while (fade_gsh < 24 && !((gm >> fade_gsh) & 1)) fade_gsh++;
    while (fade_bsh < 24 && !((bm >> fade_bsh) & 1)) fade_bsh++;
}

/* Scale `src` into (dx,dy,dw,dh) of `back`, blended at a CONSTANT alpha,
 * clipped to `clip` and to the screen. Priced by the DESTINATION rect -- which
 * is why the fly only fades over its second half, when the destination has
 * shrunk (see win_draw_rect). Row-addressed, integer, no per-pixel call. */
static void anim_blit_fade(const struct drect *clip, int dx, int dy, int dw, int dh,
                           const struct surface *src, int alpha)
{
    if (!back || !src->px || dw <= 0 || dh <= 0 || alpha <= 0) return;
    fade_probe();
    int x0 = dx, y0 = dy, x1 = dx + dw, y1 = dy + dh;
    if (x0 < clip->x0) x0 = clip->x0;
    if (y0 < clip->y0) y0 = clip->y0;
    if (x1 > clip->x1) x1 = clip->x1;
    if (y1 > clip->y1) y1 = clip->y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1) return;
    int ia = 255 - alpha;
    for (int y = y0; y < y1; y++) {
        int sy = (y - dy) * src->h / dh;
        if (sy < 0) sy = 0;
        if (sy >= src->h) sy = src->h - 1;
        const uint32_t *srow = src->px + (long)sy * src->w;
        uint32_t *drow = back + (long)y * W;
        for (int x = x0; x < x1; x++) {
            int sx = (x - dx) * src->w / dw;
            if (sx < 0) sx = 0;
            if (sx >= src->w) sx = src->w - 1;
            uint32_t s = srow[sx], d = drow[x];
            int r = ((int)((s >> fade_rsh) & 0xFF) * alpha + (int)((d >> fade_rsh) & 0xFF) * ia) / 255;
            int g = ((int)((s >> fade_gsh) & 0xFF) * alpha + (int)((d >> fade_gsh) & 0xFF) * ia) / 255;
            int b = ((int)((s >> fade_bsh) & 0xFF) * alpha + (int)((d >> fade_bsh) & 0xFF) * ia) / 255;
            drow[x] = ((uint32_t)r << fade_rsh) | ((uint32_t)g << fade_gsh) | ((uint32_t)b << fade_bsh);
        }
    }
}

/* The scratch surface every animated window is rendered into at FULL size
 * before being scaled down. Grown, never shrunk: the alternative is a
 * multi-megabyte kmalloc/kfree pair per frame of every animation. */
static struct surface *anim_scratch(int w, int h)
{
    static struct surface s;
    int need = w * h;
    if (need <= 0) return NULL;
    if (need > anim_buf_n) {
        if (anim_buf) kfree(anim_buf);
        anim_buf = kmalloc((unsigned)need * 4);
        anim_buf_n = anim_buf ? need : 0;
    }
    if (!anim_buf) return NULL;
    s.px = anim_buf; s.w = w; s.h = h;
    s.clip_on = 0;                       /* scratch is unclipped -- see fb.h */
    return &s;
}

/* ---- an animated window is drawn in the TWO PIECES it is made of -----------
 *
 * The obvious implementation -- render the whole window into a scratch surface
 * at full size, then scale that down -- is what the open pop did, and it is
 * affordable there because the pop scales ONE window by 16% for a sixth of a
 * second. Expose moves EVERY window at once, to a quarter of its size, and
 * paying full window area TWICE per window per frame (once to compose the
 * scratch, once to copy the app's canvas into it) made a single animated frame
 * cost more than the compositor's entire steady-state frame.
 *
 * That is not a theory. Measured on this machine, at 1280x800 with four
 * windows, the whole-window version presented THREE of the flight's eighteen
 * frames: the geometry was provably correct on the wire and nobody could see
 * it move. So:
 *
 *   THE CONTENT is the app's retained canvas, and it is scaled STRAIGHT from
 *   there into the destination. There is no intermediate copy to pay for, and
 *   the cost is the destination rectangle -- which is the thing being drawn.
 *   This is also the honest reading of "the surface IS the content": nobody
 *   asks the app to redraw, and nobody copies its pixels twice to avoid it.
 *
 *   THE CHROME is ours, and it is the only part that has to be composed. Only
 *   the TITLEBAR is ever visible (the content covers everything below it, in
 *   the animated path exactly as in the ordinary one), so only a w x TBH strip
 *   is rendered at full size and minified. That is what keeps a thumbnail's
 *   titlebar text the REAL text resampled, instead of the UI font
 *   re-rasterized at four points -- which is the whole reason to compose it at
 *   full size rather than draw it small.
 *
 * Cost per window per frame: from 2*W*H to (dest area + W*TBH). For the
 * Terminal at 900x590 scaled into a 400x260 cell, 1.06 M pixels to 131 K.
 *
 * TBH IS TALL ENOUGH to contain the rounded top corners -- radius S(10) against
 * a S(30) titlebar -- so nothing of the window's outline is lost by cutting the
 * strip there. If TITLEBAR_H ever drops below the corner radius, this strip has
 * to grow with it. */
static struct surface *win_chrome_strip(struct win *w, int focused)
{
    int sh = TBH;
    if (sh > w->h) sh = w->h;
    if (sh < 1) return NULL;
    struct surface *tmp = anim_scratch(w->w, sh);
    if (!tmp) return NULL;
    fb_target(tmp);
    /* The whole body is asked for and the scratch's own height clips it to the
     * strip: the rounded top corners, the titlebar gradient, the three lights
     * and the title are exactly what survives, and they are exactly what shows. */
    draw_frame_body(0, 0, w->w, w->h, w->title, focused);
    fb_target(NULL);
    return tmp;
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
            if (g_menu_open >= 0) {                       /* the open dropdown -- also glass */
                menu_dropdown_box(&p);
                if (rect_hit(&r[i], &p) && !rect_in(&p, &r[i])) { rect_or(&r[i], &p); grew = 1; }
            }
            if (g_overlay != OV_NONE) {                   /* About / Shut Down / Restart panel */
                overlay_box(&p);
                if (rect_hit(&r[i], &p) && !rect_in(&p, &r[i])) { rect_or(&r[i], &p); grew = 1; }
            }
            for (int k = 0; k < norder; k++) {           /* each window's glass titlebar */
                /* Asked of win_glass_box, not of w->x/w->w: a window being
                 * flown or scaled has NO glass this frame (it is rendered
                 * through a scratch surface), and protecting a panel that is
                 * not being drawn would grow this rectangle to a frame the
                 * window is not occupying. See win_drawn_direct. */
                if (!win_glass_box(&wins[order[k]], &p)) continue;
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
    /* LOCKED: exactly ONE window is composited, it fills the screen, and it is
     * drawn with no frame at all -- no titlebar, so no close button exists to
     * be hit-tested, and no dock and no menu bar. There is nothing behind it
     * to leak because nothing else has been launched: see the g_locked block
     * comment, and note that this branch is the COSMETIC half of the lock.
     * wm_launch()'s refusal is the half that matters. */
    if (g_locked) {
        int gw = greeter_win();
        if (gw >= 0 && wins[gw].used && wins[gw].surf.px)
            blit_content(&wins[gw], 0, 0, W, H);
        if (!hw_cursor) draw_cursor_back(mx, my);
        fb_clear_clip();
        uint64_t t_lock = time_mono_ns();
        fb_present_rect(R->x0, R->y0, rw, rh);
        perf_present_ns += time_mono_ns() - t_lock;
        perf_cpx += (uint64_t)rw * (uint64_t)rh;
        return 0;
    }

    /* THE DIM, behind the Expose grid, so the windows read as the content and
     * the desktop as backdrop.
     *
     * CONSTANT while the mode is up -- it appears on the frame the gesture
     * starts and leaves on the frame it ends, and never changes in between.
     * That is a damage decision, stated rather than hidden: a dim that RAMPED
     * with the transition would change every pixel of the desktop on every one
     * of the 18 frames, which is a full-screen composite per tick and exactly
     * the thing the per-window damage below exists to avoid. Two full frames
     * per gesture against thirty-six. The cost of the choice is that the
     * darkening arrives in one step instead of fading in; the windows are
     * already moving on that same frame, so it reads as part of the gesture.
     *
     * From MBH down: the menu bar is glass that samples the backdrop under
     * ITSELF (y < MBH), and dimming that would darken the bar rather than the
     * desktop. The dock's glass does sample dimmed pixels, and should -- it is
     * sitting on a dimmed desktop. */
    if (ex_state()) fb_blend_rect(0, MBH, W, H - MBH, 0, 0, 0, EX_DIM_ALPHA);

    int focus_wi = top_visible();          /* NOT order[norder-1]: that may be minimised */
    int exp = ex_prog();
    for (int i = 0; i < norder; i++) {     /* windows, back-to-front */
        struct win *w = &wins[order[i]];
        if (!w->used) continue;
        /* WHERE IT IS, asked once, of the one function that knows -- and that
         * the damage list and the mid-frame guard asked the same question of.
         * 0 means the window is not on screen at all (minimised, and the picker
         * is not up). */
        int dx, dy, dw, dh, alpha;
        if (!win_draw_rect(w, &dx, &dy, &dw, &dh, &alpha)) continue;
        struct drect b; win_box(w, &b);
        if (!rect_hit(&b, R)) continue;    /* nothing of this window is in the rect */
        int focused = (order[i] == focus_wi);
        int k = ex_slot(order[i]);

        /* THE COMMON CASE IS UNTOUCHED. A window sitting still at its own frame
         * takes exactly the path it always took -- real liquid glass sampling
         * the live backdrop, content blitted 1:1, no scratch buffer, no scale.
         * Everything below this line only runs for a window that is moving. */
        if (win_drawn_direct(w, dx, dy, dw, dh, alpha)) {
            draw_frame(w, focused);
            /* The canvas is allowed to be a size behind the frame mid-drag, so
             * the content is stretched rather than left as a hole -- see
             * blit_content and RESIZE_APPLY_MS. */
            blit_content(w, w->x, w->y + TBH, w->w, w->h - TBH);
            draw_titlebar_sep(w->x, w->y + TBH, w->w);  /* after the blit -- see draw_titlebar_sep */
            continue;
        }

        if (win_open_scale(w)) animating = 1;   /* only the pop wants a full frame */

        /* THE PICK HIGHLIGHT, drawn BEFORE the thumbnail so it survives as a rim
         * around an opaque blit instead of being painted over it. Cheap for the
         * same reason the window's own shadow is: the middle is overdrawn. */
        int sr = w->w > 0 ? dw * 256 / w->w : 256;      /* thumbnail scale, /256 */
        int rad = S(10) * sr / 256;
        if (rad < S(2)) rad = S(2);
        if (k >= 0 && k == ex_hov)
            fb_blend_round_rect(dx - S(4), dy - S(4), dw + S(8), dh + S(8), rad + S(4),
                                255, 255, 255, 210);

        /* A SCALED WINDOW STILL CASTS A SHADOW -- and it is what separates a
         * thumbnail from a sticker printed on the wallpaper. The offset and
         * blur scale with the thumbnail, because a full-size S(32) blur around
         * a quarter-size window is a smudge the width of the window itself.
         * win_box still declares the FULL-SIZE extent, so the damage is a
         * superset of what is painted -- over-reporting is only slow. */
        int sdy = WSH_DY(focused) * sr / 256, sbl = WSH_BLUR(focused) * sr / 256;
        if (sbl < S(4)) sbl = S(4);
        if (sdy < 1) sdy = 1;
        if (sdy >= sbl) sdy = sbl - 1;
        fb_shadow(dx, dy, dw, dh, rad, sdy, sbl, (uint8_t)(WSH_ALPHA(focused) * alpha / 255));

        /* Chrome strip, then content -- the same order draw_frame and
         * blit_content use, and for the same reason: the content owns every
         * row below the titlebar and is drawn last there. The two destination
         * rects ABUT and do not overlap, so with a fade every pixel is blended
         * exactly once; overlapping them would blend the seam rows twice and
         * show a darker band that gets darker as the window fades. */
        int tbh_s = TBH * sr / 256;
        if (tbh_s < 1) tbh_s = 1;
        if (tbh_s > dh) tbh_s = dh;
        struct surface *cs = win_chrome_strip(w, focused);
        if (cs) {
            if (alpha >= 255) wm_scaled_blit(dx, dy, dw, tbh_s, cs);
            else              anim_blit_fade(R, dx, dy, dw, tbh_s, cs, alpha);
        }
        if (w->surf.px && dh - tbh_s > 0) {
            if (alpha >= 255) wm_scaled_blit(dx, dy + tbh_s, dw, dh - tbh_s, &w->surf);
            else              anim_blit_fade(R, dx, dy + tbh_s, dw, dh - tbh_s, &w->surf, alpha);
            if (w->drawing) perf_torn++;   /* counted at every path that shows a canvas */
        }
        /* The hairline, only while opaque: one row at a third of an alpha on a
         * shrinking window is not a boundary anybody can see, and drawing it
         * would be the one thing in this block blended a second time. */
        if (alpha >= 255) draw_titlebar_sep(dx, dy + tbh_s, dw);

        /* THE TITLE, centred beneath the cell in the UI font -- and only once
         * the grid has mostly arrived. A label chasing a moving thumbnail is
         * noise during the flight and information after it. Its extent is
         * declared in win_box, so a title wider than its window is damaged. */
        if (k >= 0 && exp > 128) {
            int tw = fb_text_width(w->title);
            int tx = dx + (dw - tw) / 2, ty = dy + dh + S(6);
            fb_text(tx, ty, w->title,
                    k == ex_hov ? rgb(255, 255, 255) : rgb(214, 216, 226));
        }
    }
    { struct drect p; menubar_box(&p);                  /* frosted chrome ON TOP: */
      if (rect_hit(&p, R)) draw_menubar(); }            /* real-time vibrancy      */
    { struct drect p; dock_box(&p);
      if (rect_hit(&p, R)) draw_dock(); }
    /* The menu dropdown and the About/power panel, drawn like the dock: fresh
     * from state, never retained, and above every window and the dock (a menu
     * hangs off the chrome that opened it, so it has to win). */
    if (g_menu_open >= 0) { struct drect p; menu_dropdown_box(&p);
      if (rect_hit(&p, R)) draw_menu_dropdown(); }
    if (g_overlay != OV_NONE) { struct drect p; overlay_box(&p);
      if (rect_hit(&p, R)) draw_overlay_panel(); }
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

/* ---- one pass of every animation ------------------------------------------
 *
 * THE ONLY PLACE A TIMER ENDS. Everything that reports a position (min_prog,
 * ex_prog, win_open_scale) is pure and can be asked twice in a frame with the
 * same answer; this runs once per pass of the WM loop and is what turns "the
 * duration has elapsed" into "the animation is over".
 *
 * IT IS ALSO WHERE THE DAMAGE COMES FROM, which is the half worth reading. A
 * moving window's damage is (where it WAS | where it IS) -- the union, because
 * the pixels it uncovers are exactly as changed as the ones it covers, and
 * reporting only the destination is the single most visible way to get a
 * partial compositor wrong. That is not a hypothetical here: WM_DAMAGE_LIE and
 * WM_RESIZE_DAMAGE_LIE exist in this file to make that mistake on purpose and
 * be caught. `anim_prev` is the "where it was" half; it cannot be re-derived
 * from the clock, so it is stored.
 *
 * AND THE LAST FRAME PAYS FOR THE WHOLE SWEEP, once. A per-frame union is
 * airtight only if no frame was ever skipped -- and one IS skipped whenever
 * this loop runs late (an app holding the BKL through a slow syscall, or the
 * mid-frame guard deferring the rectangle a window is inside). Re-laying the
 * entire path in a single rectangle when the flight lands costs one frame and
 * cannot be wrong, which is the right trade for the one frame nobody sees. */
/* THE FLIGHT, ON THE WIRE.
 *
 * A screenshot can show a window between two places; it cannot show that the
 * compositor MEANT to put it there rather than having drawn it late, or once,
 * or at a size it will keep. These lines are what let a harness assert the
 * geometry is genuinely interpolated -- the same argument as the existing
 * `[wm] win N frame ...` report, which exists because reading geometry back out
 * of a screendump is pixel archaeology that breaks the day the titlebar is
 * restyled, and which cannot help here because it only speaks when things STOP.
 *
 * BOUNDED, which is the only reason it is safe to leave compiled in: at most
 * one line per moving window per animated frame, only while a deliberate
 * gesture is in flight (~18 frames), and never once at idle. This console is
 * also /bin/sh's stdout -- make test-shell reads bytes off it -- so a
 * compositor that narrated every frame would interleave itself into another
 * harness's expected output. Nothing else in this tree performs these
 * gestures, so nothing else can provoke a line. */
/* THE ANIMATION'S NEGATIVE CONTROL, and it is a THIRD distinct mistake from
 * WM_DAMAGE_LIE (which shrinks a window's reported box) and
 * WM_RESIZE_DAMAGE_LIE (which forgets the OLD box when a window changes shape).
 * This one reports the destination honestly and forgets where the window was
 * ON THE PREVIOUS FRAME -- which is the error an animation invites, because an
 * animation is the only thing here that moves a window many times without any
 * input event in between, so there is no click or drag to blame the leftovers
 * on. Set to 1 and every frame of a flight paints the window in its new place
 * and leaves the old one standing: a window flying to the dock smears a comet
 * trail of itself across the desktop, and Expose leaves every window's previous
 * position behind it, all the way from the frame to the cell.
 *
 * tests/qmp/qmp_motion.py --negative flips this exact line in a throwaway copy
 * of the tree and REQUIRES its round-trip checks to fail. Without that, "the
 * desktop comes back pixel-identical" is a claim about a test that has never
 * once failed, which is evidence of nothing. */
#define WM_ANIM_DAMAGE_LIE 0

/* Damage (where it was | where it is), and store where it is for next time.
 * One function, because all three animations have the same obligation and a
 * fourth would otherwise be free to forget half of it. */
static void anim_damage(struct win *w)
{
    struct drect cur, u;
    win_box(w, &cur);
    u = cur;
#if !WM_ANIM_DAMAGE_LIE
    rect_or(&u, &w->anim_prev);
#endif
    w->anim_prev = cur;
    dirty_rect(u.x0, u.y0, u.x1 - u.x0, u.y1 - u.y0);
}

static void anim_trace(const char *what, int wi, const struct win *w, int p)
{
    int x, y, ww, wh, a;
    if (!win_draw_rect(w, &x, &y, &ww, &wh, &a)) return;
    kprintf("[wm] anim %s win %d p %d rect %d %d %d %d alpha %d home %d %d %d %d\n",
            what, wi, p, x, y, ww, wh, a, w->x, w->y, w->w, w->h);
}

static void wm_anim_tick(void)
{
    uint64_t t = timer_ticks();

    /* 1. THE OPEN POP -- behaviour unchanged, deliberately: it rescales a whole
     *    window every frame, one whole-screen pass is both cheaper than
     *    tracking that and impossible to get subtly wrong, and it lasts about a
     *    sixth of a second. Only its EXPIRY moved here, out of win_open_scale,
     *    so that reading a window's position stopped mutating it. */
    for (int i = 0; i < MAXWIN; i++) {
        struct win *w = &wins[i];
        if (!w->used || !w->open_t0) continue;
        if (t - w->open_t0 >= OPEN_DUR_TICKS) w->open_t0 = 0;
        else dirty_full();
    }

    /* 2. THE DOCK FLY. */
    for (int i = 0; i < MAXWIN; i++) {
        struct win *w = &wins[i];
        if (!w->used || !w->min_t0) continue;
        if (t - w->min_t0 >= MINFLY_TICKS) {
            struct drect s;
            win_fly_sweep(w, &s);
            w->min_t0 = 0;
#if !WM_ANIM_DAMAGE_LIE
            dirty_rect(s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0);
#endif
            dirty_dock();          /* a running dot's window arrived, or left */
            continue;
        }
        anim_damage(w);
        anim_trace(w->min_dir ? "min" : "restore", i, w, min_prog(w));
    }

    /* A window closing while the picker is up leaves a hole in the grid, which
     * is harmless. ALL of them closing leaves a dimmed desktop with nothing to
     * pick, the pointer still captured by wm_expose_mouse and the keyboard
     * still swallowed -- recoverable by clicking, but a state the machine
     * should not sit in waiting to be rescued. */
    if (ex_on && !ex_t0) {
        int alive = 0;
        for (int k = 0; k < ex_n; k++) if (wins[ex_wi[k]].used) { alive = 1; break; }
        if (!alive) ex_leave(-1);
    }

    /* 3. EXPOSE. The dim is constant while the mode is up (see render_region),
     *    so every frame between the two full ones pays for the windows that
     *    moved and for nothing else. */
    if (ex_t0) {
        if (t - ex_t0 >= EX_DUR_TICKS) {
            ex_t0 = 0;
            dirty_full();          /* entering: the swept area. leaving: the dim. */
        } else {
            for (int k = 0; k < ex_n; k++) {
                struct win *w = &wins[ex_wi[k]];
                if (!w->used) continue;
                anim_damage(w);
                anim_trace(ex_on ? "expose" : "unexpose", ex_wi[k], w, ex_prog());
            }
        }
    }
}

/* Has the pointer been parked in the hot corner long enough? See the comment on
 * ex_corner_t0 for why this corner and why a dwell. */
static void wm_hotcorner_tick(void)
{
    if (g_locked || ex_state()) { ex_corner_t0 = 0; return; }
    int inside = (mx >= W - S(EX_CORNER_PT)) && (my < S(EX_CORNER_PT));
    if (!inside) { ex_corner_t0 = 0; ex_corner_armed = 1; return; }
    if (!ex_corner_armed) return;              /* fired already; leave to re-arm */
    if (!ex_corner_t0) {
        ex_corner_t0 = timer_ticks();
        if (!ex_corner_t0) ex_corner_t0 = 1;
        return;
    }
    if (timer_ticks() - ex_corner_t0 < EX_DWELL_TICKS) return;
    ex_corner_armed = 0;
    ex_corner_t0 = 0;
    ex_enter();
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
    case 'e':
        /* Expose. Claimed even with no windows open, like Cmd+W above: a
         * shortcut that is a system shortcut only when it happens to have
         * something to do is one an app can learn to swallow. */
        ex_enter();
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
    /* NO SYSTEM SHORTCUTS WHILE LOCKED. Cmd+W and Cmd+Q both send EV_CLOSE,
     * and a greeter a keystroke can close is not a greeter -- the app ignores
     * EV_CLOSE too, but the right place to stop it is before it is generated.
     * The window is also resolved directly rather than through top_visible():
     * the greeter is the only window there is, and "the only window" is a
     * stronger statement than "the top one". */
    /* EXPOSE OWNS THE KEYBOARD while it is up, for the same reason it owns the
     * pointer: the focused window is displaced and scaled, so a keystroke
     * delivered to it would be typed into something the user cannot see at the
     * size they are looking at. Escape and the chord dismiss; Enter picks what
     * the pointer is over; everything else is swallowed rather than forwarded.
     * Swallowed, not queued -- a burst of typing replayed into an app the
     * instant the grid closes is worse than a burst that was never delivered. */
    if (!g_locked && ex_state()) {
        if (c == 27) ex_leave(-1);                                   /* Escape */
        else if ((mods & EV_MOD_SUPER) && (c == 'e' || c == 'E')) ex_leave(-1);
        else if (c == '\n' || c == '\r') ex_leave(ex_hov >= 0 ? ex_wi[ex_hov] : -1);
        return;
    }
    /* THE PANEL AND THE MENU OWN THE KEYBOARD while either is up, same rule as
     * Expose above and for the same reason: nothing on screen right now is an
     * app waiting for a keystroke. Escape backs out; everything else is
     * swallowed rather than forwarded, so a burst of typing aimed at a menu
     * that has since closed never lands in whatever app comes up next. */
    if (!g_locked && g_overlay != OV_NONE) {
        if (c == 27) overlay_close();
        return;
    }
    if (!g_locked && g_menu_open >= 0) {
        if (c == 27) menu_close();
        return;
    }
    if (!g_locked && (mods & EV_MOD_SUPER) && wm_shortcut(c, mods)) return;
    int wi = g_locked ? greeter_win()
                      : top_visible();  /* NOT order[norder-1]: that may be minimised */
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

/* LOCKED: the pointer belongs to the greeter and to nothing else.
 *
 * This is a whole separate path rather than a set of `if (!g_locked)` guards
 * scattered through wm_process_mouse, and that is on purpose: the guarded
 * version would have to be right in eight places (the theme switch, the
 * notification cards, the dock, the resize band, the titlebar, the three
 * traffic lights, the drag, the right-button menu), and a version that has to
 * be right in eight places is a version that will be wrong in one. Here the
 * only thing that can happen is a click reaching the greeter's canvas.
 *
 * The greeter is composited AT 0,0 FILLING THE SCREEN (see render_region), so
 * its window-local coordinates are the screen's -- w->x/w->y are not consulted,
 * because while locked they are not where the window is drawn. */
static void wm_locked_mouse(const struct inev *in)
{
    int x = in->x, y = in->y;
    int moved = (x != mx || y != my);
    mx = x; my = y;
    if (moved) perf_motions++;

    int gw = greeter_win();
    if (gw >= 0 && wins[gw].used && wins[gw].kind == WK_APP) {
        struct win *w = &wins[gw];
        if (in->l && !mleft) enqueue_input(w, EV_MOUSE,    PT(x), PT(y), in->mods, EV_BTN_LEFT, 0);
        if (!in->l && mleft) enqueue_input(w, EV_MOUSE_UP, PT(x), PT(y), in->mods, EV_BTN_LEFT, 0);
        if (moved)           enqueue_input(w, EV_MOUSE_MOVE, PT(x), PT(y), in->mods, EV_BTN_NONE, 0);
    }
    mleft = in->l; mright = in->r; mmiddle = in->m;
    set_cursor(CUR_ARROW);
    /* Without a cursor plane the arrow lives in the composite, so a moved
     * pointer is damage -- exactly as on the unlocked path. */
    if (moved && !hw_cursor) dirty_full();
}

/* EXPOSE: the pointer belongs to the picker and to nothing else.
 *
 * A whole separate path, for the identical reason wm_locked_mouse is one: the
 * guarded version would have to be right in eight places (the theme switch, the
 * notification cards, the dock, the resize band, the titlebar, the three
 * traffic lights, the drag, the right-button menu), and a version that has to
 * be right in eight places is a version that will be wrong in one. Here the
 * only two things that can happen are "pick a window" and "dismiss".
 *
 * NOTHING REACHES AN APP while this is up -- not a press, not a release, not a
 * motion. An app has no idea Expose exists and its window is not where the app
 * thinks it is, so a click delivered in window-local coordinates would land on
 * whatever is a quarter of the way across its canvas. */
static void wm_expose_mouse(const struct inev *in)
{
    int x = in->x, y = in->y;
    int moved = (x != mx || y != my);
    int omx = mx, omy = my;
    mx = x; my = y;
    if (moved) perf_motions++;

    if (moved) {
        int h = ex_hover_at(x, y);
        if (h != ex_hov) {                 /* same idiom as the dock's hover: */
            ex_dirty_slot(ex_hov);         /* damage the one being left...    */
            ex_hov = h;
            ex_dirty_slot(h);              /* ...and the one being entered    */
        }
        if (!hw_cursor) { dirty_cursor(omx, omy); dirty_cursor(x, y); }
    }
    if (in->l && !mleft) {
        int h = ex_hover_at(x, y);
        /* A window: bring it to the front and leave. Anywhere else -- the dimmed
         * wallpaper, the dock, the menu bar -- leave with the stacking exactly
         * as it was. Dismissing is the safe answer for every pixel that is not
         * a thumbnail, because the mode covers the whole screen and there is no
         * "somewhere else" for a click to usefully mean anything else. */
        ex_leave(h >= 0 ? ex_wi[h] : -1);
    }
    mleft = in->l; mright = in->r; mmiddle = in->m;
    set_cursor(CUR_ARROW);
}

/* THE ABOUT / SHUT DOWN / RESTART PANEL owns the pointer while it is up, same
 * capture idiom as Expose: a click is a button, or it is a dismissal, and
 * nothing under the panel ever sees it. */
static void wm_overlay_mouse(const struct inev *in)
{
    int x = in->x, y = in->y;
    int moved = (x != mx || y != my);
    int omx = mx, omy = my;
    mx = x; my = y;
    if (moved) perf_motions++;
    if (moved && !hw_cursor) { dirty_cursor(omx, omy); dirty_cursor(x, y); }

    if (moved && (g_overlay == OV_CONFIRM_SHUTDOWN || g_overlay == OV_CONFIRM_RESTART)) {
        int hb = overlay_button_at(x, y);
        if (hb != g_overlay_btn_hov) {
            g_overlay_btn_hov = hb;
            struct drect p; overlay_box(&p);
            dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
        }
    }
    if (in->l && !mleft) {
        if (g_overlay == OV_ABOUT) {
            overlay_close();                    /* an About box has nothing to click but "away" */
        } else {
            int hb = overlay_button_at(x, y);
            int shutdown = (g_overlay == OV_CONFIRM_SHUTDOWN);
            overlay_close();                    /* dirty + clear state BEFORE the noreturn call */
            if (hb == 1) { if (shutdown) kernel_poweroff(); else kernel_reboot(); }
            /* hb==0 (Cancel) or hb==-1 (click outside): already dismissed above. */
        }
    }
    mleft = in->l; mright = in->r; mmiddle = in->m;
    set_cursor(CUR_ARROW);
}

/* THE OPEN MENU owns the pointer, same capture idiom. A click is a title (close
 * the one that was open, or -- via the hover switch below -- land on the new
 * one that is already showing), an item, or "away", and nothing else can be
 * clicked while a menu covers it. */
static void wm_menu_mouse(const struct inev *in)
{
    int x = in->x, y = in->y;
    int moved = (x != mx || y != my);
    int omx = mx, omy = my;
    int was_open = g_menu_open;                /* the title open at the START of this event */
    mx = x; my = y;
    if (moved) perf_motions++;
    if (moved && !hw_cursor) { dirty_cursor(omx, omy); dirty_cursor(x, y); }

    /* Hovering a DIFFERENT title switches menus live -- the detail that makes
     * a menu bar read as one system instead of four separate buttons. */
    int hit_title = -1;
    for (int i = 0; i < NMENU; i++) {
        struct drect t; menu_title_hit(i, &t);
        if (in_rect(x, y, t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0)) { hit_title = i; break; }
    }
    if (hit_title >= 0 && hit_title != g_menu_open) {
        struct drect old; menu_dropdown_box(&old);
        g_menu_open = hit_title; g_menu_item_hov = -1;
        struct drect nu; menu_dropdown_box(&nu);
        dirty_rect(old.x0, old.y0, old.x1 - old.x0, old.y1 - old.y0);
        dirty_rect(nu.x0, nu.y0, nu.x1 - nu.x0, nu.y1 - nu.y0);
        menu_report_open();   /* hover switch */
    }

    struct drect row;
    int hov = menu_item_at(g_menu_open, x, y, &row);
    if (hov != g_menu_item_hov) {
        g_menu_item_hov = hov;
        struct drect p; menu_dropdown_box(&p);      /* whole panel -- it is small, see file header */
        dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
    }

    if (in->l && !mleft) {
        if (hit_title >= 0 && hit_title == was_open) {
            menu_close();                            /* the OPEN title, clicked again: toggle off */
        } else if (hit_title >= 0) {
            /* a different title: the hover switch above already did the work */
        } else {
            int idx = menu_item_at(g_menu_open, x, y, NULL);
            if (idx >= 0 && menu_item_enabled(g_menu_open, idx)) menu_item_fire(g_menu_open, idx);
            else menu_close();                       /* not a title, not a live item: dismiss */
        }
    }
    mleft = in->l; mright = in->r; mmiddle = in->m;
    set_cursor(CUR_ARROW);
}

static void wm_process_mouse(const struct inev *in)
{
    if (g_locked) { wm_locked_mouse(in); return; }
    if (ex_state()) { wm_expose_mouse(in); return; }
    if (g_overlay != OV_NONE) { wm_overlay_mouse(in); return; }
    if (g_menu_open >= 0) { wm_menu_mouse(in); return; }
    int x = in->x, y = in->y, left = in->l, right = in->r, middle = in->m;
    int mods = in->mods;
    int moved = (x != mx || y != my);
    int old_hov = dock_hover_at(mx, my);   /* before the pointer moves */
    int omx = mx, omy = my;                /* ...and where the arrow was */
    mx = x; my = y;
    if (moved) perf_motions++;
    /* "Parked" in the hot corner means PARKED. Restarting the dwell on every
     * motion sample is what keeps a pointer sweeping through the corner on its
     * way somewhere else from opening the picker behind it. */
    if (moved) ex_corner_t0 = 0;

    if (left && !mleft && in_rect(x, y, menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h)) {
        wm_set_dark(!g_ui_dark);             /* menu-bar dark-mode switch (on top of all) */
        mleft = left; mright = right; mmiddle = middle;
        return;
    }
    /* A CLOSED menu title, clicked: open it. (An OPEN one is handled above --
     * wm_menu_mouse captures every click once g_menu_open >= 0 -- so this only
     * ever fires the closed -> open transition.) Same idiom as the dark-mode
     * switch just above: the menu bar is chrome drawn on top, so it must win
     * the click before the dock or a window gets a chance to. */
    if (left && !mleft) {
        for (int i = 0; i < NMENU; i++) {
            struct drect t; menu_title_hit(i, &t);
            if (!in_rect(x, y, t.x0, t.y0, t.x1 - t.x0, t.y1 - t.y0)) continue;
            g_menu_open = i; g_menu_item_hov = -1;
            struct drect p; menu_dropdown_box(&p);
            dirty_rect(p.x0, p.y0, p.x1 - p.x0, p.y1 - p.y0);
            menu_report_open();
            mleft = left; mright = right; mmiddle = middle;
            return;
        }
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
            /* iy used to be the bare literal `+ 10` -- correct only because it
             * happened to equal the old S(10) top padding at 1x scale, and
             * silently wrong (a dead icon-height strip at the top of the hit
             * box, at scale > 100%) the day either drifted from the other.
             * S(DOCK_PAD_PT) is the same padding draw_dock() actually placed
             * the icon with, so a click and the pixel it lands on agree at
             * every backing scale, not just this one. */
            int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap), iy = dock_y0 + S(DOCK_PAD_PT);
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
        /* THE 64 BYTES, AND ONLY THE 64 BYTES.
         *
         * This used to kmalloc the whole file and read it whole, because
         * logitfs's vfs_read is all-or-nothing (a buffer smaller than the file
         * is an ERROR, not a short read) and there was no partial read in the
         * VFS at all. So the Dock scan -- which runs at boot, before anything
         * else has allocated -- took a contiguous kmalloc the size of the
         * largest .aex in the root, to look at its first 64 bytes. That is what
         * put "never put a big binary in the root" in CLAUDE.md.
         *
         * vfs_pread() exists now (c/fs/vfs.c), so the workaround is unnecessary
         * and so is the advice: 64 bytes on the stack, one read, no allocation
         * on the boot path at all. `sz` is still taken because the fixed header
         * carries no length and a file shorter than one is not a program. */
        int sz = vfs_size(nm);
        if (sz < AEX_HDR_SIZE) continue;    /* aex_info reads the 64-byte header */
        struct aex_header hb;
        if (vfs_pread(nm, &hb, AEX_HDR_SIZE, 0) != AEX_HDR_SIZE) continue;
        char name[32], ext[8];
        if (aex_info(&hb, name, ext) == 0) {
            struct aex_header *h = &hb;
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
            "torn=%lu defer=%lu late=%lu drawmax=%lu evdrop=%lu\n",
            (unsigned long)ms, (unsigned long)perf_composites,
            (unsigned long)perf_comp_ns, (unsigned long)perf_comp_ns_max,
            (unsigned long)perf_motions, (unsigned long)perf_cursor_moves,
            (unsigned long)perf_cursor_ns,
            (unsigned long)perf_full, (unsigned long)perf_rects,
            (unsigned long)perf_cpx, (unsigned long)fb_present_px(),
            (unsigned long)perf_present_ns,
            (unsigned long)perf_torn, (unsigned long)perf_defer,
            (unsigned long)perf_late, (unsigned long)perf_drawmax,
            /* EVENTS LOST TO A FULL RING, and evq.h already says this is "the
             * number that has to stay 0 -- a dropped click is a click the user
             * made and the machine did not act on". It was counted and
             * reachable only through the sysinfo string, which no serial log
             * carries -- so on the one occasion it was wanted (six Ctrl+L
             * chords aimed at a heavy page, none of which reached the browser)
             * the record could not say whether the keys had been dropped or
             * never delivered. A counter you cannot read back is a comment. */
            (unsigned long)evq_dropped());
}

/* The desktop proper. Called at boot on a machine with no accounts, and on the
 * unlock otherwise -- once, ever, which is what g_desktop_started is for: the
 * session can be observed to have changed on many consecutive passes of the
 * loop below and the Finder must be launched on exactly one of them. */
static void wm_desktop_start(void)
{
    if (g_desktop_started) return;
    g_desktop_started = 1;
    wm_launch("files.aex", "");
}

/* Has anybody logged in? Asked of c/fs/vfs_cred.c, which is the same fact the
 * filesystem enforces against -- deliberately not of the greeter, so there is
 * no second notion of "logged in" here to drift out of step with the first.
 * A console login therefore unlocks the screen too; see the g_locked comment. */
/* HOW LONG THE UNLOCK WAITS AFTER IT IS OBSERVED, and why it waits at all.
 *
 * The session changes INSIDE /bin/login's (or the greeter's) SYS_SETSESSION,
 * and the very next thing that program does is print its own success line. The
 * WM is a separate thread on a separate core, so without this delay it printed
 * "[wm] UNLOCKED..." into the middle of that line -- the kernel's kprintf and a
 * ring-3 write() share the serial port with no lock between them, which
 * c/apps/coreutils/login.c already documents. The observed damage was exact and
 * not cosmetic: `LOGIN-OK alice uid=1000 gid=1000` came out as
 * `LOGIN-OK [wm] UNLOCKED by session uid=1000 gid=1000`, so the login line's
 * own harness -- green before this, and not mine to edit -- stopped finding the
 * string it asserts on.
 *
 * 200 ms is not a guess at a race. It is three orders of magnitude more than
 * the handful of syscalls between setsession returning and that line being
 * finished (kbench measures a syscall at ~13 us under TCG), and by the time it
 * elapses the authenticating program is inside execve. The desktop appearing
 * a fifth of a second after a login that took a second to verify a password is
 * not a cost anybody can perceive. */
#define WM_UNLOCK_DELAY_TICKS 20        /* the PIT is 100 Hz */

static void wm_check_unlock(void)
{
    if (!g_locked) return;
    uint32_t su = 0, sg = 0;
    vfs_cred_session(&su, &sg);
    if (su == 0) return;

    static uint64_t unlock_at;
    if (!unlock_at) { unlock_at = timer_ticks() + WM_UNLOCK_DELAY_TICKS; return; }
    if (timer_ticks() < unlock_at) return;

    g_locked = 0;
    kprintf("[wm] UNLOCKED by session uid=%u gid=%u\n", su, sg);

    /* THE SETTINGS STORE IS A DIFFERENT FILE NOW. SYS_SETSESSION switched it to
     * $HOME/.config/settings.conf layered over /etc/settings.conf (see
     * c/kernel/core/settings.h), so the theme and the wallpaper this desktop
     * comes up in are the ones THIS user chose -- not the ones the greeter was
     * drawn in, which could only ever be the system defaults. Re-reading them
     * here is what makes "a user's dark mode survives a reboot" visible on the
     * first frame instead of on the next toggle. */
    g_ui_dark = settings_get_int("ui.dark", 0) ? 1 : 0;
    draw_background();
    { int count = W * H; blit(bg, back, count); }

    wm_desktop_start();
    dirty_full();
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

    /* THE BOOT ORDER, WHICH IS THE WHOLE POINT OF THIS BLOCK.
     *
     * One window at boot: the Finder -- but only once there is somebody to
     * open it for. The comment that used to sit here explained why the Finder
     * and not the clock; what it did not say, because it was not true until
     * /bin/login landed, is that launching it HERE means the previous user's
     * home directory is on screen 2.7 seconds after power-on with nothing
     * having asked anybody anything.
     *
     * So: if this machine has accounts, it comes up LOCKED and the only thing
     * that runs is the greeter. If it has none it behaves exactly as it always
     * has, because a machine with nobody to authenticate has nothing to ask --
     * the same rule /bin/login follows on the console, and the reason the other
     * sixty harnesses in this tree still see a desktop and a shell.
     *
     * The store is probed with vfs_size() rather than by asking /bin/login,
     * because this runs before any process exists. It is the same file
     * c/apps/coreutils/accounts.h defines and the kernel is still root here, so
     * the probe cannot be refused. */
    g_locked = (vfs_size("/etc/passwd") > 0);
    if (g_locked) {
        kprintf("[wm] LOCKED: /etc/passwd exists, so the desktop waits for a login\n");
        if (vfs_size(GREETER_AEX) <= 0)
            kprintf("[wm] and %s IS MISSING -- the screen will stay locked until\n"
                    "[wm] somebody authenticates on the serial console. That is the\n"
                    "[wm] safe direction to fail in, and it is not a good one.\n",
                    GREETER_AEX);
        wm_launch(GREETER_AEX, "");
    } else {
        kprintf("[wm] no accounts on this machine: the desktop starts unauthenticated\n");
        wm_desktop_start();
    }

    /* init: launch LOGIN on the serial console (stdin/stdout/stderr = tty).
     *
     * This used to be /bin/sh directly, which is how a machine reaches a root
     * shell 2.5 seconds after power-on with nothing in between. /bin/login is
     * the thing in between: it authenticates against /etc/passwd, calls
     * SYS_SETSESSION (which drops root and tells the rest of the machine who
     * is here -- see c/fs/vfs_cred.h), and execve's the same /bin/sh in the
     * same process with the same fd 0/1/2. The shell is therefore never root
     * and never was.
     *
     * ON A MACHINE WITH NO ACCOUNTS -- which is every freshly built image,
     * because no credential is ever shipped -- login prints one line saying so
     * and execs /bin/sh as root, i.e. exactly what this line did before. That
     * is what keeps the other sixty boot harnesses in this tree working: they
     * still see the shell banner, one exec later. */
    { char *login_argv[] = { "login", 0 }; proc_spawn("/bin/login", login_argv); }

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
        wm_check_unlock();            /* did somebody authenticate? (greeter OR console) */
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
        /* Animations advance HERE, before the frame that shows them, and they
         * are what asks for that frame -- exactly the shape of the dock bounce
         * this loop already ran (draw_dock re-dirties itself while a bounce is
         * live). Nothing polls; an idle desktop with nothing moving reaches
         * this line, requests no damage, and goes back to sleep on the hlt. */
        wm_hotcorner_tick();
        wm_anim_tick();
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
