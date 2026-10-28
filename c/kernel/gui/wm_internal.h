/* wm_internal.h -- what the window manager's files share.
 *
 * wm.c was one 8335-line file holding every chrome painter, input path and
 * tick. It is now wm.c -- the compositor, the damage machinery, input
 * dispatch, the GUI syscalls, launching and the main loop -- plus one file
 * per surface (wm_menubar.c, wm_dock.c, ...), each with a header holding
 * what the others call. Every file includes this one and nothing else of
 * the WM's: it carries the shared includes, units, types and wm.c's shared
 * state, then includes every surface's header.
 *
 * NAMES. A file-scope name that had to leave its file kept its name when it
 * already carried the WM's own prefixes (wm_, g_, ex_, dock_, menu_, sw_,
 * win_, ...) and took a wm_ prefix where it was generic: wm_W, wm_H,
 * wm_wins, wm_order, wm_norder, wm_desktop_prefs, wm_enqueue,
 * wm_frame_glass_ns/px. The pointer (mx, my, the buttons) stays private to
 * wm.c behind wm_pointer_*().
 *
 * LOCKING is unchanged by the split: everything shared here is WM state,
 * owned by wm_lock (gui_sync.h) and read by the compositor under the
 * graphics lock, exactly as it was when it was one file's statics. */
#ifndef LOGIT_WM_INTERNAL_H
#define LOGIT_WM_INTERNAL_H

#include "openlogit_draw.h"
#include "openlogit_bitmap.h"
#include <stdint.h>
#include <stddef.h>
#include "wm.h"
#include "gui_sync.h"
#include "fb.h"
#include "text.h"
#include "icons.h"
#include "logit_desktop_prefs.h"
#include "logit_apps.h"   /* launch-argument formats (wm_launch_app) */
#include "logit_tokens.h" /* the design's token tables (wm_tok, wm_theme.c) */
#include "logit_app_icon.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "pit.h"
#include "serial.h"
#include "sched.h"
#include "mmguard.h"
#include "vfs.h"
#include "settings.h"   /* settings line: theme + wallpaper are persisted */
#include "rtc.h"
#include "aex.h"
#include "elf.h"        /* struct elf_image: wm_launch reports what the loader did */
#include "pcache.h"     /* the app's own file, so two instances share their text */
#include "blkdev.h"
#include "img.h"
#include "syscall.h" /* kernel_img_decode: all kernel codec users share ownership */
#include "gfx.h"        /* Open Logit: build_arrow's fill+stroke, below */
#include "openlogit_anim.h"
/* The byte identifier /bin/show, the Terminal's output guard and Preview all
 * already share. Pure inline, no libc, no allocation -- see its header. */
#include "logit_sniff.h"
#include "logit_abi.h"
#include "prot.h"       /* cpu_prot_nx_usable() + PTE_NX for the app stack below */
#include "vma.h"        /* vma_reserve_fixed(): a GUI app's stack is a reservation */
/* Generated from include/abi/logit_calls.abi, which is where the packed syscall
 * arguments are described. Unpacking them by hand here meant the convention was
 * stated once in a logit_abi.h comment, once in the caller's packing, and once
 * here -- three copies of `(x<<16)|y` that nothing checked against each other.
 * Writing it down found a real disagreement: SYS_GUI_GLASS's radius is 8 bits,
 * which the comment's `(radius<<32)|...` never said. */
