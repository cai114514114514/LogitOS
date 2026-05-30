#include <stdint.h>
#include <stddef.h>
#include "wm.h"
#include "fb.h"
#include "pmm.h"
#include "pit.h"
#include "serial.h"

#define NWIN     2
#define MENUBAR_H 24
#define TITLEBAR_H 30

struct window {
    int x, y, w, h;
    const char *title;
};

static struct window win[NWIN];
static int order[NWIN];          /* order[NWIN-1] is topmost / focused */
static int mx, my, mleft;
static int dragging = -1, drag_dx, drag_dy;
static volatile int dirty = 1;

static uint32_t *back;           /* the framebuffer back buffer */
static uint32_t *bg;             /* cached static background */
static int W, H;

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return fb_rgb(r, g, b); }
static int lerp(int a, int b, int n, int d) { return a + (b - a) * n / d; }

static void blit(uint32_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++)
        dst[i] = src[i];
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

    /* menu bar */
    fb_blend_rect(0, 0, W, MENUBAR_H, 245, 246, 250, 185);
    uint32_t ink = rgb(40, 40, 46);
    fb_fill_circle(16, MENUBAR_H / 2, 6, ink);          /* logo */
    fb_text(32, 4, "Aqua OS", ink);
    fb_text(112, 4, "File", ink);
    fb_text(156, 4, "Edit", ink);
    fb_text(200, 4, "View", ink);

    /* dock */
    int dn = 7, isz = 50, gap = 14;
    int dw = gap + dn * (isz + gap);
    int dh = isz + 20;
    int dx = (W - dw) / 2, dy = H - dh - 12;
    fb_blend_round_rect(dx, dy, dw, dh, 22, 255, 255, 255, 95);
    uint32_t icons[7] = {
        rgb(80, 140, 255),  rgb(55, 200, 120),  rgb(255, 92, 92),
        rgb(255, 170, 40),  rgb(170, 110, 255), rgb(40, 200, 220),
        rgb(255, 120, 170),
    };
    for (int i = 0; i < dn; i++) {
        int ix = dx + gap + i * (isz + gap);
        int iy = dy + 10;
        fb_round_rect(ix, iy, isz, isz, 12, icons[i]);
        fb_blend_round_rect(ix, iy, isz, isz / 2, 12, 255, 255, 255, 40);
    }
}

static void draw_clock(void)
{
    uint64_t t = timer_ticks() / 100;        /* seconds since boot */
    int mm = (int)((t / 60) % 60), ss = (int)(t % 60);
    char buf[6] = { '0' + mm / 10, '0' + mm % 10, ':', '0' + ss / 10, '0' + ss % 10, 0 };
    fb_text(W - 48, 4, buf, rgb(40, 40, 46));
}

static void draw_window(struct window *w, int focused)
{
    int x = w->x, y = w->y, ww = w->w, wh = w->h;

    fb_blend_round_rect(x - 5, y + 8, ww + 10, wh + 10, 18, 0, 0, 0,
                        focused ? 60 : 35);                 /* shadow */
    fb_round_rect(x, y, ww, wh, 10, rgb(252, 252, 253));    /* body */

    uint32_t tb = focused ? rgb(235, 235, 240) : rgb(245, 245, 248);
    fb_round_rect(x, y, ww, TITLEBAR_H, 10, tb);            /* title bar */
    fb_fill_rect(x, y + 20, ww, TITLEBAR_H - 20, tb);
    fb_fill_rect(x, y + TITLEBAR_H, ww, 1, rgb(214, 214, 220));

    uint32_t off = rgb(205, 205, 210);
    fb_fill_circle(x + 16, y + 15, 6, focused ? rgb(255, 95, 86) : off);
    fb_fill_circle(x + 34, y + 15, 6, focused ? rgb(254, 188, 46) : off);
    fb_fill_circle(x + 52, y + 15, 6, focused ? rgb(40, 200, 64) : off);

    int tw = fb_text_width(w->title);
    fb_text(x + (ww - tw) / 2, y + 7, w->title, rgb(70, 70, 78));

    fb_text(x + 20, y + 48, "Drag me by the title bar.", rgb(90, 90, 98));
    for (int i = 0; i < 4; i++)
        fb_round_rect(x + 20, y + 80 + i * 22, ww - 40, 10, 5, rgb(228, 229, 234));
}

/* North-west pointer: '#' outline, '.' white fill, ' ' transparent. */
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

    win[0] = (struct window){ 170, 140, 440, 320, "Finder" };
    win[1] = (struct window){ 470, 250, 380, 250, "Aqua Notes" };
    order[0] = 0;
    order[1] = 1;

    draw_background();           /* render once into back ... */
    blit(bg, back, count);       /* ... and cache it */
}

void wm_render(void)
{
    blit(back, bg, W * H);       /* restore static background */
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

    if (left && !mleft) {                          /* button press */
        for (int i = NWIN - 1; i >= 0; i--) {
            struct window *w = &win[order[i]];
            if (!hit(w, x, y))
                continue;
            int id = order[i];                     /* raise to top */
            for (int j = i; j < NWIN - 1; j++)
                order[j] = order[j + 1];
            order[NWIN - 1] = id;
            if (y < w->y + TITLEBAR_H) {            /* grab title bar */
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

void wm_run(void)
{
    /* Re-point the data selectors at the kernel descriptor (we arrived here
     * via an iret from the userland exit, which left them as user data). */
    __asm__ volatile ("mov $0x10, %%ax\n\t"
                      "mov %%ax, %%ds\n\tmov %%ax, %%es\n\t"
                      "mov %%ax, %%fs\n\tmov %%ax, %%gs" ::: "ax");

    serial_puts("\n[wm] desktop is live -- move the mouse, drag a window\n");

    uint64_t last = 0;
    for (;;) {
        uint64_t now = timer_ticks();
        if (dirty || now - last >= 25) {           /* event or ~0.25s tick */
            dirty = 0;
            last = now;
            wm_render();
        }
        __asm__ volatile ("hlt");
    }
}
