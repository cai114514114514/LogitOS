#include "glass_internal.h"

/* Window counts vary at the panel edges. These reciprocals preserve integer
 * division for every reachable byte sum without a divide in the pixel loop. */
static const int reciprocals[2 * GLASS_BLUR_RADIUS + 2] = {
    0, 65536, 32768, 21846, 16384, 13108, 10923,
    9363, 8192, 7282, 6554, 5958, 5462, 5042
};

static inline uint8_t average(int sum, int count)
{
    return (uint8_t)((sum * reciprocals[count]) >> 16);
}

static void blur_rows(struct glass_panel *panel)
{
    struct ol_display *display = panel->display;
    const struct ol_pixel_target *target = &display->target;
    uint32_t *pixels = display->glass_buf;
    int width = panel->sample_width;

    /* Read the live backdrop and write separate scratch storage. No source
     * pixel changes before it leaves the horizontal moving window. */
    for (int row = 0; row < panel->sample_height; row++) {
        long start = (long)(panel->sample_y + row) * target->w + panel->sample_x;
        const uint32_t *source = target->px + start;
        uint32_t *destination = pixels + (long)row * width;
        int sum_red = 0;
        int sum_green = 0;
        int sum_blue = 0;
        int count = 0;
        int red;
        int green;
        int blue;

        for (int column = 0; column <= GLASS_BLUR_RADIUS && column < width; column++) {
            unpack(display, source[column], &red, &green, &blue);
            sum_red += red;
            sum_green += green;
            sum_blue += blue;
            count++;
        }
        for (int column = 0; column < width; column++) {
            destination[column] = display_rgb(display, average(sum_red, count),
                                               average(sum_green, count), average(sum_blue, count));

            int entering = column + GLASS_BLUR_RADIUS + 1;
            if (entering < width) {
                unpack(display, source[entering], &red, &green, &blue);
                sum_red += red;
                sum_green += green;
                sum_blue += blue;
                count++;
            }
            int leaving = column - GLASS_BLUR_RADIUS;
            if (leaving >= 0) {
                unpack(display, source[leaving], &red, &green, &blue);
                sum_red -= red;
                sum_green -= green;
                sum_blue -= blue;
                count--;
            }
        }
    }
}

static void blur_columns(struct glass_panel *panel)
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
        uint32_t originals[GLASS_BLUR_RADIUS + 1];
        int slot = 0;

        for (int row = 0; row <= GLASS_BLUR_RADIUS && row < height; row++) {
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

            int entering = row + GLASS_BLUR_RADIUS + 1;
            if (entering < height) {
                unpack(display, pixels[(long)entering * width + column], &red, &green, &blue);
                sum_red += red;
                sum_green += green;
                sum_blue += blue;
                count++;
            }
            int next_slot = slot == GLASS_BLUR_RADIUS ? 0 : slot + 1;
            int leaving = row - GLASS_BLUR_RADIUS;
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
    blur_rows(panel);
    blur_columns(panel);
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
