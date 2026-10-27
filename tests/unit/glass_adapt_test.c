/* Glass adaptive tint: OL_GLASS_ADAPT_SMOOTH and OL_GLASS_ADAPT_FG.
 *
 * - continuity: sweeping a gray tint's tone 0..255 over a flat backdrop, the
 *   panel centre luma never steps by more than 4/255 between neighbouring tones
 *   (the hard thresholds at tone 224 and 64 jump by tens of levels).
 * - parity: flags 0 equals the legacy entry point bit for bit, and SMOOTH
 *   equals it where the hard rule is fully on or fully off.
 * - fg-contrast: with ADAPT_FG, white and black text keep WCAG >= 4.5:1 over
 *   random photo-like backdrops (checked at the panel centre), and the same
 *   backdrops violate it with flags 0 so the check is not vacuous.
 * - reduce transparency draws the same solid panel with or without the flags.
 *
 * Built with -DGFX_NCTX=2 and no fb.c, so this file defines gfx_ctx_hook.
 * Each failing check prints "FAIL <name>" and the exit status is 1. The
 * negative control builds with -DGLASS_ADAPT_HARD_THRESHOLD (SMOOTH falls back
 * to the hard rule) and "continuity" must fail. */
#include "openlogit_display.h"
#include "openlogit_glass_material.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int (*gfx_ctx_hook)(void);

static int checks, failures;

static void check(int condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

enum { SW = 160, SH = 120, CX = 80, CY = 60 };
static uint32_t px[SW * SH], ref[SW * SH];

static void render(uint32_t *out, const struct ol_glass_material *mat, int reduce, int legacy)
{
    struct ol_display d;
    struct ol_pixel_target t;
    memset(&d, 0, sizeof d);
    memset(&t, 0, sizeof t);
    d.allocate = malloc;
    d.release = free;
    t.px = out;
    t.w = SW;
    t.h = SH;
    if (ol_display_bind(&d, &t, 16, 8, 0) != OL_OK) {
        fprintf(stderr, "cannot bind display\n");
        exit(2);
    }
    d.reduce_transparency = reduce;
    if (legacy)
        ol_display_liquid_glass_cut(&d, 30, 20, 100, 80, 16, mat->tint >> 16 & 255,
                                    mat->tint >> 8 & 255, mat->tint & 255, mat->opacity, 0);
    else
        ol_display_liquid_glass_ex(&d, 30, 20, 100, 80, 16, 0, mat);
    if (d.error) {
        fprintf(stderr, "glass render error %d\n", d.error);
        exit(2);
    }
    ol_display_destroy(&d);
}

static int luma(uint32_t p)
{
    return (54 * (int)(p >> 16 & 255) + 183 * (int)(p >> 8 & 255) + 19 * (int)(p & 255)) >> 8;
}

static double lin(int c8)
{
    double c = c8 / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double rel_lum(uint32_t p)
{
    return 0.2126 * lin(p >> 16 & 255) + 0.7152 * lin(p >> 8 & 255) + 0.0722 * lin(p & 255);
}

static double contrast(uint32_t a, uint32_t b)
{
    double x = rel_lum(a), y = rel_lum(b);
    if (x < y) { double t = x; x = y; y = t; }
    return (x + 0.05) / (y + 0.05);
}

static void flat(uint32_t *out, uint32_t color)
{
    for (int i = 0; i < SW * SH; i++) out[i] = color;
}

static uint32_t rng_state = 0x1234567u;
static int rnd(int n)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int)((rng_state >> 8) % (unsigned)n);
}

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* A photo-like backdrop: a random base colour that varies smoothly, with
 * moderate colour cast and low-amplitude noise. */
static void photo(uint32_t *out)
{
    int gray = rnd(256);
    int cast[3] = {rnd(61) - 30, rnd(61) - 30, rnd(61) - 30};
    int gx = rnd(61) - 30, gy = rnd(61) - 30;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int g = gray + gx * (x - SW / 2) / SW + gy * (y - SH / 2) / SH;
            int n = rnd(9) - 4;
            out[y * SW + x] = (uint32_t)clamp8(g + cast[0] + n) << 16 |
                              (uint32_t)clamp8(g + cast[1] + n) << 8 |
                              (uint32_t)clamp8(g + cast[2] + n);
        }
}

