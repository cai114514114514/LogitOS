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

#endif
