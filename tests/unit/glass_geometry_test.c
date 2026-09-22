/* Real display rendering, with two independent geometric oracles.
 *
 * Integer roots use division-based binary search, not the renderer's restoring
 * algorithm or floating sqrt (which rounds UINT64_MAX up to 2^32). Cropping
 * compares a fully rendered panel with the same panel hanging off a smaller
 * surface. A uniform backdrop makes unavailable offscreen blur samples
 * irrelevant: this tests geometry, not an invented policy for missing pixels.
 * It does not establish visual similarity to another platform's material. */
#include "openlogit_display.h"
#include "openlogit_glass.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GUARD = 16, MARGIN = 96 };
static const uint32_t background = 0x003d729f, canary = 0xd15ea5ed;
static int checks, failures, root_failures, crop_failures, clip_failures;
static uint64_t raster_hash = UINT64_C(14695981039346656037);
static int allocation_count, allocation_failure;

struct image {
    int w, h;
    uint32_t *allocation, *pixels;
    struct ol_display display;
};

static void check(int passed, const char *kind, const char *name)
{
    checks++;
    if (!passed) {
        failures++;
        printf("FAIL %s %s\n", kind, name);
    }
}

static uint64_t exact_root(uint64_t value)
{
    uint64_t low = 0, high = UINT32_MAX;
    while (low < high) {
        uint64_t middle = low + (high - low + 1) / 2;
        if (middle <= value / middle) low = middle;
        else high = middle - 1;
    }
    return low;
}

static void root_case(uint64_t value)
{
    uint64_t wanted = exact_root(value);
    unsigned actual = gl_isqrt((unsigned long)value);
    checks++;
    if (actual != wanted) {
        failures++;
        if (root_failures++ < 8) {
            printf("FAIL sqrt input=%" PRIu64 " actual=%u expected=%" PRIu64 "\n",
                   value, actual, wanted);
        }
    }
}

static void roots(void)
{
    _Static_assert(sizeof(unsigned long) * CHAR_BIT == 64, "LogitOS uses 64-bit unsigned long");
    static const uint64_t boundaries[] = {
        0, 1, 2, 255, 256, 65535, 65536, 16777216, 2147483648, UINT32_MAX
    };
    for (unsigned i = 0; i < sizeof boundaries / sizeof boundaries[0]; i++) {
        uint64_t square = boundaries[i] * boundaries[i];
        if (square) root_case(square - 1);
        root_case(square);
        root_case(square + 1);
    }
    root_case(UINT64_C(1) << 32);
    root_case((UINT64_C(256) * 256) << 16);
    root_case(ULONG_MAX);
    uint64_t value = UINT64_C(0x123456789abcdef0);
    for (int i = 0; i < 128; i++) {
        value = value * UINT64_C(6364136223846793005) + 1;
        root_case(value);
    }
}

static void *allocate(unsigned long bytes)
{
    allocation_count++;
    return allocation_count == allocation_failure ? NULL : malloc(bytes);
}

static struct image image_create(int w, int h)
{
    struct image image = {.w = w, .h = h};
    size_t count = (size_t)w * h;
    image.allocation = malloc((count + 2 * GUARD) * sizeof(uint32_t));
    if (!image.allocation) { fputs("FAIL allocation\n", stderr); exit(2); }
    image.pixels = image.allocation + GUARD;
    for (int i = 0; i < GUARD; i++) {
        image.allocation[i] = image.pixels[count + i] = canary;
    }
    for (size_t i = 0; i < count; i++) image.pixels[i] = background;
    image.display.allocate = allocate;
    image.display.release = free;
    struct ol_pixel_target target = {.px = image.pixels, .w = w, .h = h};
    if (ol_display_bind(&image.display, &target, 16, 8, 0) != OL_OK) {
        fputs("FAIL display bind\n", stderr);
        exit(2);
    }
    return image;
}

static void image_destroy(struct image *image, const char *name)
{
    int intact = 1;
    size_t count = (size_t)image->w * image->h;
    for (int i = 0; i < GUARD; i++) {
        if (image->allocation[i] != canary || image->pixels[count + i] != canary) intact = 0;
    }
    check(intact, "bounds", name);
    check(image->display.error == OL_OK, "renderer", name);
    /* Hash every native pixel, not just changed pixels or a sample. Building
     * the same fixture with GLASS_FIELD_SLOW proves the optimized row path
     * produces the complete same raster for clipped and offscreen panels. */
    for (size_t i = 0; i < count; i++) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            raster_hash ^= (image->pixels[i] >> shift) & 255;
            raster_hash *= UINT64_C(1099511628211);
        }
    }
    ol_display_destroy(&image->display);
    free(image->allocation);
}

