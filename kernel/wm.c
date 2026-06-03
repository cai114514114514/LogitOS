#include <stdint.h>
#include <stddef.h>
#include "wm.h"
#include "fb.h"
#include "text.h"
#include "pmm.h"
#include "vmm.h"
#include "kheap.h"
#include "pit.h"
#include "serial.h"
#include "sched.h"
#include "vfs.h"
#include "rtc.h"
#include "aex.h"
#include "aqua_abi.h"
#include "net.h"
#include "icmp.h"
#include "dns.h"
#include "http.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "paint.h"
#include "img.h"
#include "kprintf.h"
#include "usercopy.h"

#define MAXWIN     8
#define MENUBAR_H  24
#define TITLEBAR_H 30
#define FW         AQUA_FONT_W
#define FH         AQUA_FONT_H
#define USER_PATH_MAX 128
#define USER_URL_MAX  384
#define USER_TEXT_MAX 1024

void *memcpy(void *, const void *, size_t);

/* The browser page currently laid out (one at a time, kernel-side). The DOM
 * tree owns the strings the display list points into, so it must outlive paint;
 * freed (with the layout list) on the next load. */
static struct node *page_root;
static int page_built;

static void page_reset(void)
{
    layout_free();
    if (page_root) { dom_free(page_root); page_root = 0; }
    page_built = 0;
}

/* Concatenate the text of every <style> element into out (author stylesheet). */
static int tag_is(const char *t, const char *lit){ int i=0; for(;lit[i];i++) if(t[i]!=lit[i]) return 0; return t[i]==0; }
static int collect_style(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "style")) {
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_style(c, out, o, max);
    return o;
}

/* Concatenate the source of every inline <script> (skip external src=) into out,
 * separating scripts with a newline+';' so they evaluate independently. */
static int collect_scripts(struct node *n, char *out, int o, int max)
{
    if (!n) return o;
    if (n->type == N_ELEM && tag_is(n->tag, "script") && !dom_attr(n, "src")) {
        for (struct node *c = n->first_child; c; c = c->next)
            if (c->type == N_TEXT && c->text)
                for (int i = 0; i < c->textlen && o < max - 1; i++) out[o++] = c->text[i];
        if (o < max - 1) out[o++] = '\n';
        if (o < max - 1) out[o++] = ';';
    }
    for (struct node *c = n->first_child; c; c = c->next)
        o = collect_scripts(c, out, o, max);
    return o;
}

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
    struct aqua_event evq[16];
    int  evhead, evtail;
    int  wants_close;
    char cwd[128];            /* Finder: current directory path */
};

static struct app apps[MAXWIN];
static struct win wins[MAXWIN];
static int order[MAXWIN], norder;      /* z-order; order[norder-1] is on top */

static int mx, my, mleft;
static int dragging = -1, drag_dx, drag_dy;
static volatile int dirty = 1;

static uint32_t *back, *bg;
static int W, H;
static int next_app_id = 1;
static int cascade;

/* app registry built by scanning the disk for *.aex */
struct regent { char file[48], name[32], ext[8]; char icon; uint32_t color; };
static struct regent reg[MAXWIN];
static int nreg;

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return fb_rgb(r, g, b); }
static int lerp(int a, int b, int n, int d) { return a + (b - a) * n / d; }
static void blit(uint32_t *d, const uint32_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }

