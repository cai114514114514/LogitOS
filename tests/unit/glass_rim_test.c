/* Glass rim modes: OL_GLASS_RIM_SYMMETRIC on small controls.
 *
 * The directional rim lights the top-left and shades the bottom-right. Over a
 * flat panel that reads as two dark crescents, which is why AUI buttons and
 * Files controls stopped using glass. SYMMETRIC replaces it with a ring that
 * depends on edge distance only, and attenuates the environment reflection by
 * the variance of the blurred backdrop around the rim.
 *
 * Built with -DGFX_NCTX=2 and without fb.c, so this file defines gfx_ctx_hook.
 * Every failing check prints "FAIL <name>" and the exit status is 1.
 *
 * The negative control builds with -DGLASS_RIM_FORCE_DIRECTIONAL: a SYMMETRIC
 * request then renders the directional rim and "rim-uniform" must fail. */
#include "openlogit_display.h"
#include "openlogit_glass_material.h"
#include "glass_internal.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int (*gfx_ctx_hook)(void);
/* -DGLASS_RIM_TEST_GAIN: -1 keeps the measured reflect_gain. */
int glass_rim_gain_override = -1;

static int checks, failures;

static void check(int condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

enum { SW = 240, SH = 160 };
static uint32_t backdrop[SW * SH], a[SW * SH], b[SW * SH];

static void bind(struct ol_display *d, struct ol_pixel_target *t, uint32_t *px)
{
    memset(d, 0, sizeof *d);
    memset(t, 0, sizeof *t);
    d->allocate = malloc;
    d->release = free;
    t->px = px;
    t->w = SW;
    t->h = SH;
    if (ol_display_bind(d, t, 16, 8, 0) != OL_OK) {
        fprintf(stderr, "cannot bind display\n");
        exit(2);
    }
}

struct scene {
    int x, y, w, h, r;
    unsigned cut;
    uint32_t tint;
    uint8_t opacity, blur;
};

static void render_ex(uint32_t *px, const struct scene *s, uint8_t rim, int reduce)
{
    struct ol_display d;
    struct ol_pixel_target t;
    struct ol_glass_material mat = {.tint = s->tint, .opacity = s->opacity,
                                    .blur = s->blur, .rim = rim};
    bind(&d, &t, px);
    d.reduce_transparency = reduce;
    ol_display_liquid_glass_ex(&d, s->x, s->y, s->w, s->h, s->r, s->cut, &mat);
    if (d.error) {
        fprintf(stderr, "glass render error %d\n", d.error);
        exit(2);
    }
    ol_display_destroy(&d);
}

static void render_legacy(uint32_t *px, const struct scene *s)
{
    struct ol_display d;
    struct ol_pixel_target t;
    bind(&d, &t, px);
    ol_display_liquid_glass_cut(&d, s->x, s->y, s->w, s->h, s->r, s->tint >> 16 & 255,
                                s->tint >> 8 & 255, s->tint & 255, s->opacity, s->cut);
    if (d.error) {
        fprintf(stderr, "glass render error %d\n", d.error);
        exit(2);
    }
    ol_display_destroy(&d);
}

static void flat(uint32_t *px, uint32_t color)
{
    for (int i = 0; i < SW * SH; i++) px[i] = color;
}

static void stripes(uint32_t *px, uint32_t c0, uint32_t c1, int width)
{
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) px[y * SW + x] = (x / width) & 1 ? c1 : c0;
}

static void patterned(uint32_t *px)
{
    uint32_t state = 0x2545F491u;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            state = state * 1664525u + 1013904223u;
            uint32_t v = state >> 8;
            px[y * SW + x] = (x / 9 + y / 6) & 1 ? (v & 0xFFFFFF) : 0x304050 + (v & 0x3F3F3F);
        }
}

static int luma(uint32_t pixel)
{
    return (54 * (int)(pixel >> 16 & 255) + 183 * (int)(pixel >> 8 & 255) +
            19 * (int)(pixel & 255)) >> 8;
}

