/* Ring-3 paint (M17 L1): walk the layout display list and draw it with the GUI
 * render syscalls. Mirrors the old kernel net/paint.c, fb_* -> gui_*. */
#include "aether.h"
#include "layout.h"
#include "browser_paint.h"

void browser_paint(int vx, int vy, int vw, int vh, int scroll)
{
    const struct item *it = layout_items();
    int n = layout_count();
    gui_clip(vx, vy, vw, vh);
    uint32_t pbg;
    if (layout_page_bg(&pbg)) gui_rect(vx, vy, vw, vh, pbg);  /* themed background */
    for (int i = 0; i < n; i++) {
        const struct item *e = &it[i];
        int top = e->y - scroll;                  /* viewport-local top */
        if (top + e->h < 0 || top > vh) continue; /* fully outside */
        int sx = vx + e->x;
        int sy = vy + top;

        if (e->type == IT_RECT) {
            if (e->has_bg) gui_rect(sx, sy, e->w, e->h, e->bg);
            if (e->border_w > 0) {
                int bw = e->border_w;
                gui_rect(sx, sy, e->w, bw, e->border_color);
                gui_rect(sx, sy + e->h - bw, e->w, bw, e->border_color);
                gui_rect(sx, sy, bw, e->h, e->border_color);
                gui_rect(sx + e->w - bw, sy, bw, e->h, e->border_color);
            }
        } else if (e->type == IT_TEXT) {
            gui_text_run(sx, sy, e->font_px, e->mono, e->color, e->text, e->len);
            if (e->underline) {
                int uy = sy + e->font_px + (e->font_px > 20 ? 3 : 2);
                gui_rect(sx, uy, e->w, 1, e->color);
            }
        } else if (e->type == IT_IMAGE && e->img) {
            gui_blit(sx, sy, e->w, e->h, e->img->rgba, e->img->w, e->img->h);
        }
    }
    gui_clip(0, 0, 0, 0);
}

int browser_hittest(int x, int y, int scroll, char *buf, int max)
{
    if (max <= 0) return 0;
    const struct item *it = layout_items();
    int n = layout_count();
    int dy = y + scroll;                          /* into doc coordinates */
    for (int i = n - 1; i >= 0; i--) {            /* topmost first */
        const struct item *e = &it[i];
        if (!e->href) continue;
        if (x >= e->x && x < e->x + e->w && dy >= e->y && dy < e->y + e->h) {
            int o = 0;
            while (e->href[o] && o < max - 1) { buf[o] = e->href[o]; o++; }
            buf[o] = 0;
            return 1;
        }
    }
    return 0;
}
