/* The material's contract is readable content over a changing backdrop, not
 * a copy of its adaptive-alpha formula. Probe the flat interior, where the
 * rim does not provide accidental contrast, through the complete renderer.
 * The original fixed tint fails the dark-on-white and light-on-black cases.
 */
#include "openlogit_display.h"
#include "glass_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 160, HEIGHT = 128, PIXELS = WIDTH * HEIGHT };
struct color { unsigned char r, g, b; };
static int checks, failures;
static uint64_t patterned_hash = UINT64_C(14695981039346656037);
static int scratch_allocations;

static void *counted_alloc(unsigned long bytes)
{
    scratch_allocations++;
    return malloc(bytes);
}

static void check(int condition, const char *name)
{
    checks++;
    if (!condition) {
        failures++;
        printf("FAIL %s\n", name);
    }
}

static uint32_t rgb(struct color color)
{
    return (uint32_t)color.r << 16 | (uint32_t)color.g << 8 | color.b;
}

static struct color color_at(uint32_t pixel)
{
    return (struct color){(pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255};
}

static void fill(uint32_t *pixels, struct color color)
{
    for (int i = 0; i < PIXELS; i++) pixels[i] = rgb(color);
}

/* Hash every rendered pixel, including the curved rim and untouched backdrop.
 * A uniform center sample misses refraction and the fast/general path seam.
 * Serialize explicitly so the comparison does not depend on host endianness. */
static void hash_pixels(const uint32_t *pixels)
{
    for (int i = 0; i < PIXELS; i++) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            patterned_hash ^= (pixels[i] >> shift) & 255;
            patterned_hash *= UINT64_C(1099511628211);
        }
    }
}

static void draw(uint32_t *pixels, struct color tint, unsigned char alpha,
                 int clipped, unsigned cut)
{
    struct ol_display display = {.allocate = malloc, .release = free};
    struct ol_pixel_target target = {
        .px = pixels, .w = WIDTH, .h = HEIGHT,
        .clip_on = clipped, .clx0 = 31, .cly0 = 27, .clx1 = 123, .cly1 = 101,
    };
    if (ol_display_bind(&display, &target, 16, 8, 0) != OL_OK) {
        fprintf(stderr, "glass fixture could not bind its pixel target\n");
        exit(2);
    }
    ol_display_liquid_glass_cut(&display, 16, 16, 128, 96, 18,
                               tint.r, tint.g, tint.b, alpha, cut);
    if (display.error) {
        fprintf(stderr, "glass fixture render error: %d\n", display.error);
        exit(2);
    }
    ol_display_destroy(&display);
}

static struct color sample(struct color background, struct color tint, unsigned char alpha)
{
    uint32_t pixels[PIXELS];
    fill(pixels, background);
    draw(pixels, tint, alpha, 0, 0);
    return color_at(pixels[64 * WIDTH + 80]);
}

/* The mask has the same pixels in both scenes. If the edge never responds to
 * the ring outside it, the renderer sampled its own rectangle rather than the
 * neighbouring content that a floating material is meant to reveal. The
 * inner-change control ensures the comparison actually reached glass. */
static void neighbouring_backdrop(void)
{
    uint32_t black_ring[PIXELS], white_ring[PIXELS], darker_body[PIXELS];
    fill(black_ring, (struct color){0, 0, 0});
    fill(white_ring, (struct color){255, 255, 255});
    fill(darker_body, (struct color){0, 0, 0});
    for (int y = 16; y < 112; y++) {
        for (int x = 16; x < 144; x++) {
            black_ring[y * WIDTH + x] = white_ring[y * WIDTH + x] =
                rgb((struct color){128, 128, 128});
            darker_body[y * WIDTH + x] = rgb((struct color){64, 64, 64});
        }
    }
    struct color tint = {255, 255, 255};
    draw(black_ring, tint, 0, 0, 0);
    draw(white_ring, tint, 0, 0, 0);
    draw(darker_body, tint, 0, 0, 0);

    int ring_changes = 0, body_changes = 0;
    for (int y = 16; y < 112; y++) {
        for (int x = 16; x < 144; x++) {
            int index = y * WIDTH + x;
            ring_changes += black_ring[index] != white_ring[index];
            body_changes += black_ring[index] != darker_body[index];
        }
    }
    printf("neighbouring backdrop: ring=%d body=%d\n", ring_changes, body_changes);
    check(body_changes > 1000, "control changes glass when its own backdrop changes");
    check(ring_changes > 0, "glass rim samples the neighbouring backdrop");
}