static uint64_t fnv(uint64_t hash, const uint32_t *px)
{
    for (int i = 0; i < SW * SH; i++)
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (px[i] >> shift) & 255;
            hash *= UINT64_C(1099511628211);
        }
    return hash;
}

/* The renderer's own frame: pixel column c sits at offset c - w/2, and the
 * signed distance to the rounded rectangle is zero on the half-covered ring. */
static double signed_distance(const struct scene *s, int column, int row)
{
    int radius = s->r;
    int short_side = s->w < s->h ? s->w : s->h;
    if (radius > short_side / 2) radius = short_side / 2;
    double dx = fabs((double)(column - s->x - s->w / 2)) - (s->w / 2 - radius);
    double dy = fabs((double)(row - s->y - s->h / 2)) - (s->h / 2 - radius);
    double ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    double inside = dx > dy ? dx : dy;
    if (inside > 0) inside = 0;
    return sqrt(ox * ox + oy * oy) + inside - radius;
}

static int in_rim(const struct scene *s, int edge, int column, int row)
{
    if (column < s->x || row < s->y || column >= s->x + s->w || row >= s->y + s->h)
        return 0;
    double d = signed_distance(s, column, row);
    return d < 0.5 && d >= -edge;
}

/* Brightest rim pixel along each of 32 rays from the centre, plus the darkest
 * pixel anywhere inside the panel. */
static void rim_stats(const uint32_t *px, const struct scene *s, int edge,
                      int *spread, int *lowest, int *highest, int *darkest)
{
    *lowest = 255;
    *highest = 0;
    *darkest = 255;
    for (int k = 0; k < 32; k++) {
        double angle = k * 2 * M_PI / 32;
        double cx = s->x + s->w / 2, cy = s->y + s->h / 2;
        int best = -1;
        for (double t = 0; t < s->w + s->h; t += 0.25) {
            int column = (int)floor(cx + t * cos(angle) + 0.5);
            int row = (int)floor(cy + t * sin(angle) + 0.5);
            if (!in_rim(s, edge, column, row)) continue;
            int l = luma(px[row * SW + column]);
            if (l > best) best = l;
        }
        if (best < 0) best = 0;
        if (best < *lowest) *lowest = best;
        if (best > *highest) *highest = best;
    }
    *spread = *highest - *lowest;
    for (int row = s->y; row < s->y + s->h; row++)
        for (int column = s->x; column < s->x + s->w; column++) {
            int l = luma(px[row * SW + column]);
            if (l < *darkest) *darkest = l;
        }
}

static void test_uniform(const char *label, int w, int h, int r)
{
    const uint32_t back = 0xECECEC;
    struct scene s = {(SW - w) / 2, (SH - h) / 2, w, h, r, 0, 0xFFFFFF, 50, 0};
    struct ol_glass_material sym = {.rim = OL_GLASS_RIM_SYMMETRIC};
    int edge = glass_panel_edge_width(w, h, r, &sym);
    int spread, lowest, highest, darkest;
    char name[96];

    flat(a, back);
    render_ex(a, &s, OL_GLASS_RIM_SYMMETRIC, 0);
    rim_stats(a, &s, edge, &spread, &lowest, &highest, &darkest);
    printf("rim %s: edge=%d rays=32 min=%d max=%d spread=%d darkest=%d backdrop=%d\n",
           label, edge, lowest, highest, spread, darkest, luma(back));
    snprintf(name, sizeof name, "rim-uniform %s", label);
    check(spread <= 6, name);
    snprintf(name, sizeof name, "rim-dark %s", label);
    check(darkest >= luma(back) - 4, name);
    snprintf(name, sizeof name, "rim-highlight %s", label);
    check(highest > luma(back), name);

    /* The same control in the dark theme (aui_glass's dark tint) over a dark
     * flat panel: the ring stays uniform there too. */
    struct scene dark = s;
    dark.tint = 0x22242E;
    dark.opacity = 120;
    flat(a, 0x2C2C30);
    render_ex(a, &dark, OL_GLASS_RIM_SYMMETRIC, 0);
    rim_stats(a, &dark, edge, &spread, &lowest, &highest, &darkest);
    printf("rim %s dark: min=%d max=%d spread=%d darkest=%d backdrop=%d\n",
           label, lowest, highest, spread, darkest, luma(0x2C2C30));
    snprintf(name, sizeof name, "rim-uniform %s dark", label);
    check(spread <= 6, name);
    snprintf(name, sizeof name, "rim-dark %s dark", label);
    check(darkest >= luma(0x2C2C30) - 4, name);
}

