#include "glass_geometry.h"
#include "glass_shading.h"

static inline struct glass_color read_color(struct ol_display *display, uint32_t pixel)
{
    struct glass_color color;
    unpack(display, pixel, &color.red, &color.green, &color.blue);
    return color;
}

static inline int sample_channel(const struct glass_panel *panel,
                                 int column, int row, struct glass_field field,
                                 int displacement, unsigned shift)
{
    int sample_x = panel->x + column - field.normal_x * displacement / 256
                   - panel->sample_x;
    int sample_y = panel->y + row - field.normal_y * displacement / 256
                   - panel->sample_y;
    sample_x = glass_clamp(sample_x, 0, panel->sample_width - 1);
    sample_y = glass_clamp(sample_y, 0, panel->sample_height - 1);
    uint32_t pixel = panel->display->glass_buf[(long)sample_y * panel->sample_width + sample_x];
    return (pixel >> shift) & 255;
}

static inline struct glass_color sample_color(const struct glass_panel *panel,
                                              int column, int row,
                                              struct glass_field field)
{
    struct ol_display *display = panel->display;
    int depth = glass_clamp(field.depth, 0, GLASS_E_MAX);
    return (struct glass_color){
        sample_channel(panel, column, row, field, panel->lut->disp[0][depth], display->rpos),
        sample_channel(panel, column, row, field, panel->lut->disp[1][depth], display->gpos),
        sample_channel(panel, column, row, field, panel->lut->disp[2][depth], display->bpos),
    };
}

static const uint32_t *sample_row(const struct glass_panel *panel,
                                 int row, int direction, int displacement)
{
    int sample_y = panel->y + row - direction * displacement - panel->sample_y;
    sample_y = glass_clamp(sample_y, 0, panel->sample_height - 1);
    return panel->display->glass_buf + (long)sample_y * panel->sample_width
           + panel->x - panel->sample_x;
}

static inline void reflect_neighbour(const struct glass_panel *panel,
                                     const struct glass_style *style,
                                     int column, int row, struct glass_field field,
                                     struct glass_light *light)
{
    if (field.depth >= GLASS_REFLECT_BAND || !light->reflection) return;
    int x = panel->x + column + field.normal_x * GLASS_REFLECT_OFFSET / 256
            - panel->sample_x;
    int y = panel->y + row + field.normal_y * GLASS_REFLECT_OFFSET / 256
            - panel->sample_y;
    x = glass_clamp(x, 0, panel->sample_width - 1);
    y = glass_clamp(y, 0, panel->sample_height - 1);
    struct glass_color nearby = read_color(panel->display,
        panel->display->glass_buf[(long)y * panel->sample_width + x]);
    int weight = GLASS_REFLECT_BAND - field.depth;

    if (style->symmetric) {
        /* The environment slides from the rim's mean backdrop to the content
         * just outside. Both are real backdrop colours, so the blend stays in
         * range without the directional rim's 32-level bound. */
        light->environment.red += (nearby.red - light->environment.red)
                                  * weight / GLASS_REFLECT_BAND;
        light->environment.green += (nearby.green - light->environment.green)
                                    * weight / GLASS_REFLECT_BAND;
        light->environment.blue += (nearby.blue - light->environment.blue)
                                   * weight / GLASS_REFLECT_BAND;
        return;
    }

    /* The old uniform-gray environment could not reflect content just beyond
     * the rim: black and white outside an identical panel produced the same
     * 12,288 panel pixels. Let the outer four pixels borrow a restrained tint
     * from a blurred sample six pixels along the surface normal. Refraction
     * still bends the interior inward; the reflection is the separate view of
     * nearby content. Keep this contribution bounded to 32 levels so it does
     * not turn a bright icon beside the dock into an opaque colored stripe. */
    light->environment.red = glass_clamp(light->environment.red +
        (nearby.red - 128) * weight / (GLASS_REFLECT_BAND * 4), 0, 255);
    light->environment.green = glass_clamp(light->environment.green +
        (nearby.green - 128) * weight / (GLASS_REFLECT_BAND * 4), 0, 255);
    light->environment.blue = glass_clamp(light->environment.blue +
        (nearby.blue - 128) * weight / (GLASS_REFLECT_BAND * 4), 0, 255);
}

