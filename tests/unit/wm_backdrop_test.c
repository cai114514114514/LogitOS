/* Host test of the backdrop regions (c/kernel/gui/wm_backdrop.c) against a
 * fake desktop and a fake window: SYS_GUI_BACKDROP through its handler, the
 * compositor's two calls (wm_backdrop_composite, wm_backdrop_blit_content),
 * the damage, cover and band queries, and the colour mask wm.c applies.
 *
 * Scene: a striped desktop (back buffer) and a window canvas at (WX, WY) with
 * one sidebar region {0, r, r, 0} down its left side, app "text" (an AA
 * coverage bitmap) drawn inside the sidebar and an app rect outside it.
 *
 *   desktop-sample  (a) the region interior equals ol_display_liquid_glass_ex
 *                   run over the desktop alone, within 2/255, and the desktop
 *                   under it has variance (the sample is real)
 *   text-ink        (b) inside the region, every pixel the app drew is the
 *                   app's pixel, untouched by the material
 *   plain-blit      (c) outside the regions the canvas is the plain 1:1 blit,
 *                   and nothing outside the window or the region changed
 *   reduce-solid    (d) Reduce Transparency: marked pixels are the material's
 *                   solid colour, and the backdrop is not read
 *   move-damage     (e) damage after a move contains the old and new region
 *                   plus the material's sample halo
 *   cover-region    (f) wm_cover refuses a rect that touches a region
 *   mark-forgery    (g) an app colour 0x01FF0000 through wm_app_rgb (CLEAR /
 *                   RECT) stores no MARK pixel
 *   validate        argument checks, <= 8 regions per window, CLEAR, reset
 *   hidpi           125% and 150%: region device rect, mark coverage, and
 *                   (a)-(c) again
 *   nctx            -DGFX_NCTX=2: two drawing contexts interleaved give the
 *                   pixels of two serial runs
 *
 * THE CONTROL (-DWM_BACKDROP_SELF_ONLY) frosts the app's own pixels instead of
 * the desktop; it must fail desktop-sample and nothing else. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "wm_cover.h"
#include "openlogit_glass_material.h"

int (*gfx_ctx_hook)(void);
static int g_ctx;
static int ctx_of(void) { return g_ctx; }

enum { BW = 720, BH = 520, WX = 80, WY = 60, SWMAX = 500, SHMAX = 360 };
static uint32_t desk[BW * BH], back[BW * BH], ref[BW * BH], back2[BW * BH];
static uint32_t surf[SWMAX * SHMAX];
static int SW, SH;                        /* this scene's canvas, device px */

static int fails;
static void verdict(int ok, const char *name, const char *fmt, ...)
{
    va_list ap;
    printf("%s %s", ok ? "PASS" : "FAIL", name);
    if (fmt) { printf(" "); va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
    printf("\n");
    if (!ok) fails++;
}

static int copyin(void *d, const void *s, unsigned long n) { memcpy(d, s, n); return 0; }

static void bind(struct ol_display *d, uint32_t *px, int w, int h)
{
    memset(d, 0, sizeof *d);
    d->allocate = malloc;
    d->release = free;
    struct ol_pixel_target t = { .px = px, .w = w, .h = h };
    if (ol_display_bind(d, &t, 16, 8, 0) != OL_OK) { printf("FAIL harness bind\n"); exit(2); }
}

static int ch(uint32_t p, int k) { return (int)(p >> (16 - 8 * k)) & 255; }
static int pxdiff(uint32_t a, uint32_t b)
{
    int m = 0;
    for (int k = 0; k < 3; k++) { int d = abs(ch(a, k) - ch(b, k)); if (d > m) m = d; }
    return m;
}
static double luma(uint32_t p) { return 0.299 * ch(p, 0) + 0.587 * ch(p, 1) + 0.114 * ch(p, 2); }

static void make_desktop(void)
{
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++) {
            int band = ((x + 2 * y) / 18) & 1;
            uint32_t r = band ? 210 : 30, g = band ? 80 + (y % 60) : 140, b = band ? 40 : 200 + (x % 40);
            desk[y * BW + x] = (r << 16) | (g << 8) | b;
        }
}

/* The scene's env: a canvas of the given device size bound like fb_target. */
static struct ol_display sd;
static void env_init(struct wm2_env *e, int slot, int pct, int sw, int sh)
{
    SW = sw; SH = sh;
    bind(&sd, surf, SW, SH);
    memset(e, 0, sizeof *e);
    e->d = &sd;
    e->scale_pct = pct;
    e->surf_w = SW;
    e->surf_h = SH;
    e->slot = slot;
    e->copyin = copyin;
}