static void draw(struct image *image, int x, int y, int w, int h, int radius, unsigned cut)
{
    ol_display_liquid_glass_cut(&image->display, x, y, w, h, radius,
                                210, 225, 250, 92, cut);
}

static void crop_case(int w, int h, int radius, unsigned cut, int side)
{
    static const char *sides[] = {"left", "right", "top", "bottom", "top-left",
                                  "bottom-right", "all-edges"};
    int sw = w + 48, sh = h + 48, x = 17, y = 17;
    switch (side) {
    case 0: x = -31; break;
    case 1: x = 79; break;
    case 2: y = -23; break;
    case 3: y = 71; break;
    case 4: x = -31; y = -23; break;
    case 5: x = 79; y = 71; break;
    default: x = -31; y = -9; sw = w - 54; sh = h - 18; break;
    }
    char name[128];
    snprintf(name, sizeof name, "%dx%d radius=%d cut=%u %s", w, h, radius, cut, sides[side]);
    struct image full = image_create(w + 2 * MARGIN, h + 2 * MARGIN);
    struct image cropped = image_create(sw, sh);
    struct image clipped = image_create(sw, sh);
    draw(&full, MARGIN, MARGIN, w, h, radius, cut);
    draw(&cropped, x, y, w, h, radius, cut);
    struct ol_pixel_target *clip = &clipped.display.target;
    clip->clip_on = 1;
    clip->clx0 = sw / 5; clip->clx1 = sw - sw / 7;
    clip->cly0 = sh / 4; clip->cly1 = sh - sh / 5;
    draw(&clipped, x, y, w, h, radius, cut);

    int crop_bad = 0, clip_bad = 0, outside_bad = 0, changed = 0;
    int first_x = -1, first_y = -1;
    for (int row = 0; row < sh; row++) {
        for (int col = 0; col < sw; col++) {
            size_t at = (size_t)row * sw + col;
            uint32_t wanted = full.pixels[(size_t)(MARGIN - y + row) * full.w + MARGIN - x + col];
            if (cropped.pixels[at] != wanted) {
                crop_bad++;
                if (first_x < 0) { first_x = col; first_y = row; }
            }
            changed += wanted != background;
            int inside = col >= clip->clx0 && col < clip->clx1 && row >= clip->cly0 && row < clip->cly1;
            if (inside) clip_bad += clipped.pixels[at] != wanted;
            else outside_bad += clipped.pixels[at] != background;
        }
    }
    checks++;
    if (crop_bad) {
        failures++;
        if (crop_failures++ < 12) {
            printf("FAIL crop %s pixels=%d first=(%d,%d)\n", name, crop_bad, first_x, first_y);
        }
    }
    checks++;
    if (clip_bad || outside_bad) {
        failures++;
        if (clip_failures++ < 12) {
            printf("FAIL clip %s inside=%d outside=%d\n", name, clip_bad, outside_bad);
        }
    }
    check(changed > 0, "paint", name);
    if (!cut && radius >= 256) {
        /* An independently known outside point, so two equally broken roots
         * cannot make the full-render/crop equality pass the large-radius case. */
        check(full.pixels[(size_t)MARGIN * full.w + MARGIN] == background, "round-corner", name);
    }
    image_destroy(&full, name);
    image_destroy(&cropped, name);
    image_destroy(&clipped, name);
}

static void square_case(void)
{
    enum { X = 8, Y = 9, W = 31, H = 21 };
    struct image image = image_create(55, 43);
    size_t count = (size_t)image.w * image.h;
    for (size_t i = 0; i < count; i++) image.pixels[i] = 0;

    /* The Browser DevTools requests radius 0 for a square dark panel. The
     * former material guard silently drew nothing. Checking only that some
     * pixels changed would also bless radius 1, whose half-covered corners
     * are not a square; opaque white on black exposes that difference. */
    ol_display_liquid_glass(&image.display, X, Y, W, H, 0, 255, 255, 255, 255);
    int changed = 0, outside = 0, corners = 0;
    for (int row = 0; row < image.h; row++) {
        for (int col = 0; col < image.w; col++) {
            uint32_t pixel = image.pixels[(size_t)row * image.w + col];
            int inside = col >= X && col < X + W && row >= Y && row < Y + H;
            if (inside) changed += pixel != 0;
            else outside += pixel != 0;
        }
    }
    const int cx[] = {X, X + W - 1, X, X + W - 1};
    const int cy[] = {Y, Y, Y + H - 1, Y + H - 1};
    int corner_red[4];
    for (int i = 0; i < 4; i++) {
        uint32_t pixel = image.pixels[(size_t)cy[i] * image.w + cx[i]];
        corner_red[i] = (pixel >> 16) & 255;
        corners += corner_red[i] > 96;
    }
    if (changed != W * H || corners != 4 || outside) {
        printf("square pixels: changed=%d/%d outside=%d corners=%d,%d,%d,%d\n",
               changed, W * H, outside, corner_red[0], corner_red[1],
               corner_red[2], corner_red[3]);
    }
    check(changed == W * H && corners == 4 && outside == 0,
          "square", "radius zero paints every pixel and all four full corners");
    image_destroy(&image, "square");
}