#include "logit_pack.h"
#include "wm_draw2.h"    /* GUI ABI v2: router, handlers, struct drect */
#include "wm_cover.h"    /* wm_cover_ok: the cover predicate, pure */
#include "wm_shadow.h"   /* shadow presets + extent rule, pure */
#include "openlogit_display_shadow.h"  /* ol_display_shadow_ex: key + ambient layers */
#include "openlogit_display_group.h"  /* rounded content: opacity-255 corner groups */
#include "openlogit_display_blit.h"   /* wallpaper: BOX-filtered bake */
#include "ktime.h"
#include "evq.h"
#include "input_queue.h"
#include "notify.h"     /* WM-HOOK 1/6: the notification overlay (see notify.h) */
#include "ime_ui.h"     /* IME-HOOK 1/4: the pinyin candidate bar (see ime_ui.h) */
#include "keyboard.h"
#include "menu_blob.h"  /* SYS_GUI_MENU_SET: an app's own menus ("Fluid" P5) */
#include "wm_bands.h"   /* the band splitter: see par_render */
#include "gfx_ctx.h"    /* GFX_NCTX: how many band workers there can be */
#include "smp.h"        /* smp_kick_others, smp_cpu_count */
#include "spinlock.h"
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
#include "kernel/sync/wait.h"   /* SYS_WAIT_EVENT: an idle app sleeps */
#include "power.h"      /* kernel_poweroff/kernel_reboot -- the LogitOS menu's Shut Down/Restart */

#define MAXWIN     16

/* ---- units ----------------------------------------------------------------
 * Every geometry constant in the window manager is in POINTS; `struct win`'s
 * x/y/w/h and everything that touches a pixel are in DEVICE pixels. S()
 * converts one to the other and is the boundary between the two worlds.
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
 * every enqueue_input() in wm.c routes its window-local coordinates through PT().
 */
#define S(v)       fb_pt(v)          /* points -> device pixels */
#define PT(v)      fb_dev2pt(v)      /* device pixels -> points */
#define MENUBAR_H  24                /* points */
#define TITLEBAR_H 30                /* points */
#define MBH        S(MENUBAR_H)
#define TBH        S(TITLEBAR_H)
#define FW         LOGIT_FONT_W
#define FH         LOGIT_FONT_H

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

void *memcpy(void *, const void *, size_t);

/* The render pipeline (DOM/CSS/layout/paint) and <style>/<script> collection now
 * live in the ring-3 browser app; the kernel only provides fetch + draw + font
 * primitives. See net/{dom,css,layout}.c (compiled into browser.aex) and L1 plan. */

/* ---------- windows + apps ---------- */
/* struct drect (half-open, DEVICE pixels) is defined in wm_draw2.h, included
 * above, so wm_backdrop.c and wm_cover.h share this one definition. It has to
 * be complete before `struct win`, which stores one (see anim_prev). The damage
 * machinery that owns it is still the only thing here that manipulates it. */

/* WK_BUILDING is the state a slot is in between the moment SYS_GUI_CREATE
 * CLAIMS it and the moment it is finished. It exists because the claim has
 * to publish `used = 1` under win_lock -- two apps creating a window at once
 * otherwise take the same slot -- and `used` alone is what several scans
 * filter on. wm_apply_sizes() (the ONLY reallocator of a window surface)
 * walks wins[] by `used && kind == WK_APP`, and a reused slot still carries
 * the PREVIOUS tenant's kind, so a half-built window passed that filter and
 * win_apply_size() ran against surf.px == NULL.
 *
 * MEASURED, because it is the opposite of what it looks like: publishing
 * surf.px = NULL at the claim is the SAFER-looking move and it is what broke
 * the desktop -- with the old garbage pointer every `if (px)` check passed
 * and the read was silently wrong pixels; with NULL the same code faulted and
 * took the WM thread with it, so no window appeared at all. Bisected: the
 * peel was innocent, `used = 1` alone was innocent, zeroing the pointer was
 * not. The pointer SHOULD be NULL; what was missing is a state that keeps
 * the scanners away until the window is whole.
 *
 * All sixteen kind comparisons in the WM are `== WK_APP`, `== WK_FINDER`
 * or `!= WK_APP`; none is `!= WK_FINDER`, so a third value is excluded by
 * every one of them without touching any of them. Checked, not assumed. */
enum wkind { WK_FINDER, WK_APP, WK_BUILDING };