static void test_continuity(void)
{
    static const int backdrops[] = {100, 140, 200};
    for (unsigned b = 0; b < sizeof backdrops / sizeof *backdrops; b++) {
        int previous = -1;
        for (int tone = 0; tone < 256; tone++) {
            struct ol_glass_material mat = {.tint = (uint32_t)tone * 0x010101u,
                                            .opacity = 60, .flags = OL_GLASS_ADAPT_SMOOTH};
            flat(px, (uint32_t)backdrops[b] * 0x010101u);
            render(px, &mat, 0, 0);
            int now = luma(px[CY * SW + CX]);
            if (previous >= 0 && abs(now - previous) > 4) {
                printf("FAIL continuity backdrop %d tone %d: %d -> %d\n", backdrops[b], tone,
                       previous, now);
                failures++;
                checks++;
                return;
            }
            previous = now;
        }
    }
    checks++;
}

static void test_parity(void)
{
    static const uint32_t tints[] = {0x000000, 0x202020, 0x808080, 0xA0A0A0, 0xF0F0F0, 0xFFFFFF,
                                     0xC0A040, 0x2060C0};
    static const int opacities[] = {0, 40, 90, 255};
    for (unsigned t = 0; t < sizeof tints / sizeof *tints; t++)
        for (unsigned o = 0; o < sizeof opacities / sizeof *opacities; o++)
            for (int b = 0; b < 3; b++) {
                struct ol_glass_material mat = {.tint = tints[t], .opacity = (uint8_t)opacities[o]};
                uint32_t back = (uint32_t[]){0x606060, 0x909090, 0xD0D0D0}[b] ;
                flat(px, back);
                render(px, &mat, 0, 0);
                flat(ref, back);
                render(ref, &mat, 0, 1);
                check(!memcmp(px, ref, sizeof px), "parity-flags0");
                /* SMOOTH matches the hard rule where it is fully on or off. */
                int tone = luma(tints[t]);
                int on = (tone >= 224 || tone <= 64) && opacities[o] >= 32 &&
                         opacities[o] != 255 && (tints[t] & 0xFF) == (tints[t] >> 8 & 0xFF);
                int off = tone > 96 && tone < 160;
                if (on || off) {
                    mat.flags = OL_GLASS_ADAPT_SMOOTH;
                    flat(px, back);
                    render(px, &mat, 0, 0);
                    check(!memcmp(px, ref, sizeof px), "parity-smooth-limit");
                }
            }
}

static void test_fg(uint32_t fg, const char *name)
{
    static const uint32_t tints[] = {0x000000, 0x808080, 0xFFFFFF, 0x3050A0};
    static const int opacities[] = {0, 30, 100, 200};
    int worst_ok = 1, flagged_bad = 0;
    double worst = 1e9;
    rng_state = 0x9E3779B9u;
    for (int n = 0; n < 60; n++) {
        photo(ref);
        for (unsigned t = 0; t < 4; t++)
            for (unsigned o = 0; o < 4; o++) {
                struct ol_glass_material mat = {.tint = tints[t], .opacity = (uint8_t)opacities[o],
                                                .flags = OL_GLASS_ADAPT_FG, .fg_rgb = fg};
                memcpy(px, ref, sizeof px);
                render(px, &mat, 0, 0);
                double c = contrast(px[CY * SW + CX], fg);
                if (c < worst) worst = c;
                if (c < 4.5) worst_ok = 0;
                mat.flags = 0;
                memcpy(px, ref, sizeof px);
                render(px, &mat, 0, 0);
                if (contrast(px[CY * SW + CX], fg) < 4.5) flagged_bad++;
            }
    }
    char label[64];
    snprintf(label, sizeof label, "fg-contrast-%s", name);
    check(worst_ok, label);
    snprintf(label, sizeof label, "fg-needed-%s", name);
    check(flagged_bad > 100, label);
    printf("%s: worst contrast %.2f, flags-0 violations %d of 960\n", name, worst, flagged_bad);
}

static void test_reduce(void)
{
    struct ol_glass_material a = {.tint = 0xFFFFFF, .opacity = 60};
    struct ol_glass_material b = a;
    b.flags = OL_GLASS_ADAPT_FG | OL_GLASS_ADAPT_SMOOTH;
    b.fg_rgb = 0xFFFFFF;
    photo(ref);
    memcpy(px, ref, sizeof px);
    render(px, &a, 1, 0);
    uint32_t keep[SW * SH];
    memcpy(keep, px, sizeof keep);
    memcpy(px, ref, sizeof px);
    render(px, &b, 1, 0);
    check(!memcmp(px, keep, sizeof keep), "reduce-transparency");
}

int main(void)
{
    test_continuity();
    test_parity();
    test_fg(0xFFFFFF, "white");
    test_fg(0x000000, "black");
    test_reduce();
    if (failures) {
        printf("%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("glass adaptive tint: %d checks passed\n", checks);
    return 0;
}