static void sample_region_cost(void)
{
    uint32_t pixels[PIXELS];
    fill(pixels, (struct color){100, 120, 140});
    struct ol_display display = {.allocate = malloc, .release = free};
    struct ol_pixel_target target = {.px = pixels, .w = WIDTH, .h = HEIGHT};
    if (ol_display_bind(&display, &target, 16, 8, 0) != OL_OK) exit(2);
    ol_display_liquid_glass(&display, 72, 56, 16, 16, 8,
                            250, 250, 255, 80);
    int scratch = display.glass_buf_n;
    check(!display.error, "small glass panel can reserve its sample region");
    check(scratch >= 16 * 16 && scratch < PIXELS / 4,
          "small glass panel keeps scratch local to its neighbourhood");
    ol_display_destroy(&display);
}

static void sample_reach_bound(void)
{
    for (int edge = 4; edge <= 22; edge++) {
        int refract = edge < 18 ? edge : 18;
        int halo = ol_display_glass_sample_halo(edge * 2 + 4,
                                                  edge * 2 + 4, edge);
        glass_build_lut(edge, refract);
        int displacement = GLASS_REFLECT_OFFSET;
        for (int channel = 0; channel < 3; channel++)
            for (int depth = 0; depth <= edge; depth++)
                if (glass_disp[channel][depth] > displacement)
                    displacement = glass_disp[channel][depth];
        check(halo >= GLASS_BLUR_RADIUS + displacement,
              "damage halo covers every blurred and refracted sample");
    }
}

/* These averages concern nearly neutral colors, not a color-space oracle.
 * Leave several levels for the material's existing environment reflection.
 * The bounds express a useful separation from black/white content without
 * dictating the opacity curve or its integer rounding at each background.
 */
static void neutral_contrast(void)
{
    const unsigned char alphas[] = {8, 64, 128, 224, 254};
    const struct color tints[] = {{250, 250, 255}, {24, 28, 36}};
    uint32_t pixels[PIXELS];
    int darkest_light = 255, brightest_dark = 0;
    for (unsigned kind = 0; kind < 2; kind++) {
        for (unsigned alpha = 0; alpha < sizeof alphas; alpha++) {
            for (int gray = 0; gray <= 255; gray += 17) {
                fill(pixels, (struct color){gray, gray, gray});
                draw(pixels, tints[kind], alphas[alpha], 0, 0);
                int low = 255, high = 0;
                /* Includes both straight-row and general-column rendering
                 * paths, while remaining beyond the 22-pixel optical rim. */
                for (int y = 43; y <= 85; y++) {
                    for (int x = 43; x <= 117; x++) {
                        struct color c = color_at(pixels[y * WIDTH + x]);
                        int value = (c.r + c.g + c.b) / 3;
                        if (value < low) low = value;
                        if (value > high) high = value;
                    }
                }
                if (!kind && low < darkest_light) darkest_light = low;
                if (kind && high > brightest_dark) brightest_dark = high;
                char name[128];
                snprintf(name, sizeof name, "%s neutral body keeps content contrast: gray=%d alpha=%u range=%d..%d",
                         kind ? "dark" : "light", gray, alphas[alpha], low, high);
                check(kind ? high <= 96 : low >= 168, name);
            }
        }
    }
    printf("neutral body observed: light minimum=%d, dark maximum=%d\n",
           darkest_light, brightest_dark);
}

