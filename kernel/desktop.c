#include <stdint.h>
#include "desktop.h"
#include "fb.h"

static int lerp(int a, int b, int num, int den)
{
    return a + (b - a) * num / den;
}

void desktop_draw(void)
{
    int W = (int)fb_width();
    int H = (int)fb_height();

    /* --- Wallpaper: vertical gradient, deep indigo -> sky blue --- */
    for (int y = 0; y < H; y++) {
        uint32_t c = fb_rgb((uint8_t)lerp(22, 96, y, H),
                            (uint8_t)lerp(44, 165, y, H),
                            (uint8_t)lerp(120, 230, y, H));
        for (int x = 0; x < W; x++)
            fb_put(x, y, c);
    }

    /* --- Menu bar: frosted translucent strip --- */
    fb_blend_rect(0, 0, W, 26, 245, 246, 250, 175);
    fb_fill_circle(20, 13, 6, fb_rgb(35, 35, 40));              /* logo */

    int bx = W - 66;                                            /* battery */
    fb_round_rect(bx, 8, 26, 11, 3, fb_rgb(40, 40, 46));
    fb_round_rect(bx + 2, 10, 18, 7, 2, fb_rgb(70, 210, 110));
    fb_fill_rect(bx + 26, 11, 2, 5, fb_rgb(40, 40, 46));
    fb_fill_circle(W - 86, 13, 3, fb_rgb(40, 40, 46));          /* status dots */
    fb_fill_circle(W - 100, 13, 3, fb_rgb(40, 40, 46));

    /* --- A window with a soft drop shadow --- */
    int wx = 232, wy = 150, ww = 560, wh = 380;
    fb_blend_round_rect(wx - 6, wy + 12, ww + 12, wh + 12, 22, 0, 0, 0, 45);
    fb_round_rect(wx, wy, ww, wh, 12, fb_rgb(252, 252, 253));

    /* title bar: rounded top, squared bottom + separator */
    fb_round_rect(wx, wy, ww, 38, 12, fb_rgb(238, 238, 242));
    fb_fill_rect(wx, wy + 26, ww, 12, fb_rgb(238, 238, 242));
    fb_fill_rect(wx, wy + 38, ww, 1, fb_rgb(214, 214, 220));

    /* traffic-light controls */
    fb_fill_circle(wx + 20, wy + 19, 7, fb_rgb(255, 95, 86));
    fb_fill_circle(wx + 42, wy + 19, 7, fb_rgb(254, 188, 46));
    fb_fill_circle(wx + 64, wy + 19, 7, fb_rgb(40, 200, 64));

    /* Finder-style sidebar */
    fb_fill_rect(wx + 1, wy + 39, 150, wh - 40, fb_rgb(245, 245, 248));
    fb_fill_rect(wx + 151, wy + 39, 1, wh - 40, fb_rgb(220, 220, 226));
    for (int i = 0; i < 6; i++) {
        fb_fill_circle(wx + 22, wy + 64 + i * 30, 6, fb_rgb(120, 165, 235));
        fb_round_rect(wx + 38, wy + 59 + i * 30, 90, 10, 5, fb_rgb(205, 208, 215));
    }
    /* main content skeleton */
    for (int i = 0; i < 8; i++)
        fb_round_rect(wx + 172, wy + 58 + i * 36, 360, 14, 7, fb_rgb(228, 229, 234));

    /* --- Dock: frosted glass with colorful rounded-square icons --- */
    int dn = 7, isz = 50, gap = 14;
    int dw = gap + dn * (isz + gap);
    int dh = isz + 20;
    int dx = (W - dw) / 2, dy = H - dh - 14;
    fb_blend_round_rect(dx, dy, dw, dh, 22, 255, 255, 255, 95);

    uint32_t icons[7] = {
        fb_rgb(80, 140, 255),  fb_rgb(55, 200, 120),  fb_rgb(255, 92, 92),
        fb_rgb(255, 170, 40),  fb_rgb(170, 110, 255), fb_rgb(40, 200, 220),
        fb_rgb(255, 120, 170),
    };
    for (int i = 0; i < dn; i++) {
        int ix = dx + gap + i * (isz + gap);
        int iy = dy + 10;
        fb_round_rect(ix, iy, isz, isz, 12, icons[i]);
        fb_blend_round_rect(ix, iy, isz, isz / 2, 12, 255, 255, 255, 40);  /* gloss */
    }
}
