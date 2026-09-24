/* aui's per-frame damage set (c/apps/gui/aui_damage.h), on the host.
 *
 * Two kinds of claim, and they fail in opposite directions:
 *
 *   COVERAGE -- every rectangle handed in lies wholly inside some member of
 *   the set. Breaking this is the dangerous failure: an under-report leaves
 *   stale pixels on the screen and nothing repaints them. Checked over
 *   randomised inputs, so it is a property, not an example.
 *
 *   TIGHTNESS -- two changes far apart stay two rectangles. This is the
 *   reason the set exists (a TextEdit keystroke plus its status strip used to
 *   flush the whole window). The control, -DAUI_FLUSH_SINGLE_RECT, restores
 *   the old single bounding box; coverage still holds under it, and the
 *   tightness checks must fail. make test-aui-damage-negctl requires that. */
#include <stdio.h>
#include <stdlib.h>
#include "aui_damage.h"

static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static int inside(const struct ad_rect *a, const struct ad_rect *b)
{ return a->x0 >= b->x0 && a->y0 >= b->y0 && a->x1 <= b->x1 && a->y1 <= b->y1; }

static long total(const struct ad_set *s)
{ long t = 0; for (int i = 0; i < s->n; i++) t += ad_area(&s->r[i]); return t; }

int main(void)
{
    /* 1. TextEdit's keystroke: a text line near the top, the status strip at
     *    the bottom of a 1650x1050 canvas. Two rectangles, not one box. */
    struct ad_set s = {0};
    ad_set_add(&s, 20, 40, 1650, 64);          /* the edited line   */
    ad_set_add(&s, 0, 1020, 1650, 1050);       /* the status strip  */
    check(s.n == 2, "keystroke: line + status stay two rectangles");
    check(total(&s) == 1630L * 24 + 1650L * 30, "keystroke: area is the two strips, not the span");

    /* 2. Touching rectangles are one. */
    struct ad_set t = {0};
    ad_set_add(&t, 0, 0, 100, 10);
    ad_set_add(&t, 0, 10, 100, 20);
    check(t.n == 1 && ad_area(&t.r[0]) == 2000, "adjacent strips merge into one");

    /* 3. A caret blink and a spinner, far apart: two small boxes whose
     *    bounding box would be ~80x larger. */
    struct ad_set u = {0};
    ad_set_add(&u, 100, 100, 102, 120);
    ad_set_add(&u, 1500, 900, 1520, 920);
    check(u.n == 2 && total(&u) == 40 + 400, "two small far-apart changes stay small");

    /* 4. COVERAGE, randomised: every input inside some member; members
     *    pairwise disjoint (not even touching); never more than AD_NR. */
    srand(12345);
    int cover_bad = 0, disjoint_bad = 0, count_bad = 0;
    for (int trial = 0; trial < 20000; trial++) {
        struct ad_set r = {0};
        struct ad_rect in[48];
        int n = 1 + rand() % 48;
        for (int i = 0; i < n; i++) {
            int x = rand() % 1900, y = rand() % 1180;
            int w = 1 + rand() % 300, h = 1 + rand() % 120;
            in[i] = (struct ad_rect){ x, y, x + w, y + h };
            ad_set_add(&r, x, y, x + w, y + h);
        }
        if (r.n > AD_NR) count_bad++;
        for (int i = 0; i < n; i++) {
            int ok = 0;
            for (int j = 0; j < r.n; j++) if (inside(&in[i], &r.r[j])) { ok = 1; break; }
            if (!ok) cover_bad++;
        }
        for (int i = 0; i < r.n; i++)
            for (int j = i + 1; j < r.n; j++)
                if (ad_touch(&r.r[i], &r.r[j])) disjoint_bad++;
    }
    check(cover_bad == 0, "coverage: every input rectangle inside some member (20000 trials)");
    check(disjoint_bad == 0, "members pairwise disjoint");
    check(count_bad == 0, "never more than AD_NR members");

    printf("aui-damage: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