static void transparency_and_color(void)
{
    struct color light = {250, 250, 250}, dark = {24, 24, 24};
    struct color a = sample((struct color){200, 200, 200}, light, 16);
    struct color b = sample((struct color){224, 224, 224}, light, 16);
    check(a.r < 216 && b.r > a.r + 12,
          "readable light backdrop stays translucent and distinguishable");
    a = sample((struct color){16, 16, 16}, dark, 16);
    b = sample((struct color){48, 48, 48}, dark, 16);
    check(a.r < 32 && b.r > a.r + 20,
          "readable dark backdrop stays translucent and distinguishable");

    a = sample((struct color){0, 0, 0}, light, 16);
    b = sample((struct color){0, 0, 0}, light, 224);
    check(b.r > a.r + 24, "explicit stronger light tint remains an opacity floor");
    a = sample((struct color){255, 255, 255}, dark, 16);
    b = sample((struct color){255, 255, 255}, dark, 224);
    check(a.r > b.r + 24, "explicit stronger dark tint remains an opacity floor");

    a = sample((struct color){40, 120, 220}, light, 64);
    check(a.r + 20 < a.g && a.g + 20 < a.b,
          "light neutral glass retains backdrop blue chroma");
    a = sample((struct color){240, 200, 120}, dark, 64);
    check(a.r > a.g + 10 && a.g > a.b + 20,
          "dark neutral glass retains backdrop warm chroma");

    /* Saturated and midtone tints are decorative, not regular neutral
     * materials. A low requested alpha must not become an opaque color slab. */
    a = sample((struct color){0, 0, 0}, (struct color){250, 20, 20}, 32);
    check(a.r < 64 && a.r > a.g + 20 && a.g == a.b,
          "colored tint keeps its requested translucent red character");
    a = sample((struct color){0, 0, 0}, (struct color){255, 255, 128}, 32);
    check(a.r < 64 && a.r == a.g && a.r > a.b + 10,
          "bright colored tint stays translucent over a dark backdrop");
    a = sample((struct color){255, 255, 255}, (struct color){0, 0, 120}, 32);
    check(a.r > 210 && a.r == a.g && a.b > a.r + 10,
          "dark colored tint stays translucent over a bright backdrop");
    a = sample((struct color){0, 0, 0}, (struct color){128, 128, 128}, 32);
    b = sample((struct color){255, 255, 255}, (struct color){128, 128, 128}, 32);
    check(a.r < 32 && b.r > 220,
          "midtone tint does not opt into regular material adaptation");
}

static void endpoints(void)
{
    const struct color tints[] = {{250, 250, 250}, {24, 24, 24}, {250, 20, 20}};
    const struct color backdrops[] = {{0, 0, 0}, {255, 255, 255}, {35, 125, 215}};
    for (unsigned i = 0; i < sizeof backdrops / sizeof *backdrops; i++) {
        struct color background = backdrops[i];
        struct color clear = sample(background, tints[0], 0);
        for (unsigned t = 0; t < sizeof tints / sizeof *tints; t++) {
            struct color got = sample(background, tints[t], 0);
            check(!memcmp(&got, &clear, sizeof got),
                  "Clear alpha zero ignores the requested tint");
        }
        check(abs((int)clear.r - background.r) <= 8 &&
              abs((int)clear.g - background.g) <= 8 &&
              abs((int)clear.b - background.b) <= 8,
              "Clear retains its backdrop with only the existing surface reflection");
    }
    for (unsigned t = 0; t < sizeof tints / sizeof *tints; t++) {
        struct color black = sample((struct color){0, 0, 0}, tints[t], 255);
        struct color white = sample((struct color){255, 255, 255}, tints[t], 255);
        check(!memcmp(&black, &white, sizeof black),
              "opaque body tint remains independent of backdrop");
        check(abs((int)black.r - tints[t].r) <= 8 &&
              abs((int)black.g - tints[t].g) <= 8 &&
              abs((int)black.b - tints[t].b) <= 8,
              "opaque body retains the caller color under the existing reflection");
    }
}

static void clip_parity(void)
{
    uint32_t backdrop[PIXELS], whole[PIXELS], clipped[PIXELS];
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            struct color c = {(x * 5 + y * 3) & 255,
                              (y * 7 + 19) & 255, (x * 3 + y * 11) & 255};
            backdrop[y * WIDTH + x] = rgb(c);
        }
    }
    const unsigned cuts[] = {0, OL_GLASS_CUT_TOP, OL_GLASS_CUT_LEFT | OL_GLASS_CUT_BOTTOM};
    const struct color tints[] = {{250, 250, 255}, {24, 28, 36}, {220, 40, 90}};
    for (unsigned t = 0; t < sizeof tints / sizeof *tints; t++) {
        for (unsigned cut = 0; cut < sizeof cuts / sizeof *cuts; cut++) {
            memcpy(whole, backdrop, sizeof whole);
            memcpy(clipped, backdrop, sizeof clipped);
            draw(whole, tints[t], 64, 0, cuts[cut]);
            draw(clipped, tints[t], 64, 1, cuts[cut]);
            hash_pixels(whole);
            hash_pixels(clipped);
            int inside = 0, outside = 0;
            for (int y = 0; y < HEIGHT; y++) {
                for (int x = 0; x < WIDTH; x++) {
                    int at = y * WIDTH + x;
                    if (x >= 31 && x < 123 && y >= 27 && y < 101)
                        inside += clipped[at] != whole[at];
                    else
                        outside += clipped[at] != backdrop[at];
                }
            }
            check(!inside, "material color is identical inside partial damage clip");
            check(!outside, "material leaves pixels outside damage clip untouched");
        }
    }
}