static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scopy(char *d, const char *s, int max) { int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
/* out = dir + "/" + name (collapsing the slash); truncates at max-1. */
static void path_join(char *out, const char *dir, const char *name, int max) {
    int n = 0;
    for (const char *p = dir; *p && n < max - 1; p++) out[n++] = *p;
    if ((n == 0 || out[n - 1] != '/') && n < max - 1) out[n++] = '/';
    for (const char *p = name; *p && n < max - 1; p++) out[n++] = *p;
    out[n] = 0;
}
/* strip the last path component in place ("/a/b" -> "/a", "/a" -> "/"). */
static void path_up(char *p) {
    int n = 0; while (p[n]) n++;
    if (n <= 1) return;
    if (p[n - 1] == '/') n--;
    while (n > 0 && p[n - 1] != '/') n--;
    if (n <= 1) { p[0] = '/'; p[1] = 0; } else p[n - 1] = 0;
}
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
    if (!img || vfs_read(aex_file, img, bytes) <= 0) return;

    char name[32], ext[8];
    if (aex_info(img, name, ext) != 0) { serial_puts("[wm] launch: bad aex\n"); kfree(img); return; }

    struct app *exist = find_live_app(name);
    if (exist) {                            /* single instance: just focus it */
        if (exist->win >= 0) raise_win(exist->win);
        dirty = 1;
        return;
    }

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
    uint64_t entry = aex_load(img, name, ext);   /* maps + copies into `space` */
    uint64_t ustack_top = 0;
    if (entry) {
        ustack_top = entry + 0x800000;           /* user stack high in the slot */
        for (int i = 1; i <= 4; i++)
            vmm_map_page(ustack_top - (uint64_t)i * 0x1000, pmm_alloc(), VMM_WRITABLE | VMM_USER);
    }
    vmm_switch(prev_cr3);
    __asm__ volatile ("sti");
    if (!entry) { serial_puts("[wm] launch: load failed\n"); kfree(img); return; }

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

    thread_create_user(ap->name, entry, ustack_top, ap, space);
    serial_puts("[wm] launched ");
    serial_puts(ap->name);
    serial_puts("\n");
}

static void launch_for_ext(const char *ext, const char *file)
{
    for (int i = 0; i < nreg; i++)
        if (reg[i].ext[0] && streq(reg[i].ext, ext)) { wm_launch(reg[i].file, file); return; }
    serial_puts("[wm] no app handles that file type\n");
}

/* ---------- system info text (for the Activity Monitor app) ---------- */
static char *ap_num(char *p, uint64_t v)
{
    char t[20]; int i = 0;
    if (!v) { *p++ = '0'; return p; }
    while (v) { t[i++] = '0' + v % 10; v /= 10; }
    while (i) *p++ = t[--i];
    return p;
}
static char *ap_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }

static int sysinfo_text(char *buf, int max)
{
    char *p = buf;
    p = ap_str(p, "Uptime  "); p = ap_num(p, timer_ticks() / 100); p = ap_str(p, " s\n");
    p = ap_str(p, "Memory  "); p = ap_num(p, (pmm_total_bytes() - pmm_free_bytes()) >> 20);
    p = ap_str(p, " / "); p = ap_num(p, pmm_total_bytes() >> 20); p = ap_str(p, " MB used\n");
    p = ap_str(p, "Switches "); p = ap_num(p, sched_switches()); p = ap_str(p, "\n\n");
    p = ap_str(p, "PID  NAME\n");
    p = ap_str(p, "  0  wm (compositor)\n");
    for (int i = 0; i < MAXWIN; i++)
        if (apps[i].used && apps[i].alive) {
            p = ap_str(p, "  "); p = ap_num(p, apps[i].id); p = ap_str(p, "  ");
            p = ap_str(p, apps[i].name); p = ap_str(p, "\n");
        }
    *p = 0;
    (void)max;
    return (int)(p - buf);
}

/* ---------- GUI syscalls (called from syscall.c in the app's context) ---------- */
static struct win *app_window(struct app *ap)
{
    return (ap && ap->win >= 0) ? &wins[ap->win] : NULL;
}

