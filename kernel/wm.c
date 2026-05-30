#include <stdint.h>
#include <stddef.h>
#include "wm.h"
#include "fb.h"
#include "pmm.h"
#include "pit.h"
#include "serial.h"
#include "sched.h"
#include "vfs.h"
#include "syscall.h"

#define NWIN       4
#define MENUBAR_H  24
#define TITLEBAR_H 30
#define FW         AQUA_FONT_W
#define FH         AQUA_FONT_H

enum wkind { W_FINDER, W_CONSOLE, W_ACTIVITY, W_NOTES };

struct window {
    int x, y, w, h;
    const char *title;
    enum wkind kind;
};

static struct window win[NWIN];
static int order[NWIN];          /* order[NWIN-1] is topmost / focused */
static int mx, my, mleft;
static int dragging = -1, drag_dx, drag_dy;
static volatile int dirty = 1;

static uint32_t *back, *bg;
static int W, H;

/* Notes window text (filled by the keyboard). */
static char notes[512];
static int  notes_len;

/* M4: three worker threads bump these; the Activity window shows them live. */
static volatile uint64_t work[3];

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return fb_rgb(r, g, b); }
static int lerp(int a, int b, int n, int d) { return a + (b - a) * n / d; }

static void blit(uint32_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = src[i];
}

/* uint -> decimal string */
static char *ustr(uint32_t v, char *b)
{
    char t[12];
    int i = 0;
    if (!v) { b[0] = '0'; b[1] = 0; return b; }
    while (v) { t[i++] = '0' + v % 10; v /= 10; }
    int j = 0;
    while (i) b[j++] = t[--i];
    b[j] = 0;
    return b;
}

/* dst = prefix + num + suffix */
static void label(char *dst, const char *pre, uint32_t num, const char *suf)
{
    int p = 0;
    while (*pre) dst[p++] = *pre++;
    char n[12];
    ustr(num, n);
    for (char *q = n; *q;) dst[p++] = *q++;
    while (*suf) dst[p++] = *suf++;
    dst[p] = 0;
}

/* Word-wrapped text within `maxw`; reports the end pen position. */
static void wrapped(int x, int y, int maxw, const char *s, uint32_t color,
                    int *ex, int *ey)
{
    int cx = x, cy = y, cols = maxw / FW, col = 0;
    for (; *s; s++) {
        if (*s == '\n' || col >= cols) {
            cx = x; cy += FH; col = 0;
            if (*s == '\n') continue;
        }
        fb_char(cx, cy, *s, color);
        cx += FW; col++;
    }
    if (ex) *ex = cx;
    if (ey) *ey = cy;
}

/* ---- static background: wallpaper + menu bar + dock ---- */
static void draw_background(void)
{
    for (int y = 0; y < H; y++) {
        uint32_t c = rgb((uint8_t)lerp(22, 96, y, H),
                         (uint8_t)lerp(44, 165, y, H),
                         (uint8_t)lerp(120, 230, y, H));
        for (int x = 0; x < W; x++)
            fb_put(x, y, c);
    }

    fb_blend_rect(0, 0, W, MENUBAR_H, 245, 246, 250, 185);
    uint32_t ink = rgb(40, 40, 46);
    fb_fill_circle(16, MENUBAR_H / 2, 6, ink);
    fb_text(32, 4, "Aqua OS", ink);
    fb_text(112, 4, "File", ink);
    fb_text(156, 4, "Edit", ink);
    fb_text(200, 4, "View", ink);

    int dn = 7, isz = 50, gap = 14;
    int dw = gap + dn * (isz + gap), dh = isz + 20;
    int dx = (W - dw) / 2, dy = H - dh - 12;
    fb_blend_round_rect(dx, dy, dw, dh, 22, 255, 255, 255, 95);
    uint32_t icons[7] = {
        rgb(80, 140, 255),  rgb(55, 200, 120),  rgb(255, 92, 92),
        rgb(255, 170, 40),  rgb(170, 110, 255), rgb(40, 200, 220),
        rgb(255, 120, 170),
    };
    for (int i = 0; i < dn; i++) {
        int ix = dx + gap + i * (isz + gap), iy = dy + 10;
        fb_round_rect(ix, iy, isz, isz, 12, icons[i]);
        fb_blend_round_rect(ix, iy, isz, isz / 2, 12, 255, 255, 255, 40);
    }
}

static void draw_clock(void)
{
    uint64_t t = timer_ticks() / 100;
    int mm = (int)((t / 60) % 60), ss = (int)(t % 60);
    char buf[6] = { '0' + mm / 10, '0' + mm % 10, ':', '0' + ss / 10, '0' + ss % 10, 0 };
    fb_text(W - 48, 4, buf, rgb(40, 40, 46));
}