struct app {
    int  used, alive, id;
    char name[32];
    char arg[64];
    uint64_t base;
    int  win;                 /* window index, -1 until the app creates one */
    /* The program's FILE identity, "files" for /files.aex: what `[wm] win N
     * app` prints after each frame line. `name` is the header's display name
     * and is not unique to a file; the stem is. Appended, not inserted. */
    char stem[32];
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
    unsigned wait_refs; /* a parked syscall pins this slot until it returns */
    int  wants_close;
    int  focus_notified;     /* last EV_WINDOW_FOCUS value; -1 before first delivery */
    int  want_keyup;         /* WINS_WANT_KEYUP: deliver EV_KEY_UP + modifier keys */
    int  panel;              /* WINS_SET_PANEL: no titlebar, hides when it loses focus */
    struct mb_set *menus;     /* SYS_GUI_MENU_SET: the app's menus, NULL if it published none */
    unsigned menus_gen;       /* unique per publish (g_menus_gen), so the menu bar knows to rebuild */
    int  cw_pt, ch_pt;        /* content size in POINTS -- what the app CURRENTLY has */
    char cwd[128];            /* Finder: current directory path */
    uint64_t open_t0;         /* monotonic ms when the open pop began (0 = settled) */
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
    int  snapped;             /* SNAP_LEFT / SNAP_RIGHT half, 0 if not (see win_snap) */
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
    uint64_t min_t0;          /* monotonic ms when the dock fly began (0 = settled) */
    int  min_dir;             /* 1 = flying to the dock, 0 = flying back out */
    int  min_slot;            /* dock icon index the flight is aimed at */
    /* The on-screen box this window occupied on the previous animated frame.
     * An animation's damage is (where it was | where it now is), and the first
     * half of that is not derivable from the current tick -- so it is kept.
     * Seeded when an animation STARTS; a zeroed one would union in the origin
     * and quietly turn every animated frame into a top-left-anchored repaint. */
    struct drect anim_prev;
    struct drect anim_from;   /* previous box used by the most recent damage call */
    struct drect anim_sent;   /* exact union submitted by that call */
    int  sx, sy, sw, sh;      /* the frame to restore to; only valid while zoomed */
    /* Pixels the canvas ALLOCATION holds, which is >= surf.w * surf.h. The
     * canvas is reshaped inside this block whenever the new size fits, so a
     * drag does not hand the kernel heap a differently-sized multi-megabyte
     * request eight times a second -- see win_apply_size. */
    int  surf_cap;
    /* Is this canvas half drawn RIGHT NOW? 1 from the app's first drawing
     * syscall after a flush until the next flush -- see the mid-frame guard
     * note near the top of wm.c. There is no ABI call that says "I am
     * starting a frame" and there does not need to be: the first draw after a
     * flush IS the start of one, which is true of every app in the tree and of
     * any app that has not been written yet. */
    int  drawing;
    uint64_t draw_t0;         /* time_mono_ms() when this frame's drawing began */
    /* WINS_SET_TITLEBAR (titlebar mode A, design P10): the app's header colour
     * and TB_* flags, kept for the chrome to paint the unified header with.
     * tb_set is 0 until the app sets one (the WM's own titlebar look). */
    unsigned tb_rgb, tb_flags;
    int  tb_set;
};

/* WHERE A COMPOSITE GOES: the pixels and their row pitch in pixels. Every
 * frame's target is the screen, { back, W } (wm_screen_target); the one other
 * caller, wm_render_full_into, names a buffer of its own. The layers drawn
 * through fb_* reach the target because fb's backbuffer IS it (wm_init's
 * fb_set_backbuffer(back)); the raw writes in wm.c -- the wallpaper rows, a
 * fade, the composited arrow -- are the ones that take it from here. */
struct wm_target { uint32_t *px; int stride; };

/* WHY A FULL FRAME HAPPENED, retained until that frame is consumed.  The
 * compositor's old `full` counter answered only how many; it could not tell a
 * necessary theme flip from an accidental escalation, so the expensive tail
 * was unactionable.  Bits are used rather than one last-writer string because
 * several requests can coalesce before the WM gets the graphics mutex, and
 * losing the earlier request would make the trace confidently wrong.
 *
 * The initial bit is seeded beside dirty_all (wm.c).  FULL_EMPTY is not passed
 * by a caller: wm_render_locked records it if `dirty` was somehow armed with
 * neither dirty_all nor a rectangle, the apparatus failure dirty_rect's empty
 * rectangle guard is specifically written to prevent. */