static const uint32_t APP_BG = 0xF4F4F6, FILL = 0xE6E6EC, INK = 0x141418, APP_RECT = 0x3070C0;

static void sidebar(struct logit_backdrop2 *bd, int pct, int id, int blur)
{
    memset(bd, 0, sizeof *bd);
    bd->size = sizeof *bd;
    bd->id = id;
    bd->x = 0; bd->y = 0;
    bd->w = 120 << 8;
    bd->h = (SH * 100 / pct) << 8;          /* the full canvas height in points */
    bd->radius[1] = bd->radius[2] = 12 << 8; /* {0, 12, 12, 0}: square left edge */
    bd->tint_argb = 0xF0F2F8;
    bd->opacity = 96;
    bd->blur_level = blur;
    bd->fill_rgb = FILL;
}

/* App text inside the sidebar: an AA coverage pattern of 0 / 96 / 255. */
static uint8_t glyph[14 * 60];
static void draw_app(int pct)
{
    ol_display_fill_rect(&sd, wm2_pt(200, pct), wm2_pt(40, pct), wm2_pt(60, pct), wm2_pt(30, pct),
                         wm_app_rgb(APP_RECT));
    for (int j = 0; j < 14; j++)
        for (int i = 0; i < 60; i++) {
            int on = ((i / 3) % 3 != 0) && (j > 1 && j < 12) && ((i + j) % 7 != 0);
            glyph[j * 60 + i] = on ? ((i % 3 == 1) ? 96 : 255) : 0;
        }
    ol_display_blit_glyph(&sd, wm2_pt(16, pct), wm2_pt(30, pct), glyph, 60, 14, wm_app_rgb(INK));
}

/* What the compositor does for the window: material, then the ink blit. */
static void composite(uint32_t *dst, int slot, const struct drect *R, int reduce, struct ol_display *bd)
{
    memcpy(dst, desk, sizeof desk);
    bind(bd, dst, BW, BH);
    bd->reduce_transparency = reduce;
    bd->target.clip_on = 1;
    bd->target.clx0 = R->x0; bd->target.cly0 = R->y0;
    bd->target.clx1 = R->x1; bd->target.cly1 = R->y1;
    wm_backdrop_composite(R, slot, WX, WY, bd);
    struct ol_pixel_target src = { .px = surf, .w = SW, .h = SH };
    wm_backdrop_blit_content(R, slot, WX, WY, bd, &src);
}

/* The oracle: ol_display_liquid_glass_ex over the desktop alone, at the
 * region's device rect, radius and cut as the documented rule derives them. */
static void oracle(int pct, const struct logit_backdrop2 *bd, int reduce, struct drect *dev_out)
{
    struct ol_display od;
    memcpy(ref, desk, sizeof desk);
    bind(&od, ref, BW, BH);
    od.reduce_transparency = reduce;
    int x0 = wm2_sat(wm2_floordiv(wm2_pt24(bd->x, pct) + 128, 256));
    int y0 = wm2_sat(wm2_floordiv(wm2_pt24(bd->y, pct) + 128, 256));
    int x1 = wm2_sat(wm2_floordiv(wm2_pt24(bd->x + bd->w, pct) + 128, 256));
    int y1 = wm2_sat(wm2_floordiv(wm2_pt24(bd->y + bd->h, pct) + 128, 256));
    if (x1 > SW) x1 = SW;
    if (y1 > SH) y1 = SH;
    int rad = wm2_sat(wm2_floordiv(wm2_pt24(bd->radius[1], pct) + 128, 256));
    int s = (x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0);
    if (rad > s / 2) rad = s / 2;
    struct ol_glass_material m = { .tint = bd->tint_argb & 0xFFFFFF, .opacity = (uint8_t)bd->opacity,
                                   .blur = (uint8_t)bd->blur_level, .rim = (uint8_t)bd->rim };
    ol_display_liquid_glass_ex(&od, WX + x0, WY + y0, x1 - x0, y1 - y0, rad, OL_GLASS_CUT_LEFT, &m);
    ol_display_destroy(&od);
    dev_out->x0 = x0; dev_out->y0 = y0; dev_out->x1 = x1; dev_out->y1 = y1;
}

