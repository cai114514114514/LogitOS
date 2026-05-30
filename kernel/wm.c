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
#include "rtc.h"

#define NWIN       4
#define MENUBAR_H  24
#define TITLEBAR_H 30
#define FW         AQUA_FONT_W
#define FH         AQUA_FONT_H

enum wkind { W_FINDER, W_TERMINAL, W_VIEWER, W_ACTIVITY };

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

/* M4: three worker threads bump these; the Activity window shows them live. */
static volatile uint64_t work[3];

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return fb_rgb(r, g, b); }
static int lerp(int a, int b, int n, int d) { return a + (b - a) * n / d; }

static void blit(uint32_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = src[i];
}

/* ---- small string helpers (no libc) ---- */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int starts(const char *s, const char *p)
{
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}
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

/* ============================ terminal / shell ============================ */
#define TROWS 18
#define TCOLS 58

static char tbuf[TROWS][TCOLS];
static int  trow, tcol;
static char tin[TCOLS];          /* current input line */
static int  tin_len;

static void term_newline(void)
{
    if (trow < TROWS - 1) {
        trow++;
    } else {
        for (int r = 0; r < TROWS - 1; r++)
            for (int c = 0; c < TCOLS; c++)
                tbuf[r][c] = tbuf[r + 1][c];
        for (int c = 0; c < TCOLS; c++)
            tbuf[TROWS - 1][c] = 0;
    }
    tcol = 0;
}

static void term_putc(char c)
{
    if (c == '\n') { term_newline(); return; }
    if (tcol >= TCOLS - 1)
        term_newline();
    tbuf[trow][tcol++] = c;
    tbuf[trow][tcol] = 0;
}

static void term_print(const char *s) { while (*s) term_putc(*s++); }
static void term_num(uint32_t v) { char b[12]; term_print(ustr(v, b)); }

/* Read a file off the disk into buf; classify text vs binary. */
static char filebuf[8192];
static int load_file(const char *name, char *dst, int cap, int *is_text)
{
    int n = vfs_read(name, filebuf, sizeof filebuf);
    if (n <= 0)
        return -1;
    int text = 1;
    int look = n < 64 ? n : 64;
    for (int i = 0; i < look; i++) {
        unsigned char ch = (unsigned char)filebuf[i];
        if (ch < 9 || (ch > 13 && ch < 32) || ch >= 127) { text = 0; break; }
    }
    if (is_text) *is_text = text;
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++)
        dst[i] = filebuf[i];
    dst[n] = 0;
    return n;
}

static void shell_exec(const char *line)
{
    term_print("$ ");
    term_print(line);
    term_print("\n");

    if (line[0] == '\0') {
        /* nothing */
    } else if (streq(line, "help")) {
        term_print("commands: help ls cat <f> mem ps clear echo <t> uname\n");
    } else if (streq(line, "ls")) {
        int n = vfs_count();
        for (int i = 0; i < n; i++) {
            term_print(vfs_ent_name(i));
            term_print("  ");
            term_num((uint32_t)vfs_ent_size(i));
            term_print(" B\n");
        }
    } else if (starts(line, "cat ")) {
        const char *nm = line + 4;
        while (*nm == ' ') nm++;
        static char txt[2048];
        int istext;
        int n = load_file(nm, txt, sizeof txt, &istext);
        if (n < 0)       term_print("cat: no such file\n");
        else if (!istext) { term_print("cat: binary file ("); term_num((uint32_t)n);
                            term_print(" bytes)\n"); }
        else { term_print(txt); if (n && txt[n - 1] != '\n') term_print("\n"); }
    } else if (streq(line, "mem")) {
        term_print("total ");  term_num((uint32_t)(pmm_total_bytes() >> 20));
        term_print(" MB  free "); term_num((uint32_t)(pmm_free_bytes() >> 20));
        term_print(" MB\n");
    } else if (streq(line, "ps")) {
        term_print("PID NAME STATE\n");
        term_print("  0 wm   run\n  1 w0   run\n  2 w1   run\n  3 w2   run\n");
        term_print("ctx switches: "); term_num((uint32_t)sched_switches()); term_print("\n");
    } else if (streq(line, "clear")) {
        for (int r = 0; r < TROWS; r++) tbuf[r][0] = 0;
        trow = tcol = 0;
    } else if (starts(line, "echo ")) {
        term_print(line + 5);
        term_print("\n");
    } else if (streq(line, "uname")) {
        term_print("Aqua OS x86_64 -- from scratch (M1-M8)\n");
    } else {
        term_print("command not found: ");
        term_print(line);
        term_print("\n");
    }
}

