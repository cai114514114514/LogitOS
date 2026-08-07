/* Ring-3 paint (M17 L1): walk the layout display list and draw it with the GUI
 * render syscalls. Mirrors the old kernel net/paint.c, fb_* -> gui_*. */
#include "logit.h"
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
        if (e->hidden) continue;                  /* visibility:hidden / opacity:0 */
        if (top + e->h < 0 || top > vh) continue; /* fully outside */
        int sx = vx + e->x;
        int sy = vy + top;

        if (e->type == IT_RECT) {
            int bmax = e->border_w[0];
            for (int k = 1; k < 4; k++) if (e->border_w[k] > bmax) bmax = e->border_w[k];
            int r = e->radius_pct ? (e->w < e->h ? e->w : e->h) * e->radius_pct / 100 : e->radius;
            int maxr = (e->w < e->h ? e->w : e->h) / 2; if (r > maxr) r = maxr;
            if (r > 0 && e->has_bg) {
                /* rounded box: border ring (uniform color/width approximation) +
                 * rounded fill inset by the widest edge */
                if (bmax > 0) {
                    gui_rrect(sx, sy, e->w, e->h, r, e->border_color[0]);
                    gui_rrect(sx + bmax, sy + bmax, e->w - 2*bmax, e->h - 2*bmax,
                              r > bmax ? r - bmax : 0, e->bg);
                } else {
                    gui_rrect(sx, sy, e->w, e->h, r, e->bg);
                }
            } else {
                if (e->has_bg) gui_rect(sx, sy, e->w, e->h, e->bg);
                if (e->border_w[0] > 0) gui_rect(sx, sy, e->w, e->border_w[0], e->border_color[0]);
                if (e->border_w[2] > 0) gui_rect(sx, sy + e->h - e->border_w[2], e->w, e->border_w[2], e->border_color[2]);
                if (e->border_w[3] > 0) gui_rect(sx, sy, e->border_w[3], e->h, e->border_color[3]);
                if (e->border_w[1] > 0) gui_rect(sx + e->w - e->border_w[1], sy, e->border_w[1], e->h, e->border_color[1]);
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

static int item_hit(const struct item *e, int x, int dy)
{ return x >= e->x && x < e->x + e->w && dy >= e->y && dy < e->y + e->h; }

int browser_hittest(int x, int y, int scroll, char *buf, int max)
{
    if (max <= 0) return 0;
    const struct item *it = layout_items();
    int n = layout_count();
    int dy = y + scroll;                          /* into doc coordinates */
    for (int i = n - 1; i >= 0; i--) {            /* topmost first */
        const struct item *e = &it[i];
        if (!e->href || e->hidden) continue;
        if (item_hit(e, x, dy)) {
            int o = 0;
            while (e->href[o] && o < max - 1) { buf[o] = e->href[o]; o++; }
            buf[o] = 0;
            return 1;
        }
    }
    return 0;
}

int browser_hittest_node(int x, int y, int scroll, struct node **node, char *href, int max)
{
    if (node) *node = 0;
    if (href && max > 0) href[0] = 0;
    const struct item *it = layout_items();
    int n = layout_count();
    int dy = y + scroll;
    /* Back to front. The display list is emitted parent-before-child (a block's
     * background rect, then its text), so the LAST box covering the point is the
     * innermost one -- which is the element a DOM event should target. */
    for (int i = n - 1; i >= 0; i--) {
        const struct item *e = &it[i];
        if (e->hidden || !item_hit(e, x, dy)) continue;
        if (node) {
            /* Text boxes hang off the TEXT node; an event target must be an
             * element, so climb until we find one. */
            struct node *p = e->node;
            while (p && p->type != N_ELEM && p->type != N_DOCUMENT) p = p->parent;
            *node = p;
        }
        if (href && max > 0 && e->href) {
            int o = 0;
            while (e->href[o] && o < max - 1) { href[o] = e->href[o]; o++; }
            href[o] = 0;
        }
        /* An inner box without its own href still navigates its ancestor link
         * (layout propagates the href down the inline flow, but a background
         * rect emitted for a nested element may carry none) -- so if this box
         * had no href, fall back to the link hit test over the same point. */
        if (href && max > 0 && !href[0]) browser_hittest(x, y, scroll, href, max);
        return 1;
    }
    return 0;
}