static int marked(int x, int y) { return (surf[y * SW + x] >> 24) == LOGIT_BACKDROP_MARK; }

/* (a) (b) (c) for one scene at one scale. */
static void scene_checks(int pct, int blur, const char *tag)
{
    struct wm2_env e;
    env_init(&e, 3, pct, wm2_pt(320, pct), wm2_pt(220, pct));
    wm2_backdrop_win_reset(3);
    ol_display_clear(&sd, wm_app_rgb(APP_BG));
    struct logit_backdrop2 bd;
    sidebar(&bd, pct, 0, blur);
    long rc = wm2_backdrop(&e, (long)&bd, 0, 0);
    if (rc != OL_OK) { verdict(0, "desktop-sample", "%s: SYS_GUI_BACKDROP returned %ld", tag, rc); return; }
    draw_app(pct);

    struct drect R = { 0, 0, BW, BH }, dev;
    struct ol_display bdsp;
    composite(back, 3, &R, 0, &bdsp);
    oracle(pct, &bd, 0, &dev);

    /* (a) */
    long nmark = 0, worst = 0, bad = 0;
    double sum = 0, sq = 0, dsum = 0, dsq = 0;
    for (int y = dev.y0; y < dev.y1; y++)
        for (int x = dev.x0; x < dev.x1; x++) {
            if (!marked(x, y)) continue;
            uint32_t got = back[(WY + y) * BW + WX + x], want = ref[(WY + y) * BW + WX + x];
            int d = pxdiff(got, want);
            if (d > worst) worst = d;
            if (d > 2) bad++;
            double l = luma(got), dl = luma(desk[(WY + y) * BW + WX + x]);
            sum += l; sq += l * l; dsum += dl; dsq += dl * dl;
            nmark++;
        }
    double var = nmark ? sq / nmark - (sum / nmark) * (sum / nmark) : 0;
    double dvar = nmark ? dsq / nmark - (dsum / nmark) * (dsum / nmark) : 0;
    verdict(nmark > 0 && bad == 0 && var > 1.0 && dvar > 1.0, "desktop-sample",
            "%s: %ld marked px, max delta %ld/255 (tol 2), %ld over; glass luma variance %.1f, desktop %.1f",
            tag, nmark, worst, bad, var, dvar);

    /* (b): every unmarked pixel inside the region rect is the app's own */
    long ink = 0, inkbad = 0;
    for (int y = dev.y0; y < dev.y1; y++)
        for (int x = dev.x0; x < dev.x1; x++) {
            if (marked(x, y)) continue;
            uint32_t got = back[(WY + y) * BW + WX + x], app = surf[y * SW + x];
            int cov_ink = (app & 0xFFFFFF) != APP_BG;
            ink += cov_ink;
            if (got != app) inkbad++;
        }
    verdict(ink > 100 && inkbad == 0, "text-ink", "%s: %ld ink px in the region, %ld not the app's pixel", tag, ink, inkbad);

    /* (c): outside every region the plain 1:1 blit; outside the window and
     * outside the region's halo, the desktop untouched */
    long out = 0, outbad = 0, deskbad = 0;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            if (x >= dev.x0 && x < dev.x1 && y >= dev.y0 && y < dev.y1) continue;
            out++;
            if (back[(WY + y) * BW + WX + x] != surf[y * SW + x]) outbad++;
        }
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++) {
            if (x >= WX && x < WX + SW && y >= WY && y < WY + SH) continue;
            if (back[y * BW + x] != desk[y * BW + x]) deskbad++;
        }
    verdict(out > 0 && outbad == 0 && deskbad == 0, "plain-blit",
            "%s: %ld canvas px outside regions, %ld differ from the canvas; %ld desktop px outside the window changed",
            tag, out, outbad, deskbad);
    ol_display_destroy(&bdsp);
}