static int render_gain(uint32_t *px, const uint32_t *source, const struct scene *s, int gain)
{
    glass_rim_gain_override = gain;
    memcpy(px, source, sizeof backdrop);
    render_ex(px, s, OL_GLASS_RIM_SYMMETRIC, 0);
    glass_rim_gain_override = -1;
    int changed = 0;
    for (int i = 0; i < SW * SH; i++) changed += px[i] != source[i];
    return changed;
}

static int count_diff(const uint32_t *p, const uint32_t *q)
{
    int n = 0;
    for (int i = 0; i < SW * SH; i++) n += p[i] != q[i];
    return n;
}

/* The environment reflection is gated by the variance of the blurred rim
 * band: off over a flat backdrop (the measured render equals the gain-0
 * render bit for bit), on over busy content (it differs from gain 0 and is
 * closer to the gain-256 render than gain 0 is). */
static void test_variance_gate(void)
{
    struct scene s = {(SW - 80) / 2, (SH - 28) / 2, 80, 28, 14, 0, 0xFFFFFF, 50, 0};
    static uint32_t zero[SW * SH], full[SW * SH];

    flat(backdrop, 0xECECEC);
    render_gain(a, backdrop, &s, -1);
    render_gain(zero, backdrop, &s, 0);
    render_gain(full, backdrop, &s, 256);
    int flat_zero = count_diff(a, zero), flat_full = count_diff(a, full);
    printf("rim gain flat: measured vs gain0 %d px, vs gain256 %d px\n", flat_zero, flat_full);
    check(flat_zero == 0, "rim-gain-flat-off");

    stripes(backdrop, 0xECECEC, 0x303030, 12);
    render_gain(a, backdrop, &s, -1);
    render_gain(zero, backdrop, &s, 0);
    render_gain(full, backdrop, &s, 256);
    int busy_zero = count_diff(a, zero), busy_full = count_diff(a, full);
    int full_zero = count_diff(full, zero);
    printf("rim gain busy: measured vs gain0 %d px, vs gain256 %d px, gain256 vs gain0 %d px\n",
           busy_zero, busy_full, full_zero);
    check(busy_zero > 0, "rim-gain-busy-on");
    check(full_zero > 0 && busy_full < full_zero, "rim-gain-busy-strong");
}

static void test_striped(void)
{
    /* Refraction and the variance-gated reflection still act over content:
     * the rim over stripes is not the rim over their flat mean. */
    struct scene s = {(SW - 80) / 2, (SH - 28) / 2, 80, 28, 14, 0, 0xFFFFFF, 50, 0};
    struct ol_glass_material sym = {.rim = OL_GLASS_RIM_SYMMETRIC};
    int edge = glass_panel_edge_width(s.w, s.h, s.r, &sym);
    stripes(a, 0xECECEC, 0x505050, 5);
    render_ex(a, &s, OL_GLASS_RIM_SYMMETRIC, 0);
    flat(b, 0x9E9E9E);
    render_ex(b, &s, OL_GLASS_RIM_SYMMETRIC, 0);
    int rim = 0, differ = 0, lo = 255, hi = 0;
    for (int row = s.y; row < s.y + s.h; row++)
        for (int column = s.x; column < s.x + s.w; column++) {
            if (!in_rim(&s, edge, column, row)) continue;
            rim++;
            int la = luma(a[row * SW + column]), lb = luma(b[row * SW + column]);
            if (la != lb) differ++;
            if (la < lo) lo = la;
            if (la > hi) hi = la;
        }
    printf("striped rim: %d of %d rim pixels differ from flat, luma %d..%d\n",
           differ, rim, lo, hi);
    check(rim > 0 && differ * 2 > rim, "refraction-visible");
    check(hi - lo > 16, "refraction-contrast");
}

