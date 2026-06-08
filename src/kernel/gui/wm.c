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
#include "img.h"
#include "aether_abi.h"
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
#define MENUBAR_H  24
#define TITLEBAR_H 30
#define FW         AETHER_FONT_W
#define FH         AETHER_FONT_H
#define USER_PATH_MAX 128
#define USER_URL_MAX  384
#define USER_TEXT_MAX 1024
#define EVQ_N         256        /* per-window event ring: deep enough that a burst of
                                  * keystrokes isn't dropped while the app repaints */

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
    struct aether_event evq[EVQ_N];
    int  evhead, evtail;
    int  wants_close;
    char cwd[128];            /* Finder: current directory path */
    uint64_t open_t0;         /* tick the open "pop" animation began (0 = settled) */
};

static struct app apps[MAXWIN];
static struct win wins[MAXWIN];
static int order[MAXWIN], norder;      /* z-order; order[norder-1] is on top */

static int mx, my, mleft, mright;
static int dragging = -1, drag_dx, drag_dy;
static volatile int dirty = 1;           /* the next frame needs a full recomposite */

static uint32_t *back, *bg;
static int W, H;

/* Mark the screen for a full recomposite. Dirty-rectangle tracking was removed:
 * with the virtio-gpu DMA present a full frame is cheap, so every frame
 * composites the whole screen + cursor into `back` and presents it once. The old
 * partial-render + cursor save-under path left stale regions in `back` that the
 * cursor restore smeared into on-screen garbage. */