static void reduce_check(void)
{
    struct wm2_env e;
    env_init(&e, 3, 100, 320, 220);
    wm2_backdrop_win_reset(3);
    ol_display_clear(&sd, wm_app_rgb(APP_BG));
    struct logit_backdrop2 bd;
    sidebar(&bd, 100, 0, OL_GLASS_BLUR_THICK);
    wm2_backdrop(&e, (long)&bd, 0, 0);
    draw_app(100);
    struct drect R = { 0, 0, BW, BH }, dev;
    struct ol_display bdsp;
    composite(back, 3, &R, 1, &bdsp);
    oracle(100, &bd, 1, &dev);
    /* a different desktop under it must give the same pixels: nothing sampled */
    for (int i = 0; i < BW * BH; i++) desk[i] ^= 0x5A3C77;
    composite(back2, 3, &R, 1, &bdsp);
    for (int i = 0; i < BW * BH; i++) desk[i] ^= 0x5A3C77;
    long n = 0, solid = 0, notsolid = 0, oracle_bad = 0, sampled = 0;
    uint32_t tint = bd.tint_argb & 0xFFFFFF;
    for (int y = dev.y0; y < dev.y1; y++)
        for (int x = dev.x0; x < dev.x1; x++) {
            if (!marked(x, y)) continue;
            uint32_t got = back[(WY + y) * BW + WX + x];
            n++;
            if (got != ref[(WY + y) * BW + WX + x]) oracle_bad++;
            /* a fully covered pixel reads nothing beneath it (the AA fringe
             * of the rounded corners blends, by design) */
            if (ref[(WY + y) * BW + WX + x] == tint && got != back2[(WY + y) * BW + WX + x]) sampled++;
            /* away from the AA rim the solid material is exactly the tint */
            if (x >= dev.x0 + 2 && x < dev.x1 - 14 && y >= dev.y0 + 14 && y < dev.y1 - 14) {
                if (got == tint) solid++; else notsolid++;
            }
        }
    verdict(n > 0 && solid > 0 && notsolid == 0 && oracle_bad == 0 && sampled == 0, "reduce-solid",
            "%ld marked px: %ld interior = tint %06x, %ld not; %ld differ from the solid oracle; %ld fully covered px depend on the desktop",
            n, solid, tint, notsolid, oracle_bad, sampled);
    /* and the damage/no-cut halo is 0 under Reduce Transparency */
    struct wm_bd_place pl = { WX, WY, SW, SH, SW, SH };
    struct drect ex;
    int any = wm_backdrop_extent(3, &pl, 1, &ex);
    verdict(any && ex.x0 == WX + dev.x0 && ex.y0 == WY + dev.y0 && ex.x1 == WX + dev.x1 && ex.y1 == WY + dev.y1,
            "reduce-solid", "extent under Reduce Transparency is the bare region (%d,%d)-(%d,%d)",
            ex.x0, ex.y0, ex.x1, ex.y1);
    ol_display_destroy(&bdsp);
}

