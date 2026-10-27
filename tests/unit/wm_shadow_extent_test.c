/* wm_shadow.h against the real ol_display_shadow_ex.
 *
 *   extent          for every preset kind at 100/125/150/200%, every pixel the
 *                   two layers write (peak alpha 255 and the presets' own
 *                   dark/light alphas, a few box sizes and radii, full size and
 *                   thumbnail scale) lies inside the box grown by
 *                   wm_shadow_extent.            -> 'FAIL extent'
 *   drew            the shadow actually reaches past the box (the property is
 *                   not vacuous).                -> 'FAIL drew'
 *   scale-2x        the 200% falloff beside a caster is the 100% falloff at
 *                   half the distance, within 6/255: no 72 px mask clamp.
 *                                                -> 'FAIL scale-2x'
 *
 * `wm_shadow_extent_test --profile PCT DARK FOCUS` prints the oracle falloff
 * (shadow alpha 0..255 at distance 1..extent left of a caster, mid height) for
 * the guest driver tests/qmp/qmp_wm_shadow.py.
 * -DWM_SHADOW_EXTENT_NEGCTL makes the extent blur-only: 'FAIL extent'. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "gfx.h"
#include "openlogit_display.h"
#include "openlogit_display_shadow.h"
#include "wm_shadow.h"

int (*gfx_ctx_hook)(void);
static int ctx0(void) { return 0; }

static int bad;
static void fail(const char *name, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void fail(const char *name, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("FAIL %s ", name); vprintf(fmt, ap); putchar('\n');
    va_end(ap); bad = 1;
}

static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }

#define CAN 900
static uint32_t buf[CAN * CAN];

static void bind(struct ol_display *d, struct ol_pixel_target *t)
{
    memset(d, 0, sizeof *d); memset(t, 0, sizeof *t);
    d->allocate = t_alloc; d->release = t_free;
    t->px = buf; t->w = CAN; t->h = CAN;
    if (ol_display_bind(d, t, 16, 8, 0) != OL_OK) { fprintf(stderr, "bind failed\n"); exit(2); }
}

static void cast(struct ol_display *d, int kind, int pct, int scale256, int x, int y, int w, int h,
                 int rad, int alpha_mode)
{
    int r4[4] = { rad, rad, rad, rad };
    for (int i = 0; i < WMSH_NLAYER; i++) {
        const struct wm_shadow_layer *l = &wm_shadow_preset[kind][i];
        int dy24, sig24, spr24;
        wm_shadow_dev(l, pct, scale256, &dy24, &sig24, &spr24);
        int a = alpha_mode == 0 ? 255 : alpha_mode == 1 ? l->a_dark : l->a_light;
        ol_display_shadow_ex(d, x, y, w, h, r4, 0, dy24, sig24, spr24, 0x000000, a, 0);
    }
}

static void profile(int pct, int kind, int out[], int n)
{
    struct ol_display d; struct ol_pixel_target t; bind(&d, &t);
    for (int i = 0; i < CAN * CAN; i++) buf[i] = 0xFFFFFF;
    int x = 400, y = 300, w = 200, h = 200;
    cast(&d, kind, pct, 256, x, y, w, h, 10 * pct / 100, 255);
    (void)n;
    for (int i = 1; i <= n; i++) out[i] = 255 - (int)(buf[(y + h / 2) * CAN + (x - i)] & 0xFF);
}

int main(int argc, char **argv)
{
    gfx_ctx_hook = ctx0;
    if (argc == 5 && !strcmp(argv[1], "--profile")) {
        int pct = atoi(argv[2]), dark = atoi(argv[3]), focus = atoi(argv[4]);
        int kind = focus ? WMSH_WIN_FOCUSED : WMSH_WIN_UNFOCUSED;
        struct ol_display d; struct ol_pixel_target t; bind(&d, &t);
        for (int i = 0; i < CAN * CAN; i++) buf[i] = 0xFFFFFF;
        int x = 400, y = 300, w = 200, h = 200, e[4];
        wm_shadow_extent(kind, pct, e);
        cast(&d, kind, pct, 256, x, y, w, h, 10 * pct / 100, dark ? 1 : 2);
        printf("extent %d\n", e[0]);
        for (int i = 1; i <= e[0]; i++)
            printf("%d %d\n", i, 255 - (int)(buf[(y + h / 2) * CAN + (x - i)] & 0xFF));
        return 0;
    }

    static const int pcts[] = { 100, 125, 150, 200 };
    static const int sizes[][3] = { { 200, 150, 10 }, { 40, 24, 12 }, { 300, 60, 28 }, { 30, 30, 0 } };
    long drew_total = 0;
    for (int kind = 0; kind < WMSH_NKIND; kind++)
    for (unsigned pi = 0; pi < 4; pi++)
    for (int sc = 0; sc < 2; sc++)
    for (int am = 0; am < 3; am++)
    for (unsigned si = 0; si < 4; si++) {
        int pct = pcts[pi], scale256 = sc ? 90 : 256;
        int ext[4]; wm_shadow_extent(kind, pct, ext);
        int w = sizes[si][0] * pct / 100, h = sizes[si][1] * pct / 100;
        int rad = sizes[si][2] * pct / 100;
        int x = (CAN - w) / 2, y = (CAN - h) / 2;
        struct ol_display d; struct ol_pixel_target t; bind(&d, &t);
        for (int i = 0; i < CAN * CAN; i++) buf[i] = 0xFFFFFF;
        cast(&d, kind, pct, scale256, x, y, w, h, rad, am);
        int out = 0, far = 0, first_x = -1, first_y = -1;
        for (int py = 0; py < CAN; py++)
            for (int px = 0; px < CAN; px++) {
                if (buf[py * CAN + px] == 0xFFFFFF) continue;
                int inx = px >= x - ext[0] && px < x + w + ext[2];
                int iny = py >= y - ext[1] && py < y + h + ext[3];
                if (!(inx && iny)) { if (!out++) { first_x = px; first_y = py; } }
                if (px < x || px >= x + w || py < y || py >= y + h) far++;
            }
        if (out) fail("extent", "kind %d pct %d scale %d alpha-mode %d box %dx%d: %d px outside, first (%d,%d) box (%d,%d) ext %d",
                      kind, pct, scale256, am, w, h, out, first_x, first_y, x, y, ext[0]);
        if (am == 0 && sc == 0 && far == 0) fail("drew", "kind %d pct %d drew nothing outside the box", kind, pct);
        drew_total += far;
    }
    if (drew_total < 100000) fail("drew", "only %ld px drawn in total", drew_total);

    /* 200% is 100% scaled by 2, not clamped (focused window, peak alpha). */
    {
        static int p1[400], p2[400];
        profile(100, WMSH_WIN_FOCUSED, p1, 200);
        profile(200, WMSH_WIN_FOCUSED, p2, 380);
        int worst = 0, at = 0;
        for (int i = 1; i <= 60; i++) {
            int d = abs(p2[2 * i] - p1[i]);
            if (d > worst) { worst = d; at = i; }
        }
        if (worst > 6) fail("scale-2x", "profile(200%%, 2i) vs profile(100%%, i): worst delta %d at i=%d", worst, at);
        else printf("ok   scale-2x: worst delta %d/255\n", worst);
    }
    if (!bad) puts("wm_shadow_extent: OK");
    return bad ? 1 : 0;
}