static void test_edge_floor(void)
{
    struct ol_glass_material sym = {.rim = OL_GLASS_RIM_SYMMETRIC};
    struct ol_glass_material dir = {.rim = OL_GLASS_RIM_DIRECTIONAL};
    int ok = glass_panel_edge_width(80, 28, 14, &sym) == 14 &&
             glass_panel_edge_width(30, 12, 2, &sym) == 2 &&
             glass_panel_edge_width(30, 12, 0, &sym) == 2 &&
             glass_panel_edge_width(60, 39, 3, &sym) == 3 &&
             glass_panel_edge_width(60, 39, 1, &sym) == 2;
    printf("edge floor: sym 30x12 r2=%d dir=%d null=%d sym 60x40 r2=%d\n",
           glass_panel_edge_width(30, 12, 2, &sym), glass_panel_edge_width(30, 12, 2, &dir),
           glass_panel_edge_width(30, 12, 2, 0), glass_panel_edge_width(60, 40, 2, &sym));
    check(ok, "edge-floor");
    /* Panels of 40px and more, and every DIRECTIONAL panel, keep 4px. */
    check(glass_panel_edge_width(30, 12, 2, &dir) == 4 &&
          glass_panel_edge_width(30, 12, 2, 0) == 4 &&
          glass_panel_edge_width(60, 40, 2, &sym) == 4 &&
          glass_panel_edge_width(200, 120, 24, &sym) == 22, "edge-floor-unchanged");
    /* The halo still covers every sample of the narrower lens. */
    check(ol_display_glass_sample_halo_ex(30, 12, 2, &sym) >=
          glass_blur_support(&sym) + GLASS_REFLECT_OFFSET, "edge-floor-halo");

    /* A small SYMMETRIC chip over flat white keeps the no-crescent promise. */
    struct scene s = {40, 40, 30, 12, 2, 0, 0xFFFFFF, 50, 0};
    int spread, lowest, highest, darkest;
    flat(a, 0xECECEC);
    render_ex(a, &s, OL_GLASS_RIM_SYMMETRIC, 0);
    rim_stats(a, &s, 2, &spread, &lowest, &highest, &darkest);
    printf("rim 30x12 r2: min=%d max=%d spread=%d darkest=%d\n", lowest, highest,
           spread, darkest);
    check(darkest >= luma(0xECECEC) - 4, "rim-dark 30x12");
}

static const struct scene identity_scenes[] = {
    {20, 20, 80, 28, 14, 0, 0xFFFFFF, 50, 0},
    {20, 60, 100, 35, 17, 0, 0xFFFFFF, 50, 0},
    {30, 40, 120, 42, 21, 0, 0x22242E, 120, 0},
    {10, 10, 30, 12, 2, 0, 0xFFFFFF, 50, 0},
    {50, 50, 24, 16, 0, 0, 0x808080, 90, 0},
    {40, 30, 200, 120, 24, 0, 0xE0E8F0, 120, 0},
    {-10, 100, 90, 50, 18, 0, 0x102030, 200, OL_GLASS_BLUR_THIN},
    {60, 40, 160, 80, 20, 3, 0x404040, 90, 0},
    {100, 60, 60, 60, 30, 0, 0x336699, 0, 0},
    {150, 90, 70, 40, 12, 0, 0xF0F0F0, 160, OL_GLASS_BLUR_THICK},
};

/* FNV-1a over every DIRECTIONAL render below, recorded on the base commit
 * (167205a35) before SYMMETRIC existed. */
#define GSR_DIRECTIONAL_HASH UINT64_C(0xbf994c98f74375fe)