static int contains(const struct drect *o, const struct drect *i)
{ return i->x0 >= o->x0 && i->y0 >= o->y0 && i->x1 <= o->x1 && i->y1 <= o->y1; }
static void box_or(struct drect *a, const struct drect *b)
{
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

/* (e): wm.c's dirty_win adds win_box | wm_backdrop_extent at the old and the
 * new position (win_set_frame calls it before and after the move), then
 * dmg_expand grows every rect with wm_backdrop_dmg_grow until it converges. */
static void damage_checks(void)
{
    static const int blur[] = { OL_GLASS_BLUR_REGULAR, OL_GLASS_BLUR_ULTRA };
    for (int b = 0; b < 2; b++) {
        struct wm2_env e;
        env_init(&e, 5, 100, 300, 200);
        wm2_backdrop_win_reset(5);
        struct logit_backdrop2 bd;
        sidebar(&bd, 100, 2, blur[b]);
        wm2_backdrop(&e, (long)&bd, 0, 0);
        struct wm_bd_place old = { 60, 70, SW, SH, SW, SH }, nw = { 150, 110, SW, SH, SW, SH };
        struct ol_glass_material m = { .tint = bd.tint_argb, .opacity = 96, .blur = (uint8_t)blur[b] };
        int halo = ol_display_glass_sample_halo_ex(120, SH, 12, &m);
        struct drect want_old = { 60 - halo, 70 - halo, 60 + 120 + halo, 70 + SH + halo };
        struct drect want_new = { 150 - halo, 110 - halo, 150 + 120 + halo, 110 + SH + halo };
        struct drect clampb = { 0, 0, BW, BH };
        if (want_old.x0 < 0) want_old.x0 = 0;
        if (want_old.y0 < 0) want_old.y0 = 0;
        (void)clampb;
        /* the window boxes (frame only: the worst case, no shadow margin) */
        struct drect d[2] = { { old.ox, old.oy, old.ox + SW, old.oy + SH },
                              { nw.ox, nw.oy, nw.ox + SW, nw.oy + SH } };
        struct drect ex;
        if (wm_backdrop_extent(5, &old, 0, &ex)) box_or(&d[0], &ex);
        if (wm_backdrop_extent(5, &nw, 0, &ex)) box_or(&d[1], &ex);
        for (int pass = 0; pass < 8; pass++) {
            int grew = 0;
            for (int i = 0; i < 2; i++) grew |= wm_backdrop_dmg_grow(&d[i], 5, &nw, 0, BW, BH);
            if (!grew) break;
        }
        struct drect all = d[0];
        box_or(&all, &d[1]);
        int ok = (contains(&d[0], &want_old) || contains(&d[1], &want_old)) &&
                 (contains(&d[0], &want_new) || contains(&d[1], &want_new));
        verdict(ok && halo > 0, "move-damage",
                "blur %d: halo %d; old (%d,%d)-(%d,%d) new (%d,%d)-(%d,%d) in damage (%d,%d)-(%d,%d) | (%d,%d)-(%d,%d)",
                blur[b], halo, want_old.x0, want_old.y0, want_old.x1, want_old.y1,
                want_new.x0, want_new.y0, want_new.x1, want_new.y1,
                d[0].x0, d[0].y0, d[0].x1, d[0].y1, d[1].x0, d[1].y0, d[1].x1, d[1].y1);
        /* a small rect touching only the halo is grown to the whole region + halo */
        struct drect small = { want_new.x0, want_new.y0 + 40, want_new.x0 + 2, want_new.y0 + 42 };
        int g = wm_backdrop_dmg_grow(&small, 5, &nw, 0, BW, BH);
        verdict(g && contains(&small, &want_new), "move-damage",
                "blur %d: a 2x2 rect in the halo grew to (%d,%d)-(%d,%d)", blur[b],
                small.x0, small.y0, small.x1, small.y1);
        /* and a rect clear of the halo is left alone */
        struct drect far = { want_new.x1 + 1, want_new.y0, want_new.x1 + 9, want_new.y0 + 8 };
        struct drect far0 = far;
        g = wm_backdrop_dmg_grow(&far, 5, &nw, 0, BW, BH);
        verdict(!g && far.x0 == far0.x0 && far.x1 == far0.x1, "move-damage",
                "blur %d: a rect outside the halo is not grown", blur[b]);
    }
}

/* (f) */
static void cover_checks(void)
{
    struct wm2_env e;
    env_init(&e, 6, 100, 300, 200);
    wm2_backdrop_win_reset(6);
    struct logit_backdrop2 bd;
    sidebar(&bd, 100, 1, 0);
    wm2_backdrop(&e, (long)&bd, 0, 0);
    int x = 100, y = 80, ww = SW, tbh = 30, wh = SH + tbh, r = 10;
    struct wm_bd_place pl = { x, y + tbh, SW, SH, SW, SH };
    struct drect touch = { x + 119, y + tbh + 50, x + 160, y + tbh + 90 };   /* 1px into the sidebar */
    struct drect inside = { x + 20, y + tbh + 50, x + 60, y + tbh + 90 };
    struct drect clear = { x + 120, y + tbh + 50, x + 200, y + tbh + 90 };   /* abuts it */
    int c_touch = wm_cover_ok(&touch, x, y, ww, wh, tbh, r, 0) && wm_backdrop_cover_ok(&touch, 6, &pl);
    int c_inside = wm_cover_ok(&inside, x, y, ww, wh, tbh, r, 0) && wm_backdrop_cover_ok(&inside, 6, &pl);
    int c_clear = wm_cover_ok(&clear, x, y, ww, wh, tbh, r, 0) && wm_backdrop_cover_ok(&clear, 6, &pl);
    verdict(!c_touch && !c_inside && c_clear, "cover-region",
            "touching %d, inside %d (both must be refused), clear %d (accepted)", c_touch, c_inside, c_clear);
    struct drect reg[LOGIT_BACKDROP_MAX];
    int n = wm_backdrop_screen_rects(6, &pl, reg, LOGIT_BACKDROP_MAX);
    verdict(n == 1 && reg[0].x0 == x && reg[0].x1 == x + 120 && reg[0].y0 == y + tbh && reg[0].y1 == y + tbh + SH,
            "cover-region", "screen rect (%d,%d)-(%d,%d)", reg[0].x0, reg[0].y0, reg[0].x1, reg[0].y1);
    wm2_backdrop_win_reset(6);
    verdict(wm_backdrop_cover_ok(&inside, 6, &pl), "cover-region", "no region after reset: accepted");
}

/* (g) */
static void forgery_checks(void)
{
    struct wm2_env e;
    env_init(&e, 7, 100, 200, 120);
    memset(surf, 0, sizeof surf);
    ol_display_clear(&sd, wm_app_rgb(0x01FF0000u));
    ol_display_fill_rect(&sd, 10, 10, 80, 40, wm_app_rgb(0x01FF0000u));
    ol_display_round_rect(&sd, 100, 10, 60, 40, 8, wm_app_rgb(0x01FF0000u));
    long marks = 0;
    for (int i = 0; i < SW * SH; i++) marks += (surf[i] >> 24) == LOGIT_BACKDROP_MARK;
    /* the instrument can see a forgery: the raw value does store MARK */
    ol_display_fill_rect(&sd, 10, 60, 20, 20, 0x01FF0000u);
    long raw = 0;
    for (int i = 0; i < SW * SH; i++) raw += (surf[i] >> 24) == LOGIT_BACKDROP_MARK;
    verdict(marks == 0 && raw == 400, "mark-forgery",
            "masked CLEAR/RECT/RRECT of 0x01FF0000: %ld MARK px (raw RECT, the instrument: %ld)", marks, raw);
}

static void validate_checks(void)
{
    struct wm2_env e;
    env_init(&e, 9, 100, 200, 160);
    wm2_backdrop_win_reset(9);
    ol_display_clear(&sd, 0);
    struct logit_backdrop2 bd;
    int bad = 0;
#define EXPECT(call, want, what) do { long r_ = (call); if (r_ != (want)) { \
        printf("     validate: %s returned %ld, want %ld\n", what, r_, (long)(want)); bad++; } } while (0)
    sidebar(&bd, 100, 0, 0); bd.id = 8;        EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "id 8");
    sidebar(&bd, 100, 0, 0); bd.id = -1;       EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "id -1");
    sidebar(&bd, 100, 0, 0); bd.reserved[2] = 1; EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "reserved");
    sidebar(&bd, 100, 0, 0); bd.flags = LOGIT_BACKDROP_ALPHA_LAYER;
    EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_UNSUPPORTED, "ALPHA_LAYER");
    sidebar(&bd, 100, 0, 0); bd.flags = 1u << 5; EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "unknown flag");
    sidebar(&bd, 100, 0, 0); bd.blur_level = 4; EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "blur 4");
    sidebar(&bd, 100, 0, 0); bd.rim = 2;       EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "rim 2");
    sidebar(&bd, 100, 0, 0); bd.opacity = 256; EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "opacity 256");
    sidebar(&bd, 100, 0, 0); bd.w = -1;        EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "w -1");
    sidebar(&bd, 100, 0, 0); bd.radius[0] = -1; EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "radius -1");
    sidebar(&bd, 100, 0, 0); bd.size = sizeof bd - 4; EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_ARGUMENT, "size-4");
    EXPECT(wm2_backdrop(&e, 0, 0, 0), OL_ARGUMENT, "NULL");
    EXPECT(wm_backdrop_any(9), 0, "nothing recorded by a refused call");
    for (int id = 0; id < LOGIT_BACKDROP_MAX; id++) {
        sidebar(&bd, 100, id, 0);
        bd.y = (id * 20) << 8; bd.h = 20 << 8; bd.w = (20 + id) << 8;
        EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_OK, "id 0..7");
    }
    struct wm2_region rg[LOGIT_BACKDROP_MAX + 2];
    EXPECT(wm_backdrop_regions(9, rg, LOGIT_BACKDROP_MAX + 2), 8, "eight regions");
    EXPECT(rg[5].id == 5 && rg[5].r.x1 == 25 && rg[5].r.y0 == 100 && rg[5].r.y1 == 120, 1, "region 5 rect");
    sidebar(&bd, 100, 5, 0); bd.y = 100 << 8; bd.h = 30 << 8;
    EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_OK, "replace id 5");
    EXPECT(wm_backdrop_regions(9, rg, LOGIT_BACKDROP_MAX), 8, "replacing keeps eight");
    EXPECT(wm2_backdrop_clear(&e, 8, 0, 0), OL_ARGUMENT, "CLEAR(8)");
    EXPECT(wm2_backdrop_clear(&e, -2, 0, 0), OL_ARGUMENT, "CLEAR(-2)");
    EXPECT(wm2_backdrop_clear(&e, 3, 0, 0), OL_OK, "CLEAR(3)");
    EXPECT(wm_backdrop_regions(9, rg, LOGIT_BACKDROP_MAX), 7, "seven after CLEAR(3)");
    EXPECT(wm2_backdrop_clear(&e, 3, 0, 0), OL_OK, "CLEAR(3) again");
    /* an empty or fully off-canvas region removes the id */
    sidebar(&bd, 100, 4, 0); bd.x = 400 << 8;
    EXPECT(wm2_backdrop(&e, (long)&bd, 0, 0), OL_OK, "off-canvas");
    EXPECT(wm_backdrop_regions(9, rg, LOGIT_BACKDROP_MAX), 6, "off-canvas removes id 4");
    /* another slot is independent */
    struct wm2_env e2 = e; e2.slot = 10;
    wm2_backdrop_win_reset(10);
    sidebar(&bd, 100, 0, 0);
    EXPECT(wm2_backdrop(&e2, (long)&bd, 0, 0), OL_OK, "slot 10");
    EXPECT(wm2_backdrop_clear(&e, -1, 0, 0), OL_OK, "CLEAR(-1)");
    EXPECT(wm_backdrop_regions(9, rg, LOGIT_BACKDROP_MAX), 0, "none after CLEAR(-1)");
    EXPECT(wm_backdrop_regions(10, rg, LOGIT_BACKDROP_MAX), 1, "slot 10 untouched");
    wm2_backdrop_win_reset(10);
    EXPECT(wm_backdrop_any(10), 0, "reset drops slot 10");
    e2.slot = 16;
    EXPECT(wm2_backdrop(&e2, (long)&bd, 0, 0), OL_STATE, "slot 16");
    EXPECT(wm2_backdrop_clear(&e2, 0, 0, 0), OL_STATE, "CLEAR slot 16");
    EXPECT(wm2_backdrop_info, LOGIT_GUI2_F_BACKDROP, "wm2_backdrop_info");