enum full_reason {
    FULL_INITIAL           = 1u << 0,
    FULL_DAMAGE_THRESHOLD  = 1u << 1,
    FULL_EXTERNAL_OVERFLOW = 1u << 2,
    FULL_EXPOSE_ENTER      = 1u << 3,
    FULL_EXPOSE_LEAVE      = 1u << 4,
    FULL_FOCUS_EXISTING    = 1u << 5,
    FULL_WINDOW_CREATE     = 1u << 6,
    FULL_FLUSH_NO_WINDOW   = 1u << 7,
    FULL_THEME             = 1u << 8,
    FULL_EXPOSE_TERMINAL   = 1u << 9,
    FULL_ZORDER_SYSTEM     = 1u << 10,
    FULL_ZORDER_APP        = 1u << 11,
    FULL_LOCKED_CURSOR     = 1u << 12,
    FULL_UNLOCK            = 1u << 13,
    FULL_EMPTY             = 1u << 14,
    FULL_APP_EXIT          = 1u << 15,
    FULL_TICK              = 1u << 16,   /* a wm_<m>_tick asked for a frame without damage */
};
#define WSH_EXT(f)  wm_sh_ext((f) ? WMSH_WIN_FOCUSED : WMSH_WIN_UNFOCUSED)

/* These radii are used by both the painter and damage's sample reach. A
 * separately guessed damage radius would leave an invisible stale-glass ring
 * when UI scale changes. Values are logical points, converted at each call. */
enum {
    MENU_GLASS_PT = 2,
    DOCK_GLASS_PT = 28,
    DROPDOWN_GLASS_PT = 10,
    OVERLAY_GLASS_PT = 14,
    TITLE_GLASS_PT = 10,
};
/* The kinds of chrome glass (wm_glass.c: wm_glass_panel). */
enum wm_mat {
    WM_MAT_MENUBAR, WM_MAT_MENU, WM_MAT_DOCK, WM_MAT_OVERLAY, WM_MAT_SWITCHER, WM_MAT_TITLE,
    WM_MAT_COUNT
};
#define WM_GLASS_ALPHA_KIND (-1)   /* wm_glass_panel: the kind's own tint alpha */
/* One chrome glass panel on screen this frame (wm_glass_regions). */
struct wm_glass_region {
    struct drect r;            /* the panel box, device px */
    int radius;                /* its corner radius, device px */
};
#define WM_GLASS_REGIONS_MAX (5 + MAXWIN)

/* ---- the helpers every file uses (pure, header-only) ------------------------ */

/* A window's titlebar height: TBH, or 0 for a panel (WINS_SET_PANEL), whose
 * canvas starts at its top edge. Every per-window use of the titlebar height
 * goes through this, so a panel is placed, hit-tested and composited without
 * a strip nobody draws in. */
static inline int win_tbh(const struct win *w) { return w->panel ? 0 : TBH; }

static inline int rect_hit(const struct drect *a, const struct drect *b)
{ return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1; }
static inline int rect_in(const struct drect *inner, const struct drect *outer)
{ return inner->x0 >= outer->x0 && inner->y0 >= outer->y0 &&
         inner->x1 <= outer->x1 && inner->y1 <= outer->y1; }