/* ---- per-window content, each backed by a real subsystem ---- */
static void content_finder(struct window *w)
{
    int x = w->x, y = w->y + TITLEBAR_H + 10;
    fb_text(x + 16, y, "AquaFS  /", rgb(120, 120, 128));
    int n = vfs_count();
    for (int i = 0; i < n; i++) {
        int yy = y + 26 + i * 26;
        fb_round_rect(x + 16, yy, 13, 16, 3, rgb(90, 150, 240));   /* file icon */
        fb_fill_rect(x + 20, yy + 3, 9, 2, rgb(255, 255, 255));
        fb_fill_rect(x + 20, yy + 7, 9, 2, rgb(255, 255, 255));
        fb_text(x + 38, yy, vfs_ent_name(i), rgb(60, 60, 68));
        char sz[16];
        label(sz, "", (uint32_t)vfs_ent_size(i), " B");
        fb_text(x + w->w - 16 - fb_text_width(sz), yy, sz, rgb(150, 150, 158));
    }
}

static void content_console(struct window *w)
{
    const char *out = syscall_console();
    int x = w->x + 14, y = w->y + TITLEBAR_H + 10;
    if (!out || !out[0]) {
        fb_text(x, y, "(no userland output yet)", rgb(150, 150, 158));
        return;
    }
    fb_text(x, y, "ring 3 -> int 0x80:", rgb(120, 120, 128));
    wrapped(x, y + 22, w->w - 28, out, rgb(50, 110, 60), NULL, NULL);
}

static void bar(int x, int y, int wdt, int frac256, uint32_t fill)
{
    fb_round_rect(x, y, wdt, 10, 5, rgb(225, 226, 231));
    int fw = wdt * frac256 / 256;
    if (fw < 6) fw = 6;
    fb_round_rect(x, y, fw, 10, 5, fill);
}

static void content_activity(struct window *w)
{
    int x = w->x + 16, y = w->y + TITLEBAR_H + 12;
    char buf[40];

    label(buf, "Uptime: ", (uint32_t)(timer_ticks() / 100), " s");
    fb_text(x, y, buf, rgb(60, 60, 68));

    uint32_t total = (uint32_t)(pmm_total_bytes() >> 20);
    uint32_t freeb = (uint32_t)(pmm_free_bytes() >> 20);
    uint32_t used = total - freeb;
    label(buf, "Memory: ", used, "");
    int p = 0; while (buf[p]) p++;
    char t2[16]; label(t2, " / ", total, " MB");
    for (char *q = t2; *q;) buf[p++] = *q++;
    buf[p] = 0;
    fb_text(x, y + 24, buf, rgb(60, 60, 68));
    bar(x, y + 42, w->w - 32, total ? (int)(used * 256 / total) : 0, rgb(90, 150, 240));

    fb_text(x, y + 64, "Threads (M4 scheduler):", rgb(120, 120, 128));
    uint32_t accent[3] = { rgb(255, 120, 120), rgb(120, 200, 120), rgb(120, 150, 255) };
    for (int k = 0; k < 3; k++) {
        int yy = y + 88 + k * 30;
        char nm[12]; label(nm, "w", (uint32_t)k, ":");
        fb_text(x, yy, nm, rgb(60, 60, 68));
        char cnt[16]; ustr((uint32_t)work[k], cnt);
        fb_text(x + 34, yy, cnt, rgb(110, 110, 118));
        bar(x, yy + 16, w->w - 32, (int)(work[k] % 256), accent[k]);
    }
}

static void content_notes(struct window *w)
{
    int x = w->x + 14, y = w->y + TITLEBAR_H + 10;
    if (notes_len == 0) {
        fb_text(x, y, "Type on the keyboard...", rgb(150, 150, 158));
        return;
    }
    int ex, ey;
    wrapped(x, y, w->w - 28, notes, rgb(50, 50, 58), &ex, &ey);
    fb_fill_rect(ex, ey, 8, FH, rgb(90, 150, 240));   /* caret */
}