static void test_directional_identity(void)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    int same = 1;
    for (int pattern = 0; pattern < 3; pattern++) {
        for (unsigned i = 0; i < sizeof identity_scenes / sizeof *identity_scenes; i++) {
            const struct scene *s = &identity_scenes[i];
            if (pattern == 0) patterned(backdrop);
            else if (pattern == 1) flat(backdrop, 0xECECEC);
            else stripes(backdrop, 0xECECEC, 0x505050, 5);
            memcpy(b, backdrop, sizeof backdrop);
            render_ex(b, s, OL_GLASS_RIM_DIRECTIONAL, 0);
            if (s->blur == OL_GLASS_BLUR_REGULAR) {
                memcpy(a, backdrop, sizeof backdrop);
                render_legacy(a, s);
                if (memcmp(a, b, sizeof a)) same = 0;
            }
            hash = fnv(hash, b);
        }
    }
    printf("directional hash: %016llx\n", (unsigned long long)hash);
    check(same, "directional-legacy-identical");
    check(hash == GSR_DIRECTIONAL_HASH, "directional-bit-identical");
}

static void test_reduce_transparency(void)
{
    int same = 1;
    patterned(backdrop);
    for (unsigned i = 0; i < sizeof identity_scenes / sizeof *identity_scenes; i++) {
        memcpy(a, backdrop, sizeof backdrop);
        memcpy(b, backdrop, sizeof backdrop);
        render_ex(a, &identity_scenes[i], OL_GLASS_RIM_DIRECTIONAL, 1);
        render_ex(b, &identity_scenes[i], OL_GLASS_RIM_SYMMETRIC, 1);
        if (memcmp(a, b, sizeof a)) same = 0;
    }
    check(same, "reduce-transparency-solid");
}

/* The rim statistic reads the whole halo, so a damage repaint through a
 * partial write clip produces the pixels a full frame does. */
static void test_clip_repaint(void)
{
    static const struct { int x0, y0, x1, y1; } clips[] = {
        {0, 0, 130, 160}, {120, 70, 240, 160}, {90, 75, 110, 85},
    };
    struct scene s = {70, 60, 100, 35, 17, 0, 0xFFFFFF, 50, 0};
    int same = 1;
    patterned(backdrop);
    memcpy(a, backdrop, sizeof backdrop);
    render_ex(a, &s, OL_GLASS_RIM_SYMMETRIC, 0);
    for (unsigned i = 0; i < sizeof clips / sizeof *clips; i++) {
        struct ol_display d;
        struct ol_pixel_target t;
        struct ol_glass_material mat = {.tint = s.tint, .opacity = s.opacity,
                                        .rim = OL_GLASS_RIM_SYMMETRIC};
        memcpy(b, backdrop, sizeof backdrop);
        bind(&d, &t, b);
        d.target.clip_on = 1;  /* bind copied the target */
        d.target.clx0 = clips[i].x0;
        d.target.cly0 = clips[i].y0;
        d.target.clx1 = clips[i].x1;
        d.target.cly1 = clips[i].y1;
        ol_display_liquid_glass_ex(&d, s.x, s.y, s.w, s.h, s.r, 0, &mat);
        ol_display_destroy(&d);
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                int inside = x >= clips[i].x0 && x < clips[i].x1 &&
                             y >= clips[i].y0 && y < clips[i].y1;
                uint32_t want = inside ? a[y * SW + x] : backdrop[y * SW + x];
                if (b[y * SW + x] != want) {
                    if (same) printf("clip %u: first mismatch at %d,%d inside=%d got %06x want %06x\n",
                                     i, x, y, inside, b[y * SW + x], want);
                    same = 0;
                }
            }
    }
    check(same, "clip-repaint-identical");
}

int main(void)
{
    test_directional_identity();
    test_uniform("80x28 r14", 80, 28, 14);
    test_uniform("100x35 r17 (125%)", 100, 35, 17);
    test_uniform("120x42 r21 (150%)", 120, 42, 21);
    test_striped();
    test_variance_gate();
    test_edge_floor();
    test_reduce_transparency();
    test_clip_repaint();
    printf("glass rim: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