static void dirty_full(void) { dirty = 1; }
static void dirty_rect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; dirty = 1; }
static int next_app_id = 1;

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

    int bytes = ((sz + 511) / 512) * 512;
    void *img = kmalloc((unsigned)bytes);
    if (!img) return;
    if (vfs_read(aex_file, img, bytes) <= 0) { kfree(img); return; }

    char name[32], ext[8];
    if (aex_info(img, name, ext) != 0) { serial_puts("[wm] launch: bad aex\n"); kfree(img); return; }

    struct app *exist = find_live_app(name);
    if (exist) {                            /* single instance: just focus it */
        if (exist->win >= 0) raise_win(exist->win);
        dirty_full();
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
    if (!space) { serial_puts("[wm] launch: no address space\n"); return; }

    /* wm_launch may run from the WM thread (kernel CR3) OR from the mouse IRQ
     * while a ring-3 app is current (that app's CR3). Save and restore the CR3
     * that was actually active, not the kernel's -- otherwise returning from the
     * IRQ into the interrupted app would run it with the wrong address space. */
    uint64_t prev_cr3;
    __asm__ volatile ("cli");
    __asm__ volatile ("mov %%cr3, %0" : "=r"(prev_cr3));
    vmm_switch(space);
    uint64_t entry = aex_load(img, (uint64_t)bytes, name, ext);   /* maps + copies into `space` */
    uint64_t ustack_top = 0;
    if (entry) {
        /* The stack must sit ABOVE the whole app image. browser/js link a 24 MiB
         * mini-libc arena in BSS plus several large CSS/page buffers (image
         * reaches ~entry+30 MiB), and a stack landing *inside* that BSS would
         * corrupt the allocator. Top at 40 MiB clears the image; the stack is
         * 4 MiB because QuickJS recurses deeply throwing errors on real pages'
         * scripts (github overran a 256 KiB stack inside JS_ThrowError2). */
        ustack_top = entry + 0x2800000;          /* 40 MiB above the link base */
        /* The browser links QuickJS, which recurses deeply on real pages' scripts
         * (and deeper still when THROWING a stack-overflow error in JS_ThrowError2);
         * 4 MiB wasn't enough headroom for the throw to complete, so it overran the
         * stack into a page fault. Give it 8 MiB. (Stack bottom 0x47000000 still
         * clears the ~30 MiB browser image.) Other apps keep 4 MiB. */
        int stk_pages = streq(name, "browser.aex") ? 2048 : 1024;
        for (int i = 1; i <= stk_pages; i++) {
            uint64_t frame = pmm_alloc();
            if (!frame) { entry = 0; break; }    /* OOM: fail the launch, don't run on a partial stack */
            vmm_map_page(ustack_top - (uint64_t)i * 0x1000, frame, VMM_WRITABLE | VMM_USER);
        }
    }
    vmm_switch(prev_cr3);
    __asm__ volatile ("sti");
    if (!entry) { serial_puts("[wm] launch: load failed\n"); vmm_free_space(space); kfree(img); return; }

    int ai = -1;
    for (int i = 0; i < MAXWIN; i++) if (!apps[i].used) { ai = i; break; }
    if (ai < 0) { kfree(img); return; }
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
    if (!p) { ap->used = ap->alive = 0; vmm_free_space(space); kfree(img); return; }
    /* Give every app real stdio (fd 0/1/2 = the serial console). Apps that only
     * draw never touch them, but it means pipe()/dup2() in an app (e.g. the
     * Terminal spawning a shell) get fds >= 3 and don't collide with 0/1/2. */
    { struct file *tty = file_open_tty();
      if (tty) { p->fd[0] = tty; file_dup(tty); p->fd[1] = tty; file_dup(tty); p->fd[2] = tty; } }
    p->tid = thread_create_user(ap->name, entry, ustack_top, p, space);
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
    p = ap_str(p, end, "Uptime  "); p = ap_num(p, end, timer_ticks() / 100); p = ap_str(p, end, " s\n");
    p = ap_str(p, end, "Memory  "); p = ap_num(p, end, (pmm_total_bytes() - pmm_free_bytes()) >> 20);
    p = ap_str(p, end, " / "); p = ap_num(p, end, pmm_total_bytes() >> 20); p = ap_str(p, end, " MB used\n");
    p = ap_str(p, end, "Switches "); p = ap_num(p, end, sched_switches()); p = ap_str(p, end, "\n\n");
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
    if (!ap) return -1;

    switch (num) {
    case SYS_GUI_CREATE: {
        if (ap->win >= 0) return 0;
        char title[64];
        if (user_copy_string(title, sizeof title, (const char *)a) < 0) return -1;
        int wi = -1;
        for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { wi = i; break; }
        if (wi < 0) return -1;
        int cw = (int)((b >> 16) & 0xFFFF), ch = (int)(b & 0xFFFF);
        if (cw <= 0 || ch <= 0) return -1;
        /* cw,ch are each up to 65535 -- cw*ch*4 overflows int. A window larger
         * than the screen is meaningless, so cap to the framebuffer; do the size
         * math in 64-bit. */
        if (cw > (int)fb_width() || ch > (int)fb_height()) return -1;
        uint64_t pxcount = (uint64_t)cw * (uint64_t)ch;
        struct win *w = &wins[wi];
        w->used = 1; w->kind = WK_APP; w->app = ap;
        w->w = cw; w->h = TITLEBAR_H + ch;
        w->x = 110 + cascade * 28; w->y = 70 + cascade * 28;
        cascade = (cascade + 1) % 6;
        { int SW = (int)fb_width(), SH = (int)fb_height();   /* keep big windows on-screen */
          if (w->x + w->w > SW) w->x = SW - w->w; if (w->x < 0) w->x = 0;
          if (w->y + w->h > SH - 4) w->y = SH - 4 - w->h; if (w->y < 24) w->y = 24; }
        scopy(w->title, title, sizeof w->title);
        w->surf.w = cw; w->surf.h = ch;
        w->surf.px = kmalloc((size_t)(pxcount * 4));
        if (!w->surf.px) { w->used = 0; return -1; }
        for (uint64_t i = 0; i < pxcount; i++) w->surf.px[i] = rgb(250, 250, 252);
        w->evhead = w->evtail = 0; w->wants_close = 0;
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
        int x = (int)((a >> 16) & 0xFFFF), y = (int)(a & 0xFFFF);
        int rw = (int)((b >> 16) & 0xFFFF), rh = (int)(b & 0xFFFF);
        fb_target(&w->surf); fb_fill_rect(x, y, rw, rh, (uint32_t)c); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_TEXT: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = (int)((a >> 16) & 0xFFFF), y = (int)(a & 0xFFFF);
        char text[USER_TEXT_MAX];
        if (user_copy_string(text, sizeof text, (const char *)c) < 0) return -1;
        fb_target(&w->surf); fb_text(x, y, text, (uint32_t)b); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_TEXT_MONO: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = (int)((a >> 16) & 0xFFFF), y = (int)(a & 0xFFFF);
        int cell = (int)((b >> 24) & 0xFF); uint32_t color = (uint32_t)(b & 0xFFFFFF);
        char text[USER_TEXT_MAX];
        if (user_copy_string(text, sizeof text, (const char *)c) < 0) return -1;
        fb_target(&w->surf); text_draw_mono(x, y, text, cell, color); fb_target(NULL);
        return 0;
    }
    case SYS_GUI_FLUSH: {
        struct win *w = app_window(ap);          /* repaint just this app's window */
        if (w) dirty_rect(w->x, w->y, w->w, w->h); else dirty_full();
        return 0;
    }
    case SYS_POLL_EVENT: {
        struct win *w = app_window(ap); if (!w) return 0;
        struct aether_event *ev = (struct aether_event *)a;
        if (!user_range_ok(ev, sizeof *ev, 1)) return -1;
        if (w->evhead == w->evtail) return 0;
        *ev = w->evq[w->evhead];
        w->evhead = (w->evhead + 1) % EVQ_N;
        return 1;
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
        uint64_t t0 = timer_ticks();
        int grc = http_get(url);
        for (int retry = 0; grc < 0 && retry < 3; retry++)   /* transient DNS/conn/TLS loss */
            grc = http_get(url);
        g_net_busy = 0;
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
        char tmp[USER_TEXT_MAX];
        if (len > 0) { if (!user_range_ok(s, (uint64_t)len, 0)) return -1; memcpy(tmp, s, (size_t)len); }
        return text_measure(tmp, len, px, mono);
    }
    case SYS_GUI_TEXT_RUN: {
        struct win *w = app_window(ap); if (!w) return -1;
        struct aether_run r;
        if (!user_range_ok((const void *)a, sizeof r, 0)) return -1;
        memcpy(&r, (const void *)a, sizeof r);
        int len = r.len; if (len < 0) len = 0; if (len > USER_TEXT_MAX - 1) len = USER_TEXT_MAX - 1;
        char tmp[USER_TEXT_MAX];
        if (len > 0) { if (!user_range_ok(r.s, (uint64_t)len, 0)) return -1; memcpy(tmp, r.s, (size_t)len); }
        tmp[len] = 0;
        fb_target(&w->surf);
        text_draw_run(r.x, r.y, tmp, len, r.px, r.mono, r.color);
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
        uint8_t *rb = 0; int rl = 0; int rc = res_fetch(src, &rb, &rl);
        g_net_busy = 0;
        __asm__ volatile ("cli");
        if (rc != 0 || !rb) return -1;
        int n = rl < max ? rl : max;
        memcpy(buf, rb, (size_t)n);
        kfree(rb);
        return n;
    }
    case SYS_GUI_BLIT: {
        struct win *w = app_window(ap); if (!w) return -1;
        struct aether_blit bl;
        if (!user_range_ok((const void *)a, sizeof bl, 0)) return -1;
        memcpy(&bl, (const void *)a, sizeof bl);
        if (bl.sw <= 0 || bl.sh <= 0 || bl.sw > 4096 || bl.sh > 4096) return -1;
        if (!user_range_ok(bl.rgba, (uint64_t)bl.sw * (uint64_t)bl.sh * 4, 0)) return -1;
        fb_target(&w->surf);
        fb_blit_rgba(bl.x, bl.y, bl.w, bl.h, bl.rgba, bl.sw, bl.sh);
        fb_target(NULL);
        return 0;
    }
    case SYS_GUI_CLIP: {
        struct win *w = app_window(ap); if (!w) return -1;
        int x = (int)((a >> 16) & 0xFFFF), y = (int)(a & 0xFFFF);
        int cw2 = (int)((b >> 16) & 0xFFFF), ch2 = (int)(b & 0xFFFF);
        if (cw2 == 0 && ch2 == 0) fb_clear_clip();
        else fb_set_clip(x, y, cw2, ch2);
        return 0;
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

static void enqueue(struct win *w, int type, int a, int b)
{
    int nt = (w->evtail + 1) % EVQ_N;
    if (nt == w->evhead) return;       /* full, drop */
    w->evq[w->evtail].type = type;
    w->evq[w->evtail].a = a;
    w->evq[w->evtail].b = b;
    w->evtail = nt;
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
    dirty = 1;
}

/* ---------- reaping dead apps ---------- */
static void reap(void)
{
    for (int i = 0; i < MAXWIN; i++) {
        if (apps[i].used && !apps[i].alive) {
            int wi = apps[i].win;
            if (wi >= 0 && wins[wi].used) {
                if (wins[wi].surf.px) kfree(wins[wi].surf.px);
                wins[wi].used = 0;
                remove_win(wi);
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
struct inev { int type; int x, y, l, r; };   /* type 0 = mouse (x,y,l,r); 1 = key (x = code) */
#define INQ_N 512
static struct inev inq[INQ_N];
static volatile int inq_head, inq_tail;
static void inq_push(int type, int x, int y, int l, int r)   /* IRQ-safe: no locks, no shared-state */
{
    int nt = (inq_tail + 1) % INQ_N;
    if (nt == inq_head) return;            /* full: drop (cosmetic under flooding) */
    inq[inq_tail].type = type; inq[inq_tail].x = x; inq[inq_tail].y = y;
    inq[inq_tail].l = l; inq[inq_tail].r = r;
    inq_tail = nt;
}
static void wm_process_mouse(int x, int y, int left, int right);   /* fwd: bodies below */
static void wm_process_key(int c);
/* The IRQ entry points (called from mouse.c / keyboard.c) now ONLY enqueue. */
void wm_mouse_event(int x, int y, int left, int right) { inq_push(0, x, y, left, right); }
void wm_key(int c)                                      { inq_push(1, c, 0, 0, 0); }
static void wm_drain_input(void)           /* WM thread: process all input here, NOT in the IRQ */
{
    for (;;) {
        struct inev e;
        __asm__ volatile ("cli");          /* brief: atomic dequeue vs the producing IRQs */
        int empty = (inq_head == inq_tail);
        if (!empty) { e = inq[inq_head]; inq_head = (inq_head + 1) % INQ_N; }
        __asm__ volatile ("sti");
        if (empty) break;
        if (e.type == 0) wm_process_mouse(e.x, e.y, e.l, e.r);
        else             wm_process_key(e.x);
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
static int menu_tog_x, menu_tog_y, menu_tog_w = 38, menu_tog_h = 18;   /* dark-mode switch */
static void draw_menubar(void)
{
    fb_blur_rect(0, 0, W, MENUBAR_H, 6, 0);                  /* frost live backdrop */
    if (g_ui_dark) fb_blend_rect(0, 0, W, MENUBAR_H, 28, 28, 34, 165);   /* dark glass */
    else           fb_blend_rect(0, 0, W, MENUBAR_H, 255, 255, 255, 120); /* light glass */
    fb_blend_rect(0, MENUBAR_H - 1, W, 1, 0, 0, 0, g_ui_dark ? 70 : 28);  /* hairline */
    uint32_t ink = g_ui_dark ? rgb(232, 233, 238) : rgb(40, 40, 48);
    fb_fill_circle(16, MENUBAR_H / 2, 6, ink);
    fb_text(32, 4, "Aether OS", ink);
    fb_text(112, 4, "File", ink);
    fb_text(156, 4, "Edit", ink);
    fb_text(200, 4, "View", ink);
    /* dark-mode toggle switch: track + knob (knob right = dark) */
    menu_tog_x = W - 210; menu_tog_y = (MENUBAR_H - menu_tog_h) / 2;
    if (g_ui_dark) fb_round_rect(menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h, menu_tog_h / 2, rgb(94, 150, 255));
    else           fb_round_rect(menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h, menu_tog_h / 2, rgb(198, 200, 208));
    int kr = menu_tog_h / 2 - 2;
    int kx = g_ui_dark ? menu_tog_x + menu_tog_w - kr - 3 : menu_tog_x + kr + 3;
    fb_fill_circle(kx, menu_tog_y + menu_tog_h / 2, kr, rgb(255, 255, 255));
    draw_clock();
}

static int dock_x0, dock_y0, dock_isz = 50, dock_gap = 14;

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
    else { char ch[2] = { reg[i].icon, 0 }; fb_text(cx - FW / 2, cy - FH / 2, ch, rgb(255, 255, 255)); }
}

/* Upward pixel offset of dock icon `i`'s launch bounce (two decaying parabolic
 * hops over ~0.55s). Clears the timer when finished. Integer-only. */
static int dock_bounce_off(int i)
{
    uint64_t t0 = reg_bounce[i];
    if (!t0) return 0;
    uint64_t e = timer_ticks() - t0, DUR = 55, half = DUR / 2;
    if (e >= DUR) { reg_bounce[i] = 0; return 0; }
    int hop = (e < half) ? 0 : 1, H = hop ? 7 : 14;
    int u = (int)(e - (uint64_t)hop * half), d = (int)half;   /* 0..d within the arc */
    return H * 4 * u * (d - u) / (d * d);                      /* parabola, peak mid-arc */
}

static void draw_dock(void)
{
    int n = nreg < 1 ? 1 : nreg;
    int dw = dock_gap + n * (dock_isz + dock_gap), dh = dock_isz + 20;
    dock_x0 = (W - dw) / 2; dock_y0 = H - dh - 12;
    fb_blend_round_rect(dock_x0 - 1, dock_y0 + 7, dw + 2, dh, 28, 0, 0, 0, 50);    /* soft drop shadow */
    fb_blur_rect(dock_x0, dock_y0, dw, dh, 6, 28);                                  /* frost the live backdrop */
    if (g_ui_dark) fb_blend_round_rect(dock_x0, dock_y0, dw, dh, 28, 36, 36, 44, 120);   /* dark glass */
    else           fb_blend_round_rect(dock_x0, dock_y0, dw, dh, 28, 255, 255, 255, 58); /* light glass */
    fb_blend_rect(dock_x0 + 16, dock_y0 + 1, dw - 32, 1, 255, 255, 255, g_ui_dark ? 60 : 115); /* top sheen */

    /* Live hover magnification: the icon under the cursor grows in place (kept
     * inside the panel + gap so it never overlaps a neighbour) and shows its name
     * as a tooltip. Re-evaluated every frame, and the WM repaints on each mouse
     * move, so it animates as the cursor sweeps the dock. */
    int ccy = dock_y0 + 10 + dock_isz / 2, hov = -1, animating = 0;
    if (my >= dock_y0 && my < dock_y0 + dh)
        for (int i = 0; i < nreg; i++) {
            int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap);
            if (mx >= ix && mx < ix + dock_isz) { hov = i; break; }
        }
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
        int tw = fb_text_width(nm), tx = ccx - tw / 2;
        fb_blend_round_rect(tx - 9, dock_y0 - 28, tw + 18, 23, 7, 28, 28, 34, 225);
        fb_text(tx, dock_y0 - 25, nm, rgb(244, 244, 248));
    }
    if (animating) dirty = 1;                              /* keep compositing while a bounce runs */
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
    fb_text(W - fb_text_width(b) - 12, 4, b, g_ui_dark ? rgb(228, 229, 235) : rgb(40, 40, 46));
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
    fb_round_rect(x, y, ww, wh, 10, g_ui_dark ? rgb(30, 30, 36) : rgb(250, 250, 252));
    uint32_t tbtop, tbbot;
    if (g_ui_dark) { tbtop = focused ? rgb(60, 60, 70) : rgb(40, 40, 48); tbbot = focused ? rgb(44, 44, 52) : rgb(34, 34, 40); }
    else           { tbtop = focused ? rgb(246, 246, 250) : rgb(250, 250, 252); tbbot = focused ? rgb(226, 227, 234) : rgb(240, 240, 244); }
    fb_round_rect_vgrad(x, y, ww, TITLEBAR_H, 10, tbtop, tbbot);
    fb_fill_rect(x, y + TITLEBAR_H, ww, 1, g_ui_dark ? rgb(60, 60, 70) : rgb(214, 214, 220));
    uint32_t off = g_ui_dark ? rgb(80, 80, 90) : rgb(205, 205, 210);
    fb_fill_circle(x + 16, y + 15, 6, focused ? rgb(255, 95, 86) : off);  /* close */
    fb_fill_circle(x + 34, y + 15, 6, focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + 52, y + 15, 6, focused ? rgb(40, 200, 64) : off);
    int tw = fb_text_width(title);
    fb_text(x + (ww - tw) / 2, y + 7, title, g_ui_dark ? rgb(210, 211, 218) : rgb(70, 70, 78));
}

static void draw_frame(struct win *w, int focused)
{
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    shadow_band(x, y, ww, wh, 8, focused ? 11 : 7);        /* faint outer fringe */
    shadow_band(x, y, ww, wh, 4, focused ? 22 : 13);       /* mid */
    shadow_band(x, y, ww, wh, 2, focused ? 40 : 24);       /* dark edge */
    draw_frame_body(x, y, ww, wh, w->title, focused);
}

static const char *cursor_bmp[] = {
    "#","##","#.#","#..#","#...#","#....#","#.....#","#......#","#.......#",
    "#........#","#.....####","#..#..#","#.# #..#","##  #..#","#    #..#","     #..#","      ##",
};
#define CURSOR_W 16
#define CURSOR_H 20

/* The cursor is composited into `back` on top of everything else, then presented
 * with the rest of the frame -- no save-under overlay (that restored a stale
 * `back` and smeared garbage as the cursor moved). */
static void draw_cursor_back(int x, int y)
{
    uint32_t o = rgb(20, 20, 26), f = rgb(255, 255, 255);
    int rows = (int)(sizeof cursor_bmp / sizeof cursor_bmp[0]);
    for (int r = 0; r < rows; r++)
        for (int c = 0; cursor_bmp[r][c]; c++) {
            char p = cursor_bmp[r][c];
            if (p == '#') fb_put(x + c, y + r, o);
            else if (p == '.') fb_put(x + c, y + r, f);
        }
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

/* Composite the whole screen -- background + menu-bar clock + every window + the
 * cursor -- into `back`, then present it in one shot. No dirty-rect / partial
 * path: the virtio-gpu present is a cheap DMA, and a single full composite each
 * frame keeps `back` always-valid. */
void wm_render(void)
{
    int animating = 0;
    reap();
    fb_target(NULL);
    for (int y = 0; y < H; y++)            /* wallpaper (baked in bg) */
        blit(back + y * W, bg + y * W, W);
    for (int i = 0; i < norder; i++) {     /* windows, back-to-front */
        struct win *w = &wins[order[i]];
        if (!w->used) continue;
        int focused = (i == norder - 1);
        int s = win_open_scale(w);         /* open pop in progress? */
        if (s) {
            int need = w->w * w->h;
            if (need > anim_buf_n) { if (anim_buf) kfree(anim_buf); anim_buf = kmalloc((unsigned)need * 4); anim_buf_n = anim_buf ? need : 0; }
            if (anim_buf) {
                animating = 1;
                struct surface tmp = { anim_buf, w->w, w->h };
                fb_target(&tmp);           /* render the whole window into scratch... */
                draw_frame_body(0, 0, w->w, w->h, w->title, focused);
                if (w->surf.px) fb_blit_surface(0, TITLEBAR_H, &w->surf);
                fb_target(NULL);
                int dw = w->w * s / 256, dh = w->h * s / 256;     /* ...scale to back */
                fb_blit_surface_scaled(w->x + (w->w - dw) / 2, w->y + (w->h - dh) / 2, dw, dh, &tmp);
                continue;
            }
        }
        draw_frame(w, focused);
        if (w->surf.px)
            fb_blit_surface(w->x, w->y + TITLEBAR_H, &w->surf);
    }
    draw_menubar();                        /* frosted chrome ON TOP: real-time */
    draw_dock();                           /* vibrancy frosts the live windows  */
    draw_cursor_back(mx, my);              /* cursor on top, into the composite */
    fb_present();                          /* full back -> framebuffer + virtio flush */
    if (animating) dirty = 1;              /* keep compositing until the pop settles */
}

/* ---------- input ---------- */
static int in_rect(int px, int py, int x, int y, int w, int h)
{ return px >= x && px < x + w && py >= y && py < y + h; }

static void wm_process_key(int c)
{
    if (norder == 0) return;
    struct win *w = &wins[order[norder - 1]];
    if (w->kind == WK_APP) { enqueue(w, EV_KEY, (int)c, 0); dirty_rect(w->x, w->y, w->w, w->h); }
}

static void wm_process_mouse(int x, int y, int left, int right)
{
    int moved = (x != mx || y != my);
    int content = 0;                       /* did anything other than the cursor change? */
    mx = x; my = y;

    if (left && !mleft && in_rect(x, y, menu_tog_x, menu_tog_y, menu_tog_w, menu_tog_h)) {
        wm_set_dark(!g_ui_dark);             /* menu-bar dark-mode switch (on top of all) */
        mleft = left; mright = right; dirty = 1;
        return;
    }
    if (left && !mleft) {
        content = 1;
        int hitorder = -1;
        for (int i = norder - 1; i >= 0; i--) {
            struct win *w = &wins[order[i]];
            if (w->used && in_rect(x, y, w->x, w->y, w->w, w->h)) { hitorder = i; break; }
        }
        if (hitorder >= 0) {
            int wi = order[hitorder];
            struct win *w = &wins[wi];
            raise_win(wi);
            int cx = x - w->x, cy = y - w->y;
            if (cy < TITLEBAR_H) {
                if ((cx - 16) * (cx - 16) + (cy - 15) * (cy - 15) <= 64) {
                    if (w->kind == WK_APP) enqueue(w, EV_CLOSE, 0, 0);  /* close button */
                } else {
                    dragging = wi; drag_dx = cx; drag_dy = cy;
                }
            } else if (w->kind == WK_APP) {
                enqueue(w, EV_MOUSE, cx, cy - TITLEBAR_H);
            }
        } else {
            /* dock? */
            for (int i = 0; i < nreg; i++) {
                int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap), iy = dock_y0 + 10;
                if (in_rect(x, y, ix, iy, dock_isz, dock_isz)) { wm_launch(reg[i].file, ""); break; }
            }
        }
    }
    if (right && !mright) {
        for (int i = norder - 1; i >= 0; i--) {
            struct win *w = &wins[order[i]];
            if (!w->used || !in_rect(x, y, w->x, w->y, w->w, w->h)) continue;
            int cx = x - w->x, cy = y - w->y;
            if (cy >= TITLEBAR_H && w->kind == WK_APP) {
                raise_win(order[i]);    /* focus + bring to front (like left-click) so the menu shows on top */
                enqueue(w, EV_MOUSE_R, cx, cy - TITLEBAR_H);
                content = 1;
            }
            break;
        }
    }
    if (!left) dragging = -1;
    if (dragging >= 0 && left) { wins[dragging].x = x - drag_dx; wins[dragging].y = y - drag_dy; content = 1; }
    mleft = left;
    mright = right;
    /* Any change -- content (click/drag) or pure cursor motion -- recomposites the
     * whole screen next frame; the cursor is part of the composite now. */
    if (content || moved) dirty = 1;
}

/* ---------- registry + init ---------- */
static void scan_apps(void)
{
    int n = vfs_count("/");
    for (int i = 0; i < n && nreg < MAXWIN; i++) {
        char nm[64];
        scopy(nm, vfs_ent_name("/", i), sizeof nm);
        if (!ends_aex(nm)) continue;
        /* aetherfs reads whole-file (errors if the buffer is smaller), so size the
         * buffer to the file -- the JS app's .aex is ~1 MiB, far over any header
         * scratch. We only need the header, but read it all then free. */
        int sz = vfs_size(nm);
        if (sz <= 0) continue;
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
    fb_set_backbuffer(back);
    mx = W / 2; my = H / 2;

    scan_apps();

    /* The Finder is now the ring-3 file-manager app, launched in wm_run(). */
    draw_background();          /* wallpaper -> bg; menu bar + dock are per-frame now */
    blit(bg, back, count);
}

void wm_render_first(void) { wm_render(); }

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
        wm_drain_input();             /* process ALL keyboard/mouse input here, NOT in the IRQ */
        proc_reap();                  /* free zombie processes (GUI apps + orphans) */
        if (!g_net_busy) net_poll();  /* drive RX -- unless a blocking fetch owns the net */
        uint64_t now = timer_ticks();
        /* Composite on change: a full frame only when geometry changed; an app's
         * flush repaints just its window rect. Else ~2 Hz refresh of the menu-bar
         * clock (a small strip). Pure cursor motion is a cheap overlay. */
        if (dirty || now - last >= 50) {   /* on any change, else ~2 Hz for the menu clock */
            dirty = 0; last = now;
            wm_render();
        }
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