static void draw_span(const struct glass_panel *panel, const struct glass_style *style,
                      int row, struct glass_span span, struct glass_field field)
{
    if (span.start == span.end || !field.coverage) return;
    struct ol_display *display = panel->display;
    int depth = glass_clamp(field.depth, 0, GLASS_E_MAX);
    int direction = field.normal_y / 256;
    const uint32_t *red_row = sample_row(panel, row, direction, panel->lut->disp[0][depth]);
    const uint32_t *green_row = sample_row(panel, row, direction, panel->lut->disp[1][depth]);
    const uint32_t *blue_row = sample_row(panel, row, direction, panel->lut->disp[2][depth]);
    uint32_t *destination = display->target.px +
                            (long)(panel->y + row) * display->target.w + panel->x;
    struct glass_light light = lighting(style, field);

    /* The field and lighting are constant across this straight-edge run.
     * Dispersion only needs three fetches when the channel rows differ. */
    if (red_row == green_row && green_row == blue_row) {
        for (int column = span.start; column < span.end; column++) {
            struct glass_color color = read_color(display, red_row[column]);
            struct glass_light lit = light;
            reflect_neighbour(panel, style, column, row, field, &lit);
            color = shade_color(color, style, lit);
            store_pixel(display, &destination[column], color, field.coverage);
        }
    } else {
        for (int column = span.start; column < span.end; column++) {
            struct glass_color color = {
                (red_row[column] >> display->rpos) & 255,
                (green_row[column] >> display->gpos) & 255,
                (blue_row[column] >> display->bpos) & 255,
            };
            struct glass_light lit = light;
            reflect_neighbour(panel, style, column, row, field, &lit);
            color = shade_color(color, style, lit);
            store_pixel(display, &destination[column], color, field.coverage);
        }
    }
}

static void draw_edge(const struct glass_panel *panel, const struct glass_style *style,
                      int row, int distance_y, int direction_y, int first, int last)
{
    struct ol_display *display = panel->display;
    const int *columns = display->glass_line;
    uint32_t *destination = display->target.px +
                            (long)(panel->y + row) * display->target.w + panel->x;

    for (int column = first; column < last; column++) {
        int direction_x = column < panel->center_x ? -1 : 1;
        struct glass_field field = pixel_field(panel, columns[column], distance_y,
                                                direction_x, direction_y);
        if (!field.coverage) continue;

        struct glass_color color = sample_color(panel, column, row, field);
        struct glass_light light = lighting(style, field);
        reflect_neighbour(panel, style, column, row, field, &light);
        color = shade_color(color, style, light);
        store_pixel(display, &destination[column], color, field.coverage);
    }
}

static void draw_square_edge(const struct glass_panel *panel,
                             const struct glass_style *style,
                             int row, int first, int last)
{
    struct ol_display *display = panel->display;
    uint32_t *destination = display->target.px +
                            (long)(panel->y + row) * display->target.w + panel->x;
    for (int column = first; column < last; column++) {
        struct glass_field field = square_field(panel, column, row);
        struct glass_color color = sample_color(panel, column, row, field);
        struct glass_light light = lighting(style, field);
        reflect_neighbour(panel, style, column, row, field, &light);
        color = shade_color(color, style, light);
        store_pixel(display, &destination[column], color, 255);
    }
}

static void render_square(struct glass_panel *panel, const struct glass_style *style)
{
    for (int row = panel->clip_top; row < panel->clip_bottom; row++) {
        struct glass_field field = square_row_field(panel, row);
        /* The middle has one row normal. Only the narrow vertical rims need
         * per-pixel work. Equality stays on the rim so fast and slow geometry
         * make exactly the same choice at all four corners. */
        int boundary = field.depth < panel->edge_width
            ? field.depth + 1 : panel->edge_width;
        int start = panel->cut & OL_GLASS_CUT_LEFT ? panel->clip_left
            : glass_clamp(panel->shape_left + boundary,
                          panel->clip_left, panel->clip_right);
        int end = panel->cut & OL_GLASS_CUT_RIGHT ? panel->clip_right
            : glass_clamp(panel->shape_right - boundary,
                          panel->clip_left, panel->clip_right);
        if (end < start) end = start;
        draw_square_edge(panel, style, row, panel->clip_left, start);
        draw_span(panel, style, row, (struct glass_span){start, end}, field);
        draw_square_edge(panel, style, row, end, panel->clip_right);
    }
}

/* SYMMETRIC: mean and luma variance of the already-blurred halo over the rim
 * band, from the reflected sample just outside the outline to the inner end
 * of the bevel. The band is the shape's rectangle widened by the reflection
 * offset minus its interior shrunk by the edge width; rounded corners do not
 * need an exact outline for a statistic. The sample box is the whole halo even
 * under a partial write clip, so a damage repaint measures the same value as
 * a full frame. A flat backdrop has zero variance and turns the reflection
 * off; busy content brings it back. */
static void measure_rim_backdrop(const struct glass_panel *panel,
                                 struct glass_style *style)
{
    const struct ol_display *display = panel->display;
    int width = panel->sample_width, height = panel->sample_height;
    int left = panel->x + panel->shape_left - panel->sample_x;
    int top = panel->y + panel->shape_top - panel->sample_y;
    int right = panel->x + panel->shape_right - panel->sample_x;
    int bottom = panel->y + panel->shape_bottom - panel->sample_y;
    int edge = panel->edge_width;
    int outer_left = glass_clamp(left - GLASS_REFLECT_OFFSET, 0, width);
    int outer_right = glass_clamp(right + GLASS_REFLECT_OFFSET, outer_left, width);
    int outer_top = glass_clamp(top - GLASS_REFLECT_OFFSET, 0, height);
    int outer_bottom = glass_clamp(bottom + GLASS_REFLECT_OFFSET, outer_top, height);
    int inner_left = glass_clamp(left + edge, outer_left, outer_right);
    int inner_right = glass_clamp(right - edge, inner_left, outer_right);
    int inner_top = glass_clamp(top + edge, outer_top, outer_bottom);
    int inner_bottom = glass_clamp(bottom - edge, inner_top, outer_bottom);
    long area = (long)(outer_right - outer_left) * (outer_bottom - outer_top)
              - (long)(inner_right - inner_left) * (inner_bottom - inner_top);
    int step = 1;
    while (area / ((long)step * step) > GLASS_RIM_SAMPLES) step++;

