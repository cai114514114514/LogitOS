#include "paint.h"
#include "layout.h"
#include "fb.h"
#include "text.h"

/* Paint the display list (layout_items) into a scrolled viewport. */
void paint_viewport(int vx, int vy, int vw, int vh, int scroll)
{
    const struct item *it = layout_items();
    int n = layout_count();
    fb_set_clip(vx, vy, vw, vh);
    for (int i = 0; i < n; i++) {
        const struct item *e = &it[i];
        int top = e->y - scroll;                  /* viewport-local top */
        if (top + e->h < 0 || top > vh) continue; /* fully outside */
        int sx = vx + e->x;
        int sy = vy + top;

        if (e->type == IT_RECT) {
            if (e->has_bg) {
                if (e->radius > 0) fb_round_rect(sx, sy, e->w, e->h, e->radius, e->bg);
                else               fb_fill_rect(sx, sy, e->w, e->h, e->bg);
            }
            if (e->border_w > 0) {
                int bw = e->border_w;
                fb_fill_rect(sx, sy, e->w, bw, e->border_color);
                fb_fill_rect(sx, sy + e->h - bw, e->w, bw, e->border_color);
                fb_fill_rect(sx, sy, bw, e->h, e->border_color);
                fb_fill_rect(sx + e->w - bw, sy, bw, e->h, e->border_color);
            }
        } else if (e->type == IT_TEXT) {
            text_draw_run(sx, sy, e->text, e->len, e->font_px, e->mono, e->color);
            if (e->underline) {
                int uy = sy + e->font_px + (e->font_px > 20 ? 3 : 2);
                fb_fill_rect(sx, uy, e->w, 1, e->color);
            }
        } else if (e->type == IT_IMAGE && e->img) {
            fb_blit_rgba(sx, sy, e->w, e->h, e->img->rgba, e->img->w, e->img->h);
        }
    }
    fb_clear_clip();
}

int paint_hittest(int x, int y, int scroll, char *buf, int max)
{
    const struct item *it = layout_items();
    int n = layout_count();
    int dy = y + scroll;                          /* into doc coordinates */
    /* topmost = last painted that contains the point */
    for (int i = n - 1; i >= 0; i--) {
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