/* ============================ viewer ============================ */
static char view_buf[2048];
static char view_name[48];
static int  view_is_text;

static void open_file_in_viewer(const char *name)
{
    int i = 0;
    while (name[i] && i < (int)sizeof(view_name) - 1) { view_name[i] = name[i]; i++; }
    view_name[i] = 0;
    int n = load_file(name, view_buf, sizeof view_buf, &view_is_text);
    if (n < 0) {
        view_name[0] = 0;
    }
}

/* ============================ background ============================ */
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

static int fmt2(char *b, int v) { b[0] = '0' + (v / 10) % 10; b[1] = '0' + v % 10; return 2; }

static void draw_clock(void)
{
    static const char *wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    struct rtc_time t;
    rtc_now(&t);

    char buf[32];
    int p = 0;
    const char *w = wd[t.weekday % 7];
    buf[p++] = w[0]; buf[p++] = w[1]; buf[p++] = w[2]; buf[p++] = ' ';
    buf[p++] = '0' + (t.year / 1000) % 10; buf[p++] = '0' + (t.year / 100) % 10;
    buf[p++] = '0' + (t.year / 10) % 10;   buf[p++] = '0' + t.year % 10;
    buf[p++] = '-'; p += fmt2(buf + p, t.month);
    buf[p++] = '-'; p += fmt2(buf + p, t.day);
    buf[p++] = ' '; buf[p++] = ' ';
    p += fmt2(buf + p, t.hour);   buf[p++] = ':';
    p += fmt2(buf + p, t.minute); buf[p++] = ':';
    p += fmt2(buf + p, t.second);
    buf[p] = 0;

    fb_text(W - fb_text_width(buf) - 12, 4, buf, rgb(40, 40, 46));
}

/* ============================ window content ============================ */
#define FINDER_ROW_H   26
static int finder_list_top(struct window *w) { return w->y + TITLEBAR_H + 36; }

static void content_finder(struct window *w)
{
    int x = w->x;
    fb_text(x + 16, w->y + TITLEBAR_H + 10, "AquaFS  /", rgb(120, 120, 128));
    int n = vfs_count();
    for (int i = 0; i < n; i++) {
        int yy = finder_list_top(w) + i * FINDER_ROW_H;
        fb_round_rect(x + 16, yy, 13, 16, 3, rgb(90, 150, 240));
        fb_fill_rect(x + 20, yy + 4, 9, 2, rgb(255, 255, 255));
        fb_fill_rect(x + 20, yy + 8, 9, 2, rgb(255, 255, 255));
        fb_text(x + 38, yy, vfs_ent_name(i), rgb(60, 60, 68));
        char sz[16];
        ustr((uint32_t)vfs_ent_size(i), sz);
        fb_text(x + w->w - 16 - fb_text_width(sz) - 16, yy, sz, rgb(150, 150, 158));
    }
    fb_text(x + 16, w->y + w->h - 22, "click a file to open it", rgb(170, 170, 178));
}

static void content_terminal(struct window *w, int focused)
{
    int x = w->x + 12, y = w->y + TITLEBAR_H + 8;
    for (int r = 0; r < TROWS; r++)
        if (tbuf[r][0])
            fb_text(x, y + r * FH, tbuf[r], rgb(55, 58, 66));
    int iy = y + TROWS * FH;
    fb_text(x, iy, "$ ", rgb(90, 150, 240));
    fb_text(x + 2 * FW, iy, tin, rgb(40, 40, 48));
    if (focused)
        fb_fill_rect(x + (2 + tin_len) * FW, iy, 8, FH, rgb(90, 150, 240));
}

static void content_viewer(struct window *w)
{
    int x = w->x + 14, y = w->y + TITLEBAR_H + 10;
    if (!view_name[0]) {
        fb_text(x, y, "(click a file in Finder to open it)", rgb(150, 150, 158));
        return;
    }
    char hdr[64];
    int p = 0; const char *o = "open: ";
    while (*o) hdr[p++] = *o++;
    for (int i = 0; view_name[i]; i++) hdr[p++] = view_name[i];
    hdr[p] = 0;
    fb_text(x, y, hdr, rgb(120, 120, 128));
    if (view_is_text)
        wrapped(x, y + 24, w->w - 28, view_buf, rgb(50, 50, 58), NULL, NULL);
    else
        fb_text(x, y + 24, "(binary file -- not shown as text)", rgb(150, 150, 158));
}

static void bar(int x, int y, int wdt, int frac256, uint32_t fill)
{
    fb_round_rect(x, y, wdt, 9, 4, rgb(225, 226, 231));
    int fw = wdt * frac256 / 256;
    if (fw < 8) fw = 8;
    fb_round_rect(x, y, fw, 9, 4, fill);
}