long wm_gui_syscall(long num, long a, long b, long c)
{
    struct app *ap = sched_current_data();
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
        struct win *w = &wins[wi];
        w->used = 1; w->kind = WK_APP; w->app = ap;
        w->w = cw; w->h = TITLEBAR_H + ch;
        w->x = 110 + cascade * 28; w->y = 70 + cascade * 28;
        cascade = (cascade + 1) % 6;
        scopy(w->title, title, sizeof w->title);
        w->surf.w = cw; w->surf.h = ch;
        w->surf.px = kmalloc((unsigned)(cw * ch * 4));
        if (!w->surf.px) { w->used = 0; return -1; }
        for (int i = 0; i < cw * ch; i++) w->surf.px[i] = rgb(250, 250, 252);
        w->evhead = w->evtail = 0; w->wants_close = 0;
        ap->win = wi;
        raise_win(wi);
        dirty = 1;
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
    case SYS_GUI_FLUSH:
        dirty = 1;
        return 0;
    case SYS_POLL_EVENT: {
        struct win *w = app_window(ap); if (!w) return 0;
        struct aqua_event *ev = (struct aqua_event *)a;
        if (!user_range_ok(ev, sizeof *ev, 1)) return -1;
        if (w->evhead == w->evtail) return 0;
        *ev = w->evq[w->evhead];
        w->evhead = (w->evhead + 1) % 16;
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
    case SYS_NET_INFO: {
        if (!net_up()) return 0;
        struct aqua_netinfo *ni = (struct aqua_netinfo *)a;
        if (!user_range_ok(ni, sizeof *ni, 1)) return -1;
        ni->ip = net_cfg.ip; ni->mask = net_cfg.mask; ni->gw = net_cfg.gw;
        for (int i = 0; i < 6; i++) ni->mac[i] = net_cfg.mac[i];
        return 1;
    }
    case SYS_NET_PING:
        return net_up() ? icmp_ping((uint32_t)a) : -1;
    case SYS_NET_PING_RTT: {
        int t = icmp_last_rtt();
        return t < 0 ? -1 : t * 10;        /* ticks (10 ms) -> ms */
    }
    case SYS_NET_DNS:
        if (!net_up()) return -1;
        {
            char name[USER_PATH_MAX];
            if (user_copy_string(name, sizeof name, (const char *)a) < 0) return -1;
            dns_start(name);
        }
        return 0;
    case SYS_NET_DNS_RESULT:
        return (long)(int)dns_result();
    case SYS_HTTP_GET: {
        /* Fetch + build the page (DOM -> computed style -> layout). http_get and
         * res_fetch (sub-resources/images) block while pumping net_poll, which
         * needs a live PIT base; the int 0x80 gate cleared IF, so re-enable
         * interrupts across the whole pipeline, then restore for the iretq. */
        struct win *w = app_window(ap);
        int cw = w ? w->surf.w : 600;
        int grc;
        char url[USER_URL_MAX];
        if (user_copy_string(url, sizeof url, (const char *)a) < 0) return -1;
        __asm__ volatile ("sti");
        page_reset();
        uint64_t t0 = timer_ticks();
        grc = http_get(url);
        kprintf("[page] http_get rc=%d status=%d t=%dms\n", grc, http_status(), (int)(timer_ticks()-t0)*10);
        if (grc == 0 && http_status() == HTTP_DONE) {
            int blen; const char *body = http_body(&blen);
            kprintf("[page] body=%d bytes\n", blen);
            uint64_t t1 = timer_ticks();
            page_root = dom_parse(body, blen);
            kprintf("[page] dom_parse t=%dms\n", (int)(timer_ticks()-t1)*10);
            if (page_root) {
                static char author_css[16384];
                int css_len = collect_style(page_root, author_css, 0, (int)sizeof author_css);
                uint64_t t2 = timer_ticks();
                css_apply(page_root, author_css, css_len);
                kprintf("[page] css_apply (%d css) t=%dms\n", css_len, (int)(timer_ticks()-t2)*10);
                uint64_t t3 = timer_ticks();
                layout_page(page_root, cw);
                kprintf("[page] layout items=%d h=%d t=%dms\n", layout_count(), layout_height(), (int)(timer_ticks()-t3)*10);
                page_built = 1;
                /* NB: images are NOT fetched here -- the browser paints the text
                 * first, then calls SYS_PAGE_LOAD_IMAGES so a text page never
                 * stalls on its (often large, cross-origin) decorations. */
            } else grc = HTTP_ERR;
        }
        __asm__ volatile ("cli");
        return grc;
    }
    case SYS_HTTP_STATUS:
        return http_status();
    case SYS_PAGE_RENDER: {
        struct win *w = app_window(ap); if (!w) return -1;
        if (!page_built) return 0;
        int scroll = (int)a;
        int vx = (int)((b >> 16) & 0xFFFF), vy = (int)(b & 0xFFFF);
        int vw = (int)((c >> 16) & 0xFFFF), vh = (int)(c & 0xFFFF);
        fb_target(&w->surf);
        paint_viewport(vx, vy, vw, vh, scroll);
        fb_target(NULL);
        return 0;
    }
    case SYS_PAGE_HEIGHT:
        return page_built ? layout_height() : 0;
    case SYS_PAGE_HITTEST: {
        if (!page_built) return 0;
        int x = (int)((a >> 16) & 0xFFFF), y = (int)(a & 0xFFFF);
        if (!user_range_ok((char *)c, 256, 1)) return -1;
        return paint_hittest(x, y, (int)b, (char *)c, 256);
    }
    case SYS_PAGE_LOAD_IMAGES: {
        /* Blocking sub-resource fetch; same IF=1 requirement as SYS_HTTP_GET. */
        if (!page_built) return 0;
        int max = (int)a; if (max < 0) max = 0; if (max > 16) max = 16;
        __asm__ volatile ("sti");
        int n = layout_load_images(max);
        __asm__ volatile ("cli");
        return n;
    }
    case SYS_PAGE_SCRIPTS: {
        if (!page_built || !page_root) return 0;
        char *ubuf = (char *)a; int max = (int)b;
        if (max <= 0 || !user_range_ok(ubuf, (uint64_t)max, 1)) return -1;
        int o = collect_scripts(page_root, ubuf, 0, max);
        ubuf[o < max ? o : max - 1] = 0;
        return o;
    }
    }
    return -1;
}