    long long count = 0, red = 0, green = 0, blue = 0, luma = 0, squares = 0;
    for (int y = outer_top; y < outer_bottom; y += step) {
        const uint32_t *line = display->glass_buf + (long)y * width;
        int inner_row = y >= inner_top && y < inner_bottom;
        for (int x = outer_left; x < outer_right; x += step) {
            if (inner_row && x >= inner_left && x < inner_right) {
                x += (inner_right - x + step - 1) / step * step - step;
                continue;
            }
            struct glass_color color = read_color(panel->display, line[x]);
            int tone = color_luma(color);
            red += color.red;
            green += color.green;
            blue += color.blue;
            luma += tone;
            squares += tone * tone;
            count++;
        }
    }
    if (!count) return;
    style->mean = (struct glass_color){(int)(red / count), (int)(green / count),
                                       (int)(blue / count)};
    long long variance = (count * squares - luma * luma) / (count * count);
    long long gain = (variance - GLASS_RIM_VARIANCE_FLOOR) * 256
                   / (GLASS_RIM_VARIANCE_FULL - GLASS_RIM_VARIANCE_FLOOR);
    style->reflect_gain = gain < 0 ? 0 : gain > 256 ? 256 : (int)gain;
}

void glass_render(struct glass_panel *panel, struct glass_color tint, int opacity)
{
    int refraction = panel->edge_width < 18 ? panel->edge_width : 18;
    panel->lut = glass_lut_get(panel->edge_width, refraction, &panel->lut_own);
    struct glass_style style;
    prepare_style(&style, panel, tint, opacity);
    if (style.symmetric) measure_rim_backdrop(panel, &style);

    if (panel->radius == 0) {
        render_square(panel, &style);
        return;
    }

    int *columns = panel->display->glass_line;
    for (int column = 0; column < panel->width; column++) {
        columns[column] = axis_distance(column - panel->center_x, panel->inner_x,
                                        panel->cut, OL_GLASS_CUT_LEFT, OL_GLASS_CUT_RIGHT);
    }

    for (int row = panel->clip_top; row < panel->clip_bottom; row++) {
        int offset_y = row - panel->center_y;
        int direction_y = offset_y < 0 ? -1 : 1;
        int distance_y = axis_distance(offset_y, panel->inner_y, panel->cut,
                                        OL_GLASS_CUT_TOP, OL_GLASS_CUT_BOTTOM);
        struct glass_span span = row_span(panel, distance_y);

        draw_edge(panel, &style, row, distance_y, direction_y, panel->clip_left, span.start);
        draw_span(panel, &style, row, span, row_field(panel, distance_y, direction_y));
        draw_edge(panel, &style, row, distance_y, direction_y, span.end, panel->clip_right);
    }
}

void glass_render_solid(struct glass_panel *panel, struct glass_color tint)
{
    struct ol_display *display = panel->display;
    uint32_t fill = display_rgb(display, tint.red, tint.green, tint.blue);

    /* A rounded panel may be cropped by the screen or clipped by damage.
     * Drawing a fresh rounded rectangle from its visible bounds invents a rim
     * at that crop; use the original shape and the same cut-edge field as the
     * translucent material. Only the one-pixel antialias fringe blends. */
    for (int row = panel->clip_top; row < panel->clip_bottom; row++) {
        uint32_t *destination = display->target.px +
                                (long)(panel->y + row) * display->target.w;
        if (panel->radius == 0) {
            for (int column = panel->clip_left; column < panel->clip_right; column++)
                destination[panel->x + column] = fill;
            continue;
        }

        int offset_y = row - panel->center_y;
        int direction_y = offset_y < 0 ? -1 : 1;
        int distance_y = axis_distance(offset_y, panel->inner_y, panel->cut,
                                       OL_GLASS_CUT_TOP, OL_GLASS_CUT_BOTTOM);
        for (int column = panel->clip_left; column < panel->clip_right; column++) {
            int offset_x = column - panel->center_x;
            int direction_x = offset_x < 0 ? -1 : 1;
            int distance_x = axis_distance(offset_x, panel->inner_x, panel->cut,
                                           OL_GLASS_CUT_LEFT, OL_GLASS_CUT_RIGHT);
            int coverage = pixel_field(panel, distance_x, distance_y,
                                       direction_x, direction_y).coverage;
            if (coverage == 255) destination[panel->x + column] = fill;
            else if (coverage)
                store_pixel(display, &destination[panel->x + column], tint, coverage);
        }
    }
}