static void content_activity(struct window *w)
{
    int x = w->x + 16, y = w->y + TITLEBAR_H + 12;
    char buf[16];

    fb_text(x, y, "Uptime", rgb(120, 120, 128));
    ustr((uint32_t)(timer_ticks() / 100), buf);
    fb_text(x + w->w - 48, y, buf, rgb(60, 60, 68));
    fb_text(x + w->w - 48 + fb_text_width(buf), y, "s", rgb(120, 120, 128));

    uint32_t total = (uint32_t)(pmm_total_bytes() >> 20);
    uint32_t freeb = (uint32_t)(pmm_free_bytes() >> 20);
    uint32_t used = total - freeb;
    fb_text(x, y + 26, "Memory", rgb(120, 120, 128));
    bar(x, y + 44, w->w - 32, total ? (int)(used * 256 / total) : 0, rgb(90, 150, 240));

    fb_text(x, y + 66, "Threads (scheduler)", rgb(120, 120, 128));
    uint32_t accent[3] = { rgb(255, 120, 120), rgb(120, 200, 120), rgb(120, 150, 255) };
    const char *nm[3] = { "w0", "w1", "w2" };
    for (int k = 0; k < 3; k++) {
        int yy = y + 88 + k * 24;
        fb_text(x, yy, nm[k], rgb(80, 80, 88));
        /* smooth triangle-wave activity meter from the worker's progress */
        uint32_t ph = (uint32_t)(work[k] % 120);
        uint32_t lvl = ph < 60 ? ph : 120 - ph;          /* 0..60 */
        bar(x + 28, yy + 2, w->w - 60, (int)(lvl * 256 / 60), accent[k]);
    }
    fb_text(x, y + 164, "ctx switches", rgb(120, 120, 128));
    ustr((uint32_t)sched_switches(), buf);
    fb_text(x + w->w - 32 - fb_text_width(buf), y + 164, buf, rgb(60, 60, 68));
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
    case W_FINDER:   content_finder(w);          break;
    case W_TERMINAL: content_terminal(w, focused); break;
    case W_VIEWER:   content_viewer(w);          break;
    case W_ACTIVITY: content_activity(w);        break;
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

    win[0] = (struct window){ 30,  56,  290, 388, "Finder",           W_FINDER };
    win[1] = (struct window){ 340, 56,  480, 400, "Terminal",         W_TERMINAL };
    win[2] = (struct window){ 30,  460, 500, 224, "Viewer",           W_VIEWER };
    win[3] = (struct window){ 548, 476, 300, 208, "Activity Monitor", W_ACTIVITY };
    /* Terminal (index 1) focused on top so you can type immediately. */
    order[0] = 0; order[1] = 3; order[2] = 2; order[3] = 1;

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

static void raise_to_top(int idx_in_order)
{
    int id = order[idx_in_order];
    for (int j = idx_in_order; j < NWIN - 1; j++)
        order[j] = order[j + 1];
    order[NWIN - 1] = id;
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
            raise_to_top(i);
            if (y < w->y + TITLEBAR_H) {
                dragging = id;
                drag_dx = x - w->x;
                drag_dy = y - w->y;
            } else if (w->kind == W_FINDER) {
                int row = (y - finder_list_top(w)) / FINDER_ROW_H;
                if (row >= 0 && row < vfs_count()) {
                    open_file_in_viewer(vfs_ent_name(row));
                    /* bring the Viewer to the front so the file is visible */
                    for (int k = 0; k < NWIN; k++)
                        if (win[order[k]].kind == W_VIEWER) { raise_to_top(k); break; }
                }
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
    /* Focus-aware: input only reaches the focused window, and only the
     * terminal accepts typing. */
    struct window *f = &win[order[NWIN - 1]];
    if (f->kind != W_TERMINAL)
        return;

    if (c == '\n') {
        tin[tin_len] = 0;
        shell_exec(tin);
        tin_len = 0;
        tin[0] = 0;
    } else if (c == '\b') {
        if (tin_len > 0)
            tin[--tin_len] = 0;
    } else if (tin_len < TCOLS - 3) {
        tin[tin_len++] = c;
        tin[tin_len] = 0;
    }
    dirty = 1;
}

/* M4: background worker threads, scheduled cooperatively + preemptively. */
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

    serial_puts("\n[wm] desktop live; shell + finder + viewer + activity\n");

    /* Seed the terminal with the userland boot output + a prompt. */
    term_print("Aqua OS -- userland said:\n");
    term_print(syscall_console());
    term_print("\ntype 'help' for commands.\n");

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
