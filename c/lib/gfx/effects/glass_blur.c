#include "glass_internal.h"

/* Window counts vary at the panel edges. These reciprocals preserve integer
 * division for every reachable byte sum without a divide in the pixel loop.
 * They are ceil(65536 / count), the legacy convention, and are sized for the
 * widest window a tier can use (2 * GLASS_BLUR_RADIUS_MAX + 1 samples). The
 * first 14 entries are the table REGULAR has always used. */
static const int reciprocals[2 * GLASS_BLUR_RADIUS_MAX + 2] = {
    0, 65536, 32768, 21846, 16384, 13108, 10923,
    9363, 8192, 7282, 6554, 5958, 5462, 5042,
    4682, 4370, 4096, 3856, 3641, 3450, 3277,
    3121, 2979, 2850, 2731, 2622, 2521, 2428,
    2341, 2260, 2185, 2115, 2048, 1986, 1928,
    1873, 1821, 1772, 1725, 1681, 1639, 1599,
};

/* Per-pass box radii of each tier, indexed by OL_GLASS_BLUR_*. Three passes
 * per axis approximate a gaussian; the radii (variance of a box pass of radius
 * r is r(r+1)/3) are chosen so the summed variance matches the tier's sigma:
 * THIN ~3 px, THICK ~12 px, ULTRA ~20 px. Mixed radii are used where equal
 * integer radii cannot hit the sigma (THIN). A zero entry ends the list.
 * REGULAR is the legacy single radius-6 pass. */
static const unsigned char tier_radii[4][3] = {
    [OL_GLASS_BLUR_REGULAR] = {GLASS_BLUR_RADIUS, 0, 0},
    [OL_GLASS_BLUR_THIN]    = {2, 2, 3},
    [OL_GLASS_BLUR_THICK]   = {11, 11, 11},
    [OL_GLASS_BLUR_ULTRA]   = {18, 18, 19},
};

static int tier_of(const struct ol_glass_material *mat)
{
    return mat && mat->blur <= OL_GLASS_BLUR_ULTRA ? mat->blur : OL_GLASS_BLUR_REGULAR;
}

/* Total reach of the blur kernel beyond a pixel: the sum of the pass radii. */
int glass_blur_support(const struct ol_glass_material *mat)
{
    int tier = tier_of(mat);
    int support = 0;
#ifdef GLASS_TIERS_NEGCTL_SINGLE_PASS
    /* Negative control: THICK collapses to one box pass. */
    if (tier == OL_GLASS_BLUR_THICK) return 11;
#endif
    for (int i = 0; i < 3; i++) support += tier_radii[tier][i];
    return support;
}

static inline uint8_t average(int sum, int count)
{
    return (uint8_t)((sum * reciprocals[count]) >> 16);
}

/* One horizontal box pass. The first pass reads the live backdrop and writes
 * the scratch buffer; later passes run in place and keep the last radius + 1
 * source pixels in a ring, because the pixel leaving the window has already
 * been overwritten. */
static void blur_rows(struct glass_panel *panel, int radius, int first)
{
    struct ol_display *display = panel->display;
    const struct ol_pixel_target *target = &display->target;
    uint32_t *pixels = display->glass_buf;
    int width = panel->sample_width;

    /* Read the live backdrop and write separate scratch storage. No source
     * pixel changes before it leaves the horizontal moving window. */
    for (int row = 0; row < panel->sample_height; row++) {
        uint32_t *destination = pixels + (long)row * width;
        const uint32_t *source = destination;
        if (first) {
            long start = (long)(panel->sample_y + row) * target->w + panel->sample_x;
            source = target->px + start;
        }
        int sum_red = 0;
        int sum_green = 0;
        int sum_blue = 0;
        int count = 0;
        int red;
        int green;
        int blue;
        uint32_t originals[GLASS_BLUR_RADIUS_MAX + 1];
        int slot = 0;

        for (int column = 0; column <= radius && column < width; column++) {
            unpack(display, source[column], &red, &green, &blue);
            sum_red += red;
            sum_green += green;
            sum_blue += blue;
            count++;
        }
        for (int column = 0; column < width; column++) {
            if (!first) originals[slot] = destination[column];
            destination[column] = display_rgb(display, average(sum_red, count),
                                               average(sum_green, count), average(sum_blue, count));

            int entering = column + radius + 1;
            if (entering < width) {
                unpack(display, source[entering], &red, &green, &blue);
                sum_red += red;
                sum_green += green;
                sum_blue += blue;
                count++;
            }
            int next_slot = slot == radius ? 0 : slot + 1;
            int leaving = column - radius;
            if (leaving >= 0) {
                /* radius + 1 slots: the oldest saved pixel follows the slot
                 * just written, including at wraparound. */
                unpack(display, first ? source[leaving] : originals[next_slot],
                       &red, &green, &blue);
                sum_red -= red;
                sum_green -= green;
                sum_blue -= blue;
                count--;
            }
            slot = next_slot;
        }
    }
}