static void draw_solid_at(uint32_t *pixels, int x, int radius, int clipped, unsigned cut)
{
    struct ol_display display = {
        .allocate = counted_alloc, .release = free,
        .reduce_transparency = 1,
    };
    struct ol_pixel_target target = {
        .px = pixels, .w = WIDTH, .h = HEIGHT,
        .clip_on = clipped, .clx0 = 31, .cly0 = 27, .clx1 = 123, .cly1 = 101,
    };
    if (ol_display_bind(&display, &target, 16, 8, 0) != OL_OK) {
        fprintf(stderr, "solid glass fixture could not bind target\n");
        exit(2);
    }
    ol_display_liquid_glass_cut(&display, x, 16, 128, 96, radius,
                                230, 235, 240, 16, cut);
    check(display.error == OL_OK, "reduced transparency renderer reports no error");
    ol_display_destroy(&display);
}

static void draw_solid(uint32_t *pixels, int radius, int clipped, unsigned cut)
{
    draw_solid_at(pixels, 16, radius, clipped, cut);
}

static void reduced_transparency(void)
{
    uint32_t first[PIXELS], second[PIXELS], clipped[PIXELS];
    const uint32_t tint = rgb((struct color){230, 235, 240});

    /* Black and white are adversarial backdrops: a stronger alpha or a blur
     * still depends on them, whereas a solid interior cannot. The zero-alpha
     * fringe at a curved outline is outside the panel, not its background. */
    for (int radius = 0; radius <= 18; radius += 18) {
        fill(first, (struct color){0, 0, 0});
        fill(second, (struct color){255, 255, 255});
        scratch_allocations = 0;
        draw_solid(first, radius, 0, 0);
        draw_solid(second, radius, 0, 0);
        check(first[64 * WIDTH + 80] == tint && second[64 * WIDTH + 80] == tint,
              "reduced transparency interior is opaque on both backdrops");
        check(scratch_allocations == 0,
              "reduced transparency skips backdrop blur allocation");
        check(first[0] == 0 && second[0] == 0xFFFFFF,
              "reduced transparency leaves outside pixels unchanged");
    }

    fill(first, (struct color){0, 0, 0});
    draw_solid(first, 18, 0, 0);
    check(first[16 * WIDTH + 16] == 0,
          "solid rounded panel preserves its free corner");
    fill(first, (struct color){0, 0, 0});
    draw_solid(first, 18, 0, OL_GLASS_CUT_TOP | OL_GLASS_CUT_LEFT);
    check(first[16 * WIDTH + 16] == tint,
          "solid cut edges join without an invented rounded corner");

    fill(first, (struct color){40, 80, 120});
    memcpy(clipped, first, sizeof clipped);
    draw_solid(first, 18, 0, OL_GLASS_CUT_TOP);
    draw_solid(clipped, 18, 1, OL_GLASS_CUT_TOP);
    int changed_inside = 0, changed_outside = 0;
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int at = y * WIDTH + x;
            if (x >= 31 && x < 123 && y >= 27 && y < 101)
                changed_inside += clipped[at] != first[at];
            else
                changed_outside += clipped[at] != rgb((struct color){40, 80, 120});
        }
    }
    check(changed_inside == 0, "solid glass matches full render inside damage clip");
    check(changed_outside == 0, "solid glass leaves outside damage clip untouched");

    /* The visible rectangle begins at screen x=0 while the panel began at
     * x=-12. Both the geometry and destination pointer must remain valid. */
    fill(first, (struct color){40, 80, 120});
    fill(clipped, (struct color){40, 80, 120});
    draw_solid_at(first, -12, 18, 0, 0);
    draw_solid_at(clipped, -12, 18, 1, 0);
    check(first[64 * WIDTH] == tint, "cropped solid panel fills its visible interior");
    check(clipped[64 * WIDTH + 40] == first[64 * WIDTH + 40],
          "cropped solid panel preserves original geometry inside clip");
    check(clipped[64 * WIDTH] == rgb((struct color){40, 80, 120}),
          "cropped solid panel does not write outside clip");
}

int main(void)
{
    neighbouring_backdrop();
    sample_region_cost();
    sample_reach_bound();
    neutral_contrast();
    transparency_and_color();
    endpoints();
    clip_parity();
    reduced_transparency();
    printf("glass patterned hash: %016llx\n", (unsigned long long)patterned_hash);
    printf("glass material: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
