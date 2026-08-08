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
#include "rtc.h"
#include "aex.h"
#include "blkdev.h"
#include "img.h"
#include "logit_abi.h"
/* Generated from include/abi/logit_calls.abi, which is where the packed syscall
 * arguments are described. Unpacking them by hand here meant the convention was
 * stated once in a logit_abi.h comment, once in the caller's packing, and once
 * here -- three copies of `(x<<16)|y` that nothing checked against each other.
 * Writing it down found a real disagreement: SYS_GUI_GLASS's radius is 8 bits,
 * which the comment's `(radius<<32)|...` never said. */
#include "logit_pack.h"
#include "ktime.h"
#include "evq.h"
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
    int  cw_pt, ch_pt;        /* content size in POINTS -- what the app asked for */
    char cwd[128];            /* Finder: current directory path */
    uint64_t open_t0;         /* tick the open "pop" animation began (0 = settled) */
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

/* System light/dark theme. The kernel-drawn chrome (menu bar, dock, window
 * frames) reads this directly; ring-3 apps query it via SYS_UI_DARK and follow. */
static int g_ui_dark;
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
        if (exist->win >= 0) raise_win(exist->win);
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
        for (int i = 1; i <= stk_pages; i++) {
            uint64_t frame = pmm_alloc();
            if (!frame) { entry = 0; break; }    /* OOM: fail the launch, don't run on a partial stack */
            vmm_map_page(ustack_top - (uint64_t)i * 0x1000, frame, VMM_WRITABLE | VMM_USER);
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

static void launch_for_ext(const char *ext, const char *file)
{
    for (int i = 0; i < nreg; i++)
        if (reg[i].ext[0] && streq(reg[i].ext, ext)) { wm_launch(reg[i].file, file); return; }
    /* Images open in the Preview viewer (the kernel decodes PNG/GIF). */
    if (streq(ext, "png") || streq(ext, "gif")) { wm_launch("preview.aex", file); return; }
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
        for (uint64_t i = 0; i < pxcount; i++) w->surf.px[i] = rgb(250, 250, 252);
        evq_reset(&w->ev); w->wants_close = 0;
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
                wins[wi].used = 0;
                remove_win(wi);
                if (dragging == wi) dragging = -1;   /* don't drag a reaped (soon reused) slot */
                if (mouse_capture == wi) mouse_capture = -1;   /* ...nor deliver its drag to the slot's next tenant */
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
    int sz = vfs_size("/wallpaper.png");
    if (sz <= 0) return 0;
    uint8_t *file = (uint8_t *)kmalloc((unsigned)sz);
    if (!file) return 0;
    int n = vfs_read("/wallpaper.png", file, sz);
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

/* Without a cursor plane the arrow is pixels in the frame, so moving it damages
 * where it was and where it now is -- two small rectangles instead of a screen.
 * This is the LFB fallback path; on virtio-gpu the pointer is not in the frame
 * at all and neither of these is ever called. */
static void dirty_cursor(int x, int y)
{ dirty_rect(x, y, S(CURSOR_W) + 1, S(CURSOR_H) + 1); }

/* The cursor is composited into `back` on top of everything else, then presented
 * with the rest of the frame -- no save-under overlay (that restored a stale
 * `back` and smeared garbage as the cursor moved). */
/* The bitmap is one CELL per character, not one pixel: at scale 200 a cell is a
 * 2x2 block. A 16-pixel arrow on a 2560-wide display is a mote -- the pointer is
 * the one piece of chrome a user tracks continuously, and leaving it at device
 * resolution is the single most obvious way a "scaled" desktop announces that it
 * is only half scaled. Cell edges come from S() of the cell index, so the blocks
 * tile exactly with no seams and no overlap at fractional scales. */
static void draw_cursor_back(int x, int y)
{
    uint32_t o = rgb(20, 20, 26), f = rgb(255, 255, 255);
    int rows = (int)(sizeof cursor_bmp / sizeof cursor_bmp[0]);
    for (int r = 0; r < rows; r++) {
        int y0 = S(r), y1 = S(r + 1);
        for (int c = 0; cursor_bmp[r][c]; c++) {
            char p = cursor_bmp[r][c];
            if (p != '#' && p != '.') continue;
            uint32_t col = (p == '#') ? o : f;
            int x0 = S(c), x1 = S(c + 1);
            for (int j = y0; j < y1; j++)
                for (int i = x0; i < x1; i++) fb_put(x + i, y + j, col);
        }
    }
}

/* ---- the pointer as a display plane ---------------------------------------
 *
 * Set once, at init, when the display has a cursor plane. Everything else in
 * this file branches on it: wm_render stops drawing the arrow, and plain motion
 * stops setting `dirty`. When it is 0 the file behaves exactly as it did --
 * that is the fallback for a multiboot LFB, and it is why the removal of the
 * old save-under path is not being undone here. There is still no partial
 * rendering and still no save-under; the pointer simply is not in the frame. */
static int hw_cursor;

/* The same arrow as draw_cursor_back, encoded as a 64x64 ARGB plane image.
 * Deliberately reads the SAME cursor_bmp with the SAME S() cell scaling, so the
 * hardware pointer and the software one are the same picture -- a plane cursor
 * that is a different shape from the composited one turns every screenshot
 * comparison into an argument.
 *
 * The plane is 64x64 and a cell is S(1) px, so the arrow fits up to scale 300
 * (17 rows x 3 = 51). fb_cursor_image crops beyond that rather than corrupting
 * anything, and pick_scale() never goes past 300. */
#define CUR_PLANE 64
static uint32_t cursor_plane[CUR_PLANE * CUR_PLANE];
static int build_cursor_plane(void)
{
    uint32_t o = 0xFF000000u | rgb(20, 20, 26), f = 0xFF000000u | rgb(255, 255, 255);
    for (int i = 0; i < CUR_PLANE * CUR_PLANE; i++) cursor_plane[i] = 0;
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
                for (int i = x0; i < x1; i++) cursor_plane[j * CUR_PLANE + i] = col;
        }
    }
    /* The arrow's tip is the top-left cell, so the hotspot is (0,0) -- the same
     * relationship draw_cursor_back(mx,my) had, which is what keeps hit-testing
     * and the pointer position identical across the two paths. */
    return fb_cursor_image(cursor_plane, CUR_PLANE, CUR_PLANE, 0, 0) == 0;
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
                if (!w->used) continue;
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
    for (int i = 0; i < norder; i++) {     /* windows, back-to-front */
        struct win *w = &wins[order[i]];
        if (!w->used) continue;
        struct drect b; win_box(w, &b);
        if (!rect_hit(&b, R)) continue;    /* nothing of this window is in the rect */
        int focused = (i == norder - 1);
        int s = win_open_scale(w);         /* open pop in progress? */
        if (s) {
            int need = w->w * w->h;
            if (need > anim_buf_n) { if (anim_buf) kfree(anim_buf); anim_buf = kmalloc((unsigned)need * 4); anim_buf_n = anim_buf ? need : 0; }
            if (anim_buf) {
                animating = 1;
                struct surface tmp = { .px = anim_buf, .w = w->w, .h = w->h };  /* clip_on = 0: scratch is unclipped */
                fb_target(&tmp);           /* render the whole window into scratch... */
                draw_frame_body(0, 0, w->w, w->h, w->title, focused);
                if (w->surf.px) fb_blit_surface(0, TBH, &w->surf);
                fb_target(NULL);
                int dw = w->w * s / 256, dh = w->h * s / 256;     /* ...scale to back */
                fb_blit_surface_scaled(w->x + (w->w - dw) / 2, w->y + (w->h - dh) / 2, dw, dh, &tmp);
                continue;
            }
        }
        draw_frame(w, focused);
        if (w->surf.px)
            fb_blit_surface(w->x, w->y + TBH, &w->surf);
    }
    { struct drect p; menubar_box(&p);                  /* frosted chrome ON TOP: */
      if (rect_hit(&p, R)) draw_menubar(); }            /* real-time vibrancy      */
    { struct drect p; dock_box(&p);
      if (rect_hit(&p, R)) draw_dock(); }
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
    for (int k = 0; k < nr; k++) animating |= render_region(&r[k]);
    if (animating) dirty_full();           /* keep compositing until the pop settles */

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

static void wm_process_key(int c, int mods)
{
    if (norder == 0) return;
    struct win *w = &wins[order[norder - 1]];
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
        if (!w->used || w->kind != WK_APP) continue;
        if (in_rect(x, y, w->x, w->y + TBH, w->w, w->h - TBH)) return order[i];
    }
    return -1;
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
        if (!docked) {
        int hitorder = -1;
        for (int i = norder - 1; i >= 0; i--) {
            struct win *w = &wins[order[i]];
            if (w->used && in_rect(x, y, w->x, w->y, w->w, w->h)) { hitorder = i; break; }
        }
        if (hitorder >= 0) {
            int wi = order[hitorder];
            struct win *w = &wins[wi];
            /* Raising repaints BOTH windows, and forgetting the second one is
             * the classic partial-render bug: the window that just lost focus
             * still shows its coloured traffic lights, in its own titlebar,
             * nowhere near the rectangle that was damaged. */
            int prev_top = norder ? order[norder - 1] : -1;
            raise_win(wi);
            dirty_win(w);
            if (prev_top >= 0 && prev_top != wi && wins[prev_top].used)
                dirty_win(&wins[prev_top]);
            int cx = x - w->x, cy = y - w->y;
            if (cy < TBH) {
                if ((cx - S(16)) * (cx - S(16)) + (cy - S(15)) * (cy - S(15)) <= S(8) * S(8)) {
                    if (w->kind == WK_APP) enqueue(w, EV_CLOSE, 0, 0);  /* close button */
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

    if (!left) dragging = -1;
    /* A drag changes TWO regions: where the window was and where it now is.
     * Reporting only the destination is what leaves a window-shaped hole
     * trailing behind the drag, and it is the single most visible way to get
     * damage tracking wrong. */
    if (dragging >= 0 && left) {
        struct win *w = &wins[dragging];
        dirty_win(w);
        w->x = x - drag_dx; w->y = y - drag_dy;
        dirty_win(w);
    }

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
    if (moved && dragging < 0) {
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
    hw_cursor = build_cursor_plane();
    if (hw_cursor) fb_cursor_move(mx, my);
    kprintf("[wm] pointer: %s\n",
            hw_cursor ? "display cursor plane (motion does not composite)"
                      : "composited into the frame (no cursor plane)");

    scan_apps();

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
    static uint64_t next_ms, last_comp, last_mot;
    uint64_t ms = time_mono_ms();
    if (ms < next_ms) return;
    next_ms = ms + 1000;
    uint64_t dc = perf_composites - last_comp, dm = perf_motions - last_mot;
    last_comp = perf_composites; last_mot = perf_motions;
    if (dm == 0 && dc <= 20) return;                 /* idle: the clock strip only */
    kprintf("[wm] perf t=%lu composites=%lu ns=%lu max=%lu motions=%lu curmoves=%lu "
            "curns=%lu full=%lu rects=%lu cpx=%lu fpx=%lu presns=%lu\n",
            (unsigned long)ms, (unsigned long)perf_composites,
            (unsigned long)perf_comp_ns, (unsigned long)perf_comp_ns_max,
            (unsigned long)perf_motions, (unsigned long)perf_cursor_moves,
            (unsigned long)perf_cursor_ns,
            (unsigned long)perf_full, (unsigned long)perf_rects,
            (unsigned long)perf_cpx, (unsigned long)fb_present_px(),
            (unsigned long)perf_present_ns);
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

    /* auto-launch the clock so something is alive on screen at boot */
    wm_launch("files.aex", "");      /* the Finder (unified file manager) -- desktop's always-open browser */
    wm_launch("clock.aex", "");

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
        wm_pointer_sync();            /* one cursor-plane command per loop, not per packet */
        proc_reap();                  /* free zombie processes (GUI apps + orphans) */
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