static uint32_t pattern_pixel(size_t index)
{
    return ((uint32_t)index * 1103515245u + 12345u) & 0xffffffu;
}

static void allocation_case(int side, int failure, int clipped)
{
    static const struct {
        const char *name;
        int x, y;
    } positions[] = {
        {"onscreen", 17, 17}, {"left", -140, 17}, {"right", 228, 17},
        {"top", 17, -84}, {"bottom", 17, 164},
    };
    int x = positions[side].x;
    int y = positions[side].y;
    char name[96];
    snprintf(name, sizeof name, "%s allocation=%d clip=%d", positions[side].name,
             failure, clipped);
    struct image expected = image_create(256, 192);
    struct image actual = image_create(256, 192);
    size_t count = (size_t)actual.w * actual.h;
    size_t bytes = count * sizeof(uint32_t);
    for (size_t i = 0; i < count; i++) {
        expected.pixels[i] = actual.pixels[i] = pattern_pixel(i);
    }
    if (clipped) {
        struct ol_pixel_target *target = &actual.display.target;
        target->clip_on = 1;
        target->clx0 = 7;
        target->cly0 = 9;
        target->clx1 = actual.w - 11;
        target->cly1 = actual.h - 13;
        expected.display.target = *target;
        expected.display.target.px = expected.pixels;
    }

    /* A uniform backdrop would hide the old blur fallback entirely. With
     * patterned pixels it changed the target after either scratch allocation
     * failed, including pixels outside an offscreen panel's original corners. */
    allocation_count = 0;
    allocation_failure = failure;
    draw(&actual, x, y, 160, 104, 28, 0);
    check(allocation_count >= failure, "allocation-injected", name);
    check(actual.display.error == OL_LIMIT, "allocation-error", name);
    check(memcmp(actual.pixels, expected.pixels, bytes) == 0, "allocation-write", name);

    /* Begin a fresh batch after memory recovers. Reset the pixels as well so
     * the negative control's earlier bad write cannot mask a recovery defect. */
    allocation_failure = 0;
    memcpy(actual.pixels, expected.pixels, bytes);
    struct ol_pixel_target target = actual.display.target;
    check(ol_display_bind(&actual.display, &target, 16, 8, 0) == OL_OK,
          "allocation-rebind", name);
    draw(&actual, x, y, 160, 104, 28, 0);
    draw(&expected, x, y, 160, 104, 28, 0);
    check(memcmp(actual.pixels, expected.pixels, bytes) == 0, "allocation-recovery", name);
    int changed = 0;
    for (size_t i = 0; i < count; i++) changed += actual.pixels[i] != pattern_pixel(i);
    check(changed > 0, "allocation-paint", name);
    image_destroy(&actual, name);
    image_destroy(&expected, name);
}

int main(void)
{
    roots();
    square_case();
    static const unsigned cuts[] = {
        0, OL_GLASS_CUT_TOP, OL_GLASS_CUT_BOTTOM, OL_GLASS_CUT_LEFT, OL_GLASS_CUT_RIGHT,
        OL_GLASS_CUT_TOP | OL_GLASS_CUT_LEFT,
        OL_GLASS_CUT_BOTTOM | OL_GLASS_CUT_RIGHT,
        OL_GLASS_CUT_TOP | OL_GLASS_CUT_BOTTOM | OL_GLASS_CUT_LEFT | OL_GLASS_CUT_RIGHT
    };
    for (unsigned i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
        for (int side = 0; side < 7; side++) crop_case(160, 104, 28, cuts[i], side);
    }
    for (int side = 0; side < 7; side++) {
        crop_case(160, 104, 0, 0, side);
        crop_case(160, 24, 12, 0, side);
        crop_case(512, 512, 256, 0, side);
        crop_case(1024, 1024, 512, 0, side);
    }
    for (int side = 0; side < 5; side++) {
        for (int failure = 1; failure <= 2; failure++) {
            allocation_case(side, failure, 0);
            allocation_case(side, failure, 1);
        }
    }
    printf("glass-geometry: %d checks, %d failed (sqrt=%d crop=%d clip=%d)\n",
           checks, failures, root_failures, crop_failures, clip_failures);
    printf("glass-raster: %016" PRIx64 "\n", raster_hash);
    return failures ? 1 : 0;
}