static void blur_columns(struct glass_panel *panel, int radius)
{
    struct ol_display *display = panel->display;
    uint32_t *pixels = display->glass_buf;
    int width = panel->sample_width;
    int height = panel->sample_height;

    for (int column = 0; column < width; column++) {
        int sum_red = 0;
        int sum_green = 0;
        int sum_blue = 0;
        int count = 0;
        int red;
        int green;
        int blue;
        uint32_t originals[GLASS_BLUR_RADIUS_MAX + 1];
        int slot = 0;

        for (int row = 0; row <= radius && row < height; row++) {
            unpack(display, pixels[(long)row * width + column], &red, &green, &blue);
            sum_red += red;
            sum_green += green;
            sum_blue += blue;
            count++;
        }
        for (int row = 0; row < height; row++) {
            uint32_t *pixel = &pixels[(long)row * width + column];
            /* This pass writes in place. Save the horizontal result before
             * replacing it, or removing a window member would read a value
             * already blurred vertically and change the filter's weights. */
            originals[slot] = *pixel;
            *pixel = display_rgb(display, average(sum_red, count),
                                 average(sum_green, count), average(sum_blue, count));

            int entering = row + radius + 1;
            if (entering < height) {
                unpack(display, pixels[(long)entering * width + column], &red, &green, &blue);
                sum_red += red;
                sum_green += green;
                sum_blue += blue;
                count++;
            }
            int next_slot = slot == radius ? 0 : slot + 1;
            int leaving = row - radius;
            if (leaving >= 0) {
                /* With radius + 1 slots, the oldest saved pixel is immediately
                 * after the slot just written, including at wraparound. */
                unpack(display, originals[next_slot], &red, &green, &blue);
                sum_red -= red;
                sum_green -= green;
                sum_blue -= blue;
                count--;
            }
            slot = next_slot;
        }
    }
}

void glass_blur(struct glass_panel *panel)
{
    int tier = tier_of(panel->mat);
    unsigned char radii[3];
    int passes = 0;

    for (int i = 0; i < 3; i++)
        if (tier_radii[tier][i]) radii[passes++] = tier_radii[tier][i];
#ifdef GLASS_TIERS_NEGCTL_SINGLE_PASS
    if (tier == OL_GLASS_BLUR_THICK) { radii[0] = 11; passes = 1; }
#endif
    /* Separable: every horizontal pass, then every vertical pass. REGULAR is
     * one pass each way at radius 6, the exact legacy computation. */
    for (int i = 0; i < passes; i++) blur_rows(panel, radii[i], i == 0);
    for (int i = 0; i < passes; i++) blur_columns(panel, radii[i]);
}

void glass_copy_backdrop(struct glass_panel *panel)
{
    const struct ol_pixel_target *target = &panel->display->target;
    uint32_t *pixels = panel->display->glass_buf;
    int width = panel->sample_width;

    /* Clear glass still needs the whole sample halo: its rim refracts into
     * nearby content and reflects pixels outside the shape. Copying only the
     * panel would silently reintroduce the panel-local sampling defect.
     * Unlike Regular, Clear preserves the backdrop's fine detail instead of
     * putting it through the same 13x13 box blur. */
    for (int row = 0; row < panel->sample_height; row++) {
        const uint32_t *source = target->px +
            (long)(panel->sample_y + row) * target->w + panel->sample_x;
        uint32_t *destination = pixels + (long)row * width;
        for (int column = 0; column < width; column++)
            destination[column] = source[column];
    }
}