/* Called from SYS_EXIT: mark the app dead; the WM reaps its window. */
void wm_app_exit(void)
{
    struct app *ap = sched_current_data();
    if (ap) ap->alive = 0;
    dirty = 1;
}

static void enqueue(struct win *w, int type, int a, int b)
{
    int nt = (w->evtail + 1) % 16;
    if (nt == w->evhead) return;       /* full, drop */
    w->evq[w->evtail].type = type;
    w->evq[w->evtail].a = a;
    w->evq[w->evtail].b = b;
    w->evtail = nt;
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

/* ---------- desktop chrome ---------- */
static void draw_background(void)
{
    for (int y = 0; y < H; y++) {
        uint32_t c = rgb((uint8_t)lerp(22, 96, y, H), (uint8_t)lerp(44, 165, y, H),
                         (uint8_t)lerp(120, 230, y, H));
        for (int x = 0; x < W; x++) fb_put(x, y, c);
    }
    fb_blend_rect(0, 0, W, MENUBAR_H, 245, 246, 250, 185);
    uint32_t ink = rgb(40, 40, 46);
    fb_fill_circle(16, MENUBAR_H / 2, 6, ink);
    fb_text(32, 4, "Aqua OS", ink);
    fb_text(112, 4, "File", ink);
    fb_text(156, 4, "Edit", ink);
    fb_text(200, 4, "View", ink);
}

static int dock_x0, dock_y0, dock_isz = 50, dock_gap = 14;
static void draw_dock(void)
{
    int n = nreg < 1 ? 1 : nreg;
    int dw = dock_gap + n * (dock_isz + dock_gap), dh = dock_isz + 20;
    dock_x0 = (W - dw) / 2; dock_y0 = H - dh - 12;
    fb_blend_round_rect(dock_x0, dock_y0, dw, dh, 22, 255, 255, 255, 95);
    for (int i = 0; i < nreg; i++) {
        int ix = dock_x0 + dock_gap + i * (dock_isz + dock_gap), iy = dock_y0 + 10;
        fb_round_rect(ix, iy, dock_isz, dock_isz, 12, reg[i].color);
        fb_blend_round_rect(ix, iy, dock_isz, dock_isz / 2, 12, 255, 255, 255, 40);
        char ch[2] = { reg[i].icon, 0 };
        fb_text(ix + dock_isz / 2 - FW / 2, iy + dock_isz / 2 - FH / 2, ch, rgb(255, 255, 255));
    }
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
    fb_text(W - fb_text_width(b) - 12, 4, b, rgb(40, 40, 46));
}

/* ---------- Finder (builtin) ---------- */
#define FROW 26
static int finder_top(struct win *w) { return w->y + TITLEBAR_H + 36; }
static int finder_at_root(struct win *w) { return w->cwd[0] == '/' && w->cwd[1] == 0; }

static void draw_finder(struct win *w)
{
    int x = w->x;
    fb_text(x + 16, w->y + TITLEBAR_H + 10, w->cwd, rgb(120, 120, 128));
    int row = 0;
    if (!finder_at_root(w)) {                       /* ".." to go up */
        int yy = finder_top(w) + row * FROW;
        fb_round_rect(x + 16, yy, 13, 16, 3, rgb(230, 185, 90));
        fb_text(x + 38, yy, "..", rgb(60, 60, 68));
        row++;
    }
    int n = vfs_count(w->cwd);
    for (int i = 0; i < n; i++, row++) {
        int yy = finder_top(w) + row * FROW;
        int isdir = vfs_ent_is_dir(w->cwd, i);
        fb_round_rect(x + 16, yy, 13, 16, 3, isdir ? rgb(230, 185, 90) : rgb(90, 150, 240));
        fb_text(x + 38, yy, vfs_ent_name(w->cwd, i), rgb(60, 60, 68));
    }
    fb_text(x + 16, w->y + w->h - 22, "click a folder or file", rgb(170, 170, 178));
}

/* ---------- window frame + compositing ---------- */
static void draw_frame(struct win *w, int focused)
{
    int x = w->x, y = w->y, ww = w->w, wh = w->h;
    fb_blend_round_rect(x - 5, y + 8, ww + 10, wh + 10, 18, 0, 0, 0, focused ? 60 : 35);
    fb_round_rect(x, y, ww, wh, 10, rgb(250, 250, 252));
    uint32_t tb = focused ? rgb(235, 235, 240) : rgb(245, 245, 248);
    fb_round_rect(x, y, ww, TITLEBAR_H, 10, tb);
    fb_fill_rect(x, y + 20, ww, TITLEBAR_H - 20, tb);
    fb_fill_rect(x, y + TITLEBAR_H, ww, 1, rgb(214, 214, 220));
    uint32_t off = rgb(205, 205, 210);
    fb_fill_circle(x + 16, y + 15, 6, focused ? rgb(255, 95, 86) : off);  /* close */
    fb_fill_circle(x + 34, y + 15, 6, focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + 52, y + 15, 6, focused ? rgb(40, 200, 64) : off);
    int tw = fb_text_width(w->title);
    fb_text(x + (ww - tw) / 2, y + 7, w->title, rgb(70, 70, 78));
}

static const char *cursor_bmp[] = {
    "#","##","#.#","#..#","#...#","#....#","#.....#","#......#","#.......#",
    "#........#","#.....####","#..#..#","#.# #..#","##  #..#","#    #..#","     #..#","      ##",
};
static void draw_cursor(int x, int y)
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

void wm_render(void)
{
    reap();
    blit(back, bg, W * H);                 /* restore wallpaper + menu + dock */
    fb_target(NULL);
    draw_clock();
    for (int i = 0; i < norder; i++) {
        struct win *w = &wins[order[i]];
        if (!w->used) continue;
        int focused = (i == norder - 1);
        draw_frame(w, focused);
        if (w->kind == WK_FINDER)
            draw_finder(w);
        else if (w->surf.px)
            fb_blit_surface(w->x, w->y + TITLEBAR_H, &w->surf);
    }
    draw_cursor(mx, my);
    fb_present();
}

/* ---------- input ---------- */
static int in_rect(int px, int py, int x, int y, int w, int h)
{ return px >= x && px < x + w && py >= y && py < y + h; }

void wm_key(int c)
{
    if (norder == 0) return;
    struct win *w = &wins[order[norder - 1]];
    if (w->kind == WK_APP) { enqueue(w, EV_KEY, (int)c, 0); dirty = 1; }
}

void wm_mouse_event(int x, int y, int left)
{
    mx = x; my = y;

    if (left && !mleft) {
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
            } else if (w->kind == WK_FINDER) {
                int row = (y - finder_top(w)) / FROW;
                int atroot = finder_at_root(w);
                if (row < 0) {
                    /* nothing */
                } else if (!atroot && row == 0) {
                    path_up(w->cwd);                           /* go to parent */
                } else {
                    int idx = atroot ? row : row - 1;
                    if (idx >= 0 && idx < vfs_count(w->cwd)) {
                        int isdir = vfs_ent_is_dir(w->cwd, idx);
                        char nm[64];
                        scopy(nm, vfs_ent_name(w->cwd, idx), sizeof nm);
                        if (isdir) {
                            path_join(w->cwd, w->cwd, nm, sizeof w->cwd);  /* enter */
                        } else {
                            char full[160];
                            path_join(full, w->cwd, nm, sizeof full);
                            if (ends_aex(nm)) wm_launch(full, "");
                            else launch_for_ext(ext_of(nm), full);
                        }
                    }
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
    if (!left) dragging = -1;
    if (dragging >= 0 && left) { wins[dragging].x = x - drag_dx; wins[dragging].y = y - drag_dy; }
    mleft = left;
    dirty = 1;
}

/* ---------- registry + init ---------- */
static void scan_apps(void)
{
    int n = vfs_count("/");
    for (int i = 0; i < n && nreg < MAXWIN; i++) {
        char nm[64];
        scopy(nm, vfs_ent_name("/", i), sizeof nm);
        if (!ends_aex(nm)) continue;
        /* aquafs reads whole-file (errors if the buffer is smaller), so size the
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

    /* builtin Finder window */
    wins[0].used = 1; wins[0].kind = WK_FINDER;
    wins[0].x = 30; wins[0].y = 60; wins[0].w = 280; wins[0].h = 380;
    scopy(wins[0].title, "Finder", sizeof wins[0].title);
    scopy(wins[0].cwd, "/", sizeof wins[0].cwd);
    order[norder++] = 0;

    draw_background();
    draw_dock();
    blit(bg, back, count);
}

void wm_render_first(void) { wm_render(); }

void wm_run(void)
{
    __asm__ volatile ("mov $0x10, %%ax\n\tmov %%ax,%%ds\n\tmov %%ax,%%es\n\t"
                      "mov %%ax,%%fs\n\tmov %%ax,%%gs" ::: "ax");
    serial_puts("\n[wm] desktop live; launching apps as ring-3 processes\n");

    sched_init();

    /* The WM runs as a ring-0 thread; it MUST keep interrupts enabled so the
     * timer/mouse/keyboard keep firing even when no app is running (otherwise
     * closing the last app would leave nothing with IF=1 and freeze input). */
    __asm__ volatile ("sti");

    /* auto-launch the clock so something is alive on screen at boot */
    wm_launch("clock.aex", "");

    uint64_t last = 0;
    for (;;) {
        net_poll();                  /* drive the (polled) network RX path */
        uint64_t now = timer_ticks();
        if (dirty || now - last >= 12) { dirty = 0; last = now; wm_render(); }
        schedule();
    }
}
