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
#ifdef AD_HALO_TEST
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "openlogit_display.h"
#include "openlogit_display_shadow.h"
#include "openlogit_glass_material.h"
#endif

static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static int inside(const struct ad_rect *a, const struct ad_rect *b)
{ return a->x0 >= b->x0 && a->y0 >= b->y0 && a->x1 <= b->x1 && a->y1 <= b->y1; }

static long total(const struct ad_set *s)
{ long t = 0; for (int i = 0; i < s->n; i++) t += ad_area(&s->r[i]); return t; }

#ifdef AD_HALO_TEST
/* HALO COVERAGE with the REAL primitives. Built with -DGFX_NCTX=2 and no fb.c,
 * so this file defines gfx_ctx_hook. Every pixel a primitive writes (shadow_ex,
 * the gfx stroker) must lie inside the set the halo helpers report, and every
 * backdrop pixel whose change alters a glass panel's output must too. Failures
 * print "FAIL coverage halo ...". */
int (*gfx_ctx_hook)(void);
static int ctx0(void) { return 0; }

#define CW 300
#define CH 260
static uint32_t cbuf[CW * CH], cref[CW * CH];

static int covered(const struct ad_set *s, int x, int y)
{
    for (int i = 0; i < s->n; i++)
        if (x >= s->r[i].x0 && x < s->r[i].x1 && y >= s->r[i].y0 && y < s->r[i].y1) return 1;
    return 0;
}
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }
static void disp_bind(struct ol_display *d, struct ol_pixel_target *t, uint32_t *px)
{
    memset(d, 0, sizeof *d); memset(t, 0, sizeof *t);
    d->allocate = t_alloc; d->release = t_free;
    t->px = px; t->w = CW; t->h = CH;
    if (ol_display_bind(d, t, 16, 8, 0) != OL_OK) { fprintf(stderr, "bind failed\n"); exit(2); }
}
static int rnd(int lo, int hi) { return lo + rand() % (hi - lo + 1); }
static int cdiv_up(int v24) { return v24 >= 0 ? (v24 + 255) >> 8 : -((-v24 + 255) >> 8); } /* away from 0 */

static int count_uncovered(const struct ad_set *s, const uint32_t *a, const uint32_t *b)
{
    int bad = 0;
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++)
            if (a[y * CW + x] != b[y * CW + x] && !covered(s, x, y)) bad++;
    return bad;
}