#undef EXPECT
    verdict(bad == 0, "validate", "%d mismatches", bad);
}

/* Device rect and mark coverage at fractional scales. */
static void hidpi_checks(void)
{
    static const int pcts[] = { 125, 150 };
    for (int k = 0; k < 2; k++) {
        int pct = pcts[k];
        struct wm2_env e;
        env_init(&e, 11, pct, 300, 200);
        wm2_backdrop_win_reset(11);
        memset(surf, 0, sizeof surf);
        struct logit_backdrop2 bd;
        sidebar(&bd, pct, 0, 0);
        bd.x = (3 << 8) + 128;          /* 3.5 pt */
        bd.y = 7 << 8;
        bd.w = 101 << 8;
        bd.h = 61 << 8;
        bd.radius[1] = bd.radius[2] = 9 << 8;
        wm2_backdrop(&e, (long)&bd, 0, 0);
        int x0 = (int)((((long long)bd.x * pct / 100) + 128) >> 8);
        int y0 = (int)((((long long)bd.y * pct / 100) + 128) >> 8);
        int x1 = (int)((((long long)(bd.x + bd.w) * pct / 100) + 128) >> 8);
        int y1 = (int)((((long long)(bd.y + bd.h) * pct / 100) + 128) >> 8);
        int rad = (int)((((long long)bd.radius[1] * pct / 100) + 128) >> 8);
        struct wm2_region rg[1];
        int n = wm_backdrop_regions(11, rg, 1);
        int rect_ok = n == 1 && rg[0].r.x0 == x0 && rg[0].r.y0 == y0 && rg[0].r.x1 == x1 && rg[0].r.y1 == y1 &&
                      rg[0].radius[1] == rad && rg[0].radius[0] == 0;
        /* marks: every pixel of the rect except the two rounded right corners'
         * outside, none outside the rect */
        long outside = 0, inside_unmarked = 0, corner_unmarked = 0;
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                int in = x >= x0 && x < x1 && y >= y0 && y < y1;
                int m = marked(x, y);
                if (!in) { outside += m; continue; }
                int corner = x >= x1 - rad && (y < y0 + rad || y >= y1 - rad);
                if (!m) { if (corner) corner_unmarked++; else inside_unmarked++; }
            }
        verdict(rect_ok && outside == 0 && inside_unmarked == 0 && corner_unmarked > 0 && corner_unmarked < rad * rad,
                "hidpi", "%d%%: region (%d,%d)-(%d,%d) r %d [want (%d,%d)-(%d,%d) r %d]; %ld marks outside, %ld unmarked "
                "inside, %ld unmarked in the corners", pct, n ? rg[0].r.x0 : -1, n ? rg[0].r.y0 : -1,
                n ? rg[0].r.x1 : -1, n ? rg[0].r.y1 : -1, n ? rg[0].radius[1] : -1, x0, y0, x1, y1, rad,
                outside, inside_unmarked, corner_unmarked);
        char tag[16];
        snprintf(tag, sizeof tag, "%d%%", pct);
        scene_checks(pct, OL_GLASS_BLUR_THIN, tag);
    }
}

