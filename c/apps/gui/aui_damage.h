#ifndef AUI_DAMAGE_H
#define AUI_DAMAGE_H

/* A frame's damage as a SMALL SET OF RECTANGLES, not one bounding box.
 *
 * aui.c diffs this frame's draw calls against last frame's (section 5a-flush)
 * and tells the compositor what changed. It used to union every change into a
 * single box. A keystroke into TextEdit changes one line of text near the top
 * AND the status strip at the bottom, so the box spanned both -- the whole
 * window -- and the compositor rebuilt all of it. Measured 2026-09-24 at
 * 1920x1200 (make test-repaint, `type`): 78.6% of the SCREEN recomposited per
 * keystroke, 54.6 ms a frame.
 *
 * The compositor already keeps a list of disjoint damage rectangles (wm.c
 * dmg_add) and SYS_GUI_FLUSH_RECT adds one per call, so the fix lives entirely
 * on this side: keep up to AD_NR rectangles, merge any two that touch (the
 * same rule dmg_add uses, so the set stays pairwise disjoint), and when the
 * set is full fold the newcomer into whichever member grows the least.
 * Merging only ever WIDENS, so every changed pixel stays inside some member --
 * the direction that matters: an under-report leaves stale pixels on screen
 * that nothing repaints.
 *
 * Pure and header-only so make test-aui-damage can drive it on the host. */

#define AD_NR 6

struct ad_rect { int x0, y0, x1, y1; };
struct ad_set  { struct ad_rect r[AD_NR]; int n; };

static inline long ad_area(const struct ad_rect *r)
{ return (long)(r->x1 - r->x0) * (r->y1 - r->y0); }

static inline void ad_or(struct ad_rect *a, const struct ad_rect *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

/* Overlapping OR edge-adjacent: two rectangles that share an edge are one
 * rectangle's worth of work and are cheaper flushed as one. */
static inline int ad_touch(const struct ad_rect *a, const struct ad_rect *b)
{ return a->x0 <= b->x1 && b->x0 <= a->x1 && a->y0 <= b->y1 && b->y0 <= a->y1; }

static inline void ad_set_add(struct ad_set *s, int x0, int y0, int x1, int y1)
{
    struct ad_rect n = { x0, y0, x1, y1 };
    if (x1 <= x0 || y1 <= y0) return;
#ifdef AUI_FLUSH_SINGLE_RECT
    /* THE CONTROL: the old single bounding box. make test-aui-damage-negctl
     * builds with it and requires the split cases to fail. */
    if (s->n) { ad_or(&s->r[0], &n); return; }
    s->r[s->n++] = n;
    return;
#endif
    for (;;) {
        int merged = 0;
        for (int i = 0; i < s->n; i++)
            if (ad_touch(&n, &s->r[i])) {
                ad_or(&n, &s->r[i]);
                s->r[i] = s->r[--s->n];
                merged = 1;
                break;
            }
        if (merged) continue;                 /* the grown one may touch another */
        if (s->n < AD_NR) break;
        int best = 0; long bestcost = -1;
        for (int i = 0; i < s->n; i++) {
            struct ad_rect u = n; ad_or(&u, &s->r[i]);
            long cost = ad_area(&u) - ad_area(&n) - ad_area(&s->r[i]);
            if (bestcost < 0 || cost < bestcost) { bestcost = cost; best = i; }
        }
        ad_or(&n, &s->r[best]);
        s->r[best] = s->r[--s->n];
    }
    s->r[s->n++] = n;
}

/* ------------------------------------------------------------------ halos --
 * A primitive can write pixels OUTSIDE its logical rect (a shadow's offset,
 * spread and blur; a stroke's half width; a glass panel's backdrop reach).
 * Reporting only the logical rect under-reports, and the stale halo pixels
 * stay on screen. ad_add_halo() adds the rect grown by a per-side margin; the
 * ad_halo_*() helpers compute that margin as out[4] = { left, top, right,
 * bottom } in DEVICE pixels, always rounding UP. Every helper is an upper
 * bound of what the real primitive touches (tests/unit/aui_damage_test.c,
 * make test-aui-damage-halo, draws the real ones and checks each pixel).
 *
 * AD_HALO_NEGCTL_ZERO is the control: every halo is 0, so only the logical
 * rect is reported; make test-aui-damage-halo-negctl requires the coverage
 * property to fail. */

static inline void ad_add_halo(struct ad_set *s, int x, int y, int w, int h,
                               int left, int top, int right, int bottom)
{
    if (w <= 0 || h <= 0) return;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right < 0) right = 0;
    if (bottom < 0) bottom = 0;
#ifdef AD_HALO_NEGCTL_ZERO
    left = top = right = bottom = 0;
#endif
    ad_set_add(s, x - left, y - top, x + w + right, y + h + bottom);
}

/* ol_display_shadow_ex: the gaussian is cut where alpha*(1-cdf) < 0.5 level,
 * at most ~2.9 sigma (alpha 255) plus 2 px of its own rounding (reach_px in
 * display_shadow_ex.c), measured from the offset, grown shape. dx,dy are the
 * offset in px (round AWAY from zero), blur the sigma in px (round up), spread
 * the growth in px (a negative spread shrinks the shape and adds nothing). */
static inline void ad_halo_shadow(int dx, int dy, int blur, int spread, int out[4])
{
    int reach = 3 * (blur < 0 ? 0 : blur) + 2 + (spread > 0 ? spread : 0);
#ifdef AD_HALO_NEGCTL_ZERO
    reach = 0; dx = dy = 0;
#endif
    out[0] = reach + (dx < 0 ? -dx : 0);
    out[1] = reach + (dy < 0 ? -dy : 0);
    out[2] = reach + (dx > 0 ? dx : 0);
    out[3] = reach + (dy > 0 ? dy : 0);
}

/* A stroke is centred on its path: half the width falls outside the path's
 * rect on every side (miter, round and square-cap extents of an axis-aligned
 * rect or rounded rect stay within the same half width). Width in px. */
static inline int ad_halo_stroke(int width)
{
#ifdef AD_HALO_NEGCTL_ZERO
    (void)width; return 0;
#else
    return width > 0 ? (width + 1) / 2 : 0;
#endif
}

/* Glass: mirrors ol_display_glass_sample_halo_ex (openlogit_material.c). The
 * panel itself only writes inside its rect; the halo is how far the backdrop
 * it SAMPLES reaches, so damage in the scene behind it must redraw the panel.
 * tier is OL_GLASS_BLUR_*: 0 REGULAR, 1 THIN, 2 THICK, 3 ULTRA (anything else
 * is REGULAR). Blur support is the sum of the three pass radii. */
static inline int ad_halo_glass(int w, int h, int radius, int tier)
{
#ifdef AD_HALO_NEGCTL_ZERO
    (void)w; (void)h; (void)radius; (void)tier; return 0;
#else
    static const int support[4] = { 6, 2 + 2 + 3, 11 * 3, 18 + 18 + 19 };
    if (w <= 0 || h <= 0 || radius < 0) return 0;
    int side = w < h ? w : h;
    if (radius > side / 2) radius = side / 2;
    int edge = radius < 22 ? radius : 22;
    if (edge > side / 2) edge = side / 2;
    if (edge < 4) edge = 4;
    int disp = (edge < 18 ? edge : 18) + 1;
    if (disp < 6) disp = 6;
    return support[tier >= 0 && tier <= 3 ? tier : 0] + disp;
#endif
}

#endif