static void halo_tests(void)
{
    gfx_ctx_hook = ctx0;
    srand(777);
    struct ol_display d; struct ol_pixel_target t;
    disp_bind(&d, &t, cbuf);

    /* shadows: outer and inset, fractional sigma/offset/spread, mixed radii */
    int sh_bad = 0, sh_written = 0;
    for (int trial = 0; trial < 300; trial++) {
        int w = rnd(10, 80), h = rnd(10, 60), x = rnd(100, 150), y = rnd(90, 120);
        int rad[4]; for (int i = 0; i < 4; i++) rad[i] = rnd(0, 14);
        int dx24 = rnd(-12 * 256, 12 * 256), dy24 = rnd(-12 * 256, 12 * 256);
        int sig24 = rnd(16, 14 * 256), spr24 = rnd(-4 * 256, 6 * 256);
        int alpha = rnd(0, 3) ? 255 : rnd(20, 254), inset = rnd(0, 4) == 0;
        memset(cbuf, 0, sizeof cbuf); memset(cref, 0, sizeof cref);
        ol_display_shadow_ex(&d, x, y, w, h, rad, dx24, dy24, sig24, spr24, 0xFFFFFF, alpha, inset);
        int hl[4];
        ad_halo_shadow(cdiv_up(dx24), cdiv_up(dy24), (sig24 + 255) >> 8, cdiv_up(spr24), hl);
        struct ad_set s = {0};
        ad_add_halo(&s, x, y, w, h, hl[0], hl[1], hl[2], hl[3]);
        for (int i = 0; i < CW * CH; i++) if (cbuf[i]) sh_written++;
        sh_bad += count_uncovered(&s, cbuf, cref);
    }
    check(sh_written > 100000, "halo shadow: the property actually drew pixels");
    check(sh_bad == 0, "coverage halo shadow_ex: every written pixel inside the set");

    /* strokes of the real gfx stroker */
    static int spt[8192], ssub[64], opt[8192], osub[64];
    static unsigned char cov[CW * CH];
    int st_bad = 0, st_written = 0;
    for (int trial = 0; trial < 300; trial++) {
        int w = rnd(6, 90), h = rnd(6, 70), x = rnd(60, 140), y = rnd(60, 120);
        int wd = rnd(1, 14), rr = rnd(0, 3) ? rnd(0, 20) : 0;
        struct gfx_path src, out; struct gfx_stroke st;
        gfx_path_init(&src, spt, 8192, ssub, 64);
        gfx_path_init(&out, opt, 8192, osub, 64);
        if (rr) gfx_path_rrect(&src, x << 8, y << 8, w << 8, h << 8, rr << 8);
        else gfx_path_rect(&src, x << 8, y << 8, w << 8, h << 8);
        memset(&st, 0, sizeof st);
        st.width = wd << 8; st.cap = rnd(0, 2); st.join = rnd(0, 2); st.miter_limit = 4 << 16;
        if (!gfx_stroke_path(&out, &src, &st)) { check(0, "stroke refused"); continue; }
        memset(cov, 0, sizeof cov);
        gfx_fill_mask(&out, GFX_NONZERO, cov, CW, CH, 0, 0);
        int hh = ad_halo_stroke(wd);
        struct ad_set s = {0};
        ad_add_halo(&s, x, y, w, h, hh, hh, hh, hh);
        for (int py = 0; py < CH; py++) for (int px = 0; px < CW; px++)
            if (cov[py * CW + px]) { st_written++; if (!covered(&s, px, py)) st_bad++; }
    }
    check(st_written > 50000, "halo stroke: the property actually drew pixels");
    check(st_bad == 0, "coverage halo stroke: every written pixel inside the set");

    /* glass at every tier: the panel's pixels, and any backdrop pixel whose
     * change alters the panel */
    int gl_bad = 0, gl_sensitive = 0;
    for (int tier = 0; tier < 4; tier++)
        for (int trial = 0; trial < 12; trial++) {
            int w = rnd(30, 90), h = rnd(24, 70), x = rnd(110, 150), y = rnd(90, 110);
            int rad = rnd(0, 30);
            struct ol_glass_material mat = { .tint = 0x203040, .opacity = 200, .blur = (uint8_t)tier };
            for (int i = 0; i < CW * CH; i++) cref[i] = (uint32_t)rand() & 0xFFFFFF;
            memcpy(cbuf, cref, sizeof cbuf);
            ol_display_liquid_glass_ex(&d, x, y, w, h, rad, 0, &mat);
            static uint32_t base[CW * CH];
            memcpy(base, cbuf, sizeof base);
            int hh = ad_halo_glass(w, h, rad, tier);
            struct ad_set s = {0};
            ad_add_halo(&s, x, y, w, h, hh, hh, hh, hh);
            gl_bad += count_uncovered(&s, base, cref);          /* written */
            for (int k = 0; k < 14; k++) {                       /* sampled */
                int px, py;
                if (k < 8) { /* straight out from an edge, at and just past the halo */
                    int dd = hh - 1 + (k & 3);
                    px = x + w / 2; py = y + h / 2;
                    if (k & 4) px = (k & 1) ? x - dd - 1 : x + w + dd;
                    else       py = (k & 1) ? y - dd - 1 : y + h + dd;
                } else { px = rnd(x - hh - 12, x + w + hh + 12); py = rnd(y - hh - 12, y + h + hh + 12); }
                if (px < 0 || py < 0 || px >= CW || py >= CH) continue;
                memcpy(cbuf, cref, sizeof cbuf);
                cbuf[py * CW + px] ^= 0xFFFFFF;
                ol_display_liquid_glass_ex(&d, x, y, w, h, rad, 0, &mat);
                int changed = 0;
                for (int i = 0; i < CW * CH; i++)
                    if (i != py * CW + px && cbuf[i] != base[i]) { changed = 1; break; }
                if (changed) { gl_sensitive++; if (!covered(&s, px, py)) gl_bad++; }
            }
        }
    check(gl_sensitive > 20, "halo glass: backdrop changes alter the panel (property is live)");
    check(gl_bad == 0, "coverage halo glass: every written/sampled pixel inside the set");
}
#endif

int main(void)
{
#ifdef AD_HALO_TEST
    halo_tests();
    printf("aui-damage-halo: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
#endif
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