/* Two drawing contexts interleaved (the compositor's band workers): each
 * composite uses its own display's glass scratch, so the result is the
 * serial one. */
static void nctx_checks(void)
{
    gfx_ctx_hook = ctx_of;
    struct wm2_env e;
    env_init(&e, 12, 100, 320, 220);
    wm2_backdrop_win_reset(12);
    ol_display_clear(&sd, wm_app_rgb(APP_BG));
    struct logit_backdrop2 bd;
    sidebar(&bd, 100, 0, OL_GLASS_BLUR_THICK);
    wm2_backdrop(&e, (long)&bd, 0, 0);
    sidebar(&bd, 100, 1, OL_GLASS_BLUR_REGULAR);
    bd.x = 200 << 8; bd.w = 80 << 8; bd.y = 20 << 8; bd.h = 120 << 8;
    bd.radius[0] = bd.radius[1] = bd.radius[2] = bd.radius[3] = 10 << 8;
    wm2_backdrop(&e, (long)&bd, 0, 0);
    draw_app(100);
    /* serial references: the whole frame on context 0 */
    static uint32_t s0[BW * BH], s1[BW * BH];
    struct drect top = { 0, 0, BW, WY + 150 }, all = { 0, 0, BW, BH };
    struct ol_display d0, d1;
    g_ctx = 0; composite(s0, 12, &all, 0, &d0); ol_display_destroy(&d0);
    g_ctx = 0; composite(s1, 12, &top, 0, &d0); ol_display_destroy(&d0);
    /* interleaved: context 0 and context 1 alternate on two back buffers */
    memcpy(back, desk, sizeof desk); memcpy(back2, desk, sizeof desk);
    bind(&d0, back, BW, BH); bind(&d1, back2, BW, BH);
    d0.target.clip_on = 1; d0.target.clx0 = 0; d0.target.cly0 = 0; d0.target.clx1 = BW; d0.target.cly1 = BH;
    d1.target.clip_on = 1; d1.target.clx0 = 0; d1.target.cly0 = 0; d1.target.clx1 = BW; d1.target.cly1 = top.y1;
    struct ol_pixel_target src = { .px = surf, .w = SW, .h = SH };
    g_ctx = 0; wm_backdrop_composite(&all, 12, WX, WY, &d0);
    g_ctx = 1; wm_backdrop_composite(&top, 12, WX, WY, &d1);
    g_ctx = 0; wm_backdrop_blit_content(&all, 12, WX, WY, &d0, &src);
    g_ctx = 1; wm_backdrop_blit_content(&top, 12, WX, WY, &d1, &src);
    long bad0 = 0, bad1 = 0;
    for (int i = 0; i < BW * BH; i++) { bad0 += back[i] != s0[i]; bad1 += back2[i] != s1[i]; }
    verdict(bad0 == 0 && bad1 == 0, "nctx", "interleaved contexts: %ld and %ld px differ from serial", bad0, bad1);
    ol_display_destroy(&d0); ol_display_destroy(&d1);
    gfx_ctx_hook = 0;
    g_ctx = 0;
}

int main(void)
{
    make_desktop();
    scene_checks(100, OL_GLASS_BLUR_REGULAR, "100% REGULAR");
    scene_checks(100, OL_GLASS_BLUR_THICK, "100% THICK");
    reduce_check();
    damage_checks();
    cover_checks();
    forgery_checks();
    validate_checks();
    hidpi_checks();
    nctx_checks();
    if (fails) { printf("wm_backdrop_test: %d check(s) FAILED\n", fails); return 1; }
    printf("wm_backdrop_test: all checks passed\n");
    return 0;
}