static void draw_window(struct window *w, int focused)
{
    int x = w->x, y = w->y, ww = w->w, wh = w->h;

    fb_blend_round_rect(x - 5, y + 8, ww + 10, wh + 10, 18, 0, 0, 0,
                        focused ? 60 : 35);
    fb_round_rect(x, y, ww, wh, 10, rgb(252, 252, 253));

    uint32_t tb = focused ? rgb(235, 235, 240) : rgb(245, 245, 248);
    fb_round_rect(x, y, ww, TITLEBAR_H, 10, tb);
    fb_fill_rect(x, y + 20, ww, TITLEBAR_H - 20, tb);
    fb_fill_rect(x, y + TITLEBAR_H, ww, 1, rgb(214, 214, 220));

    uint32_t off = rgb(205, 205, 210);
    fb_fill_circle(x + 16, y + 15, 6, focused ? rgb(255, 95, 86) : off);
    fb_fill_circle(x + 34, y + 15, 6, focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + 52, y + 15, 6, focused ? rgb(40, 200, 64) : off);

    int tw = fb_text_width(w->title);
    fb_text(x + (ww - tw) / 2, y + 7, w->title, rgb(70, 70, 78));

    switch (w->kind) {
    case W_FINDER:   content_finder(w);   break;
    case W_CONSOLE:  content_console(w);  break;
    case W_ACTIVITY: content_activity(w); break;
    case W_NOTES:    content_notes(w);    break;
    }
}

static const char *cursor_bmp[] = {
    "#",        "##",       "#.#",      "#..#",     "#...#",
    "#....#",   "#.....#",  "#......#", "#.......#","#........#",
    "#.....####","#..#..#", "#.# #..#", "##  #..#", "#    #..#",
    "     #..#", "      ##",
};

static void draw_cursor(int x, int y)
{
    uint32_t outline = rgb(20, 20, 26), fill = rgb(255, 255, 255);
    int rows = (int)(sizeof cursor_bmp / sizeof cursor_bmp[0]);
    for (int r = 0; r < rows; r++)
        for (int c = 0; cursor_bmp[r][c]; c++) {
            char p = cursor_bmp[r][c];
            if (p == '#')      fb_put(x + c, y + r, outline);
            else if (p == '.') fb_put(x + c, y + r, fill);
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

    mx = W / 2;
    my = H / 2;

    win[0] = (struct window){ 40,  56,  300, 440, "Finder",          W_FINDER };
    win[1] = (struct window){ 366, 56,  432, 206, "Console",         W_CONSOLE };
    win[2] = (struct window){ 366, 286, 432, 270, "Activity Monitor", W_ACTIVITY };
    win[3] = (struct window){ 150, 430, 380, 200, "Notes",           W_NOTES };
    for (int i = 0; i < NWIN; i++)
        order[i] = i;

    draw_background();
    blit(bg, back, count);
}

void wm_render(void)
{
    blit(back, bg, W * H);
    draw_clock();
    for (int i = 0; i < NWIN; i++)
        draw_window(&win[order[i]], i == NWIN - 1);
    draw_cursor(mx, my);
    fb_present();
}

static int hit(struct window *w, int x, int y)
{
    return x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h;
}

void wm_mouse_event(int x, int y, int left)
{
    mx = x;
    my = y;

    if (left && !mleft) {
        for (int i = NWIN - 1; i >= 0; i--) {
            struct window *w = &win[order[i]];
            if (!hit(w, x, y))
                continue;
            int id = order[i];
            for (int j = i; j < NWIN - 1; j++)
                order[j] = order[j + 1];
            order[NWIN - 1] = id;
            if (y < w->y + TITLEBAR_H) {
                dragging = id;
                drag_dx = x - w->x;
                drag_dy = y - w->y;
            }
            break;
        }
    }
    if (!left)
        dragging = -1;
    if (dragging >= 0 && left) {
        win[dragging].x = x - drag_dx;
        win[dragging].y = y - drag_dy;
    }

    mleft = left;
    dirty = 1;
}

void wm_key(char c)
{
    if (c == '\b') {
        if (notes_len > 0)
            notes[--notes_len] = '\0';
    } else if (notes_len < (int)sizeof(notes) - 1) {
        notes[notes_len++] = c;
        notes[notes_len] = '\0';
    }
    dirty = 1;
}

/* M4: background worker threads, scheduled preemptively + cooperatively. */
static void worker(int k)
{
    for (;;) {
        work[k]++;
        for (volatile int i = 0; i < 500000; i++)
            ;
        schedule();
    }
}
static void worker0(void) { worker(0); }
static void worker1(void) { worker(1); }
static void worker2(void) { worker(2); }

void wm_run(void)
{
    __asm__ volatile ("mov $0x10, %%ax\n\t"
                      "mov %%ax, %%ds\n\tmov %%ax, %%es\n\t"
                      "mov %%ax, %%fs\n\tmov %%ax, %%gs" ::: "ax");

    serial_puts("\n[wm] desktop live; M2-M6 wired into windows\n");

    sched_init();
    thread_create(worker0, "w0");
    thread_create(worker1, "w1");
    thread_create(worker2, "w2");

    uint64_t last = 0;
    for (;;) {
        uint64_t now = timer_ticks();
        if (dirty || now - last >= 20) {
            dirty = 0;
            last = now;
            wm_render();
        }
        schedule();              /* yield to the worker threads */
    }
}