static inline void rect_or(struct drect *a, const struct drect *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}
static inline long rect_area(const struct drect *r)
{ return (long)(r->x1 - r->x0) * (long)(r->y1 - r->y0); }

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return fb_rgb(r, g, b); }
static inline int lerp(int a, int b, int n, int d) { return a + (b - a) * n / d; }
static inline void blit(uint32_t *d, const uint32_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

static inline int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static inline void scopy(char *d, const char *s, int max) { int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static inline int ends_aex(const char *s) {
    int n = 0; while (s[n]) n++;
    return n >= 4 && s[n-4]=='.' && s[n-3]=='a' && s[n-2]=='e' && s[n-1]=='x';
}
static inline const char *ext_of(const char *s) {
    const char *dot = 0;
    for (const char *p = s; *p; p++) if (*p == '.') dot = p;
    return dot ? dot + 1 : "";
}

static inline int in_rect(int px, int py, int x, int y, int w, int h)
{ return px >= x && px < x + w && py >= y && py < y + h; }

/* ---- wm.c: the state and services the surfaces share ---------------------- */

/* The screen, in device pixels. */
extern int wm_W, wm_H;
/* The system theme (1 = dark; wm_dark() is the atomic read for other files),
 * the locked screen, and the two accessibility settings. */
extern int g_ui_dark;
extern int g_locked;
extern int wm_reduce_motion;
/* The ui.* desktop preferences (logit_desktop_prefs.h), refreshed on the prefs tick. */
extern struct desktop_preferences wm_desktop_prefs;

/* The window list: the slots, and the z-order (wm_order[wm_norder - 1] is on
 * top). raise_win() is the only way onto the top; top_visible() is the
 * topmost window that is not minimised. */
extern struct win wm_wins[MAXWIN];
extern int wm_order[MAXWIN], wm_norder;
void raise_win(int wi);
int top_visible(void);
struct app *find_live_app(const char *name);
/* Queue an event for a window's app (and wake it). */
void wm_enqueue(struct win *w, int type, int a, int b);
/* Flip the theme; (cx, cy) is where it was asked for, or (-1, -1). */
void wm_set_dark(int on, int cx, int cy);
/* Launch an app by its stem ("settings") with an argument in logit_apps.h's
 * formats; wm_launch_pane opens Settings on a pane (enum logit_pane). */
void wm_launch_app(const char *stem, const char *arg);
void wm_launch_pane(int pane);
/* Reallocate a window's canvas to its frame now and send EV_RESIZE (no-op
 * when they already agree). Caller holds graphics_lock and wm_lock. */
void win_apply_size(struct win *w);

/* The pointer, read-only (the state is wm.c's). */
int wm_pointer_x(void);
int wm_pointer_y(void);
int wm_pointer_buttons(void);      /* any button held */
int wm_pointer_owned(void);        /* a drag, a resize grab or an app's capture */

/* DAMAGE: report the TRUE extent of what changed, in device pixels (see
 * dirty_rect in wm.c). dirty_full asks for a whole frame, with its reason. */
void dirty_rect(int x, int y, int w, int h);
void dirty_full(enum full_reason why);
/* A window's footprint (frame + focused shadow) and its damage. */
void win_box(const struct win *w, struct drect *r);
void dirty_win(const struct win *w);
/* An animation's (where it was | where it is) damage, and the dock fly's
 * landing sweep; both stay in wm.c beside WM_ANIM_DAMAGE_LIE. */
void anim_damage(struct win *w);
void wm_anim_sweep_damage(const struct drect *s);
/* The frame's old box, before win_set_frame moves it (WM_RESIZE_DAMAGE_LIE). */
void wm_resize_damage_old(const struct win *w);

/* Where a window is drawn this frame (0: not on screen), and whether that is
 * the ordinary way -- at its frame, full size, opaque, not in Expose. */
int win_draw_rect(const struct win *w, int *ox, int *oy, int *ow, int *oh, int *oa);
int win_drawn_direct(const struct win *w, int x, int y, int ww, int wh, int a);

/* Shadows: a preset's extent in device px, and the two-layer cast. */
int wm_sh_ext(int kind);
void wm_cast_shadow(int kind, int x, int y, int w, int h, int rad, int scale256, int alpha);
/* A canvas scaled into (dx, dy, dw, dh) of t at a constant alpha, clipped. */
void anim_blit_fade(const struct wm_target *t, const struct drect *clip, int dx, int dy,
                    int dw, int dh, const struct surface *src, int alpha);

/* ---- each surface's interface ------------------------------------------------ */
#include "wm_wallpaper.h"
#include "wm_glass.h"
#include "wm_menu.h"
#include "wm_menubar.h"
#include "wm_dock.h"
#include "wm_overlay.h"
#include "wm_chrome.h"
#include "wm_place.h"
#include "wm_anim.h"
#include "wm_expose.h"
#include "wm_panel.h"
#include "wm_reveal.h"
#include "wm_theme.h"

#endif /* LOGIT_WM_INTERNAL_H */
