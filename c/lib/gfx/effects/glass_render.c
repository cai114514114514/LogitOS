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
    int sample_x = column - field.normal_x * displacement / 256;
    int sample_y = row - field.normal_y * displacement / 256;
    sample_x = glass_clamp(sample_x, 0, panel->width - 1);
    sample_y = glass_clamp(sample_y, 0, panel->height - 1);
    uint32_t pixel = panel->display->glass_buf[(long)sample_y * panel->width + sample_x];
    return (pixel >> shift) & 255;
}

static inline struct glass_color sample_color(const struct glass_panel *panel,
                                              int column, int row,
                                              struct glass_field field)
{
    struct ol_display *display = panel->display;
    int depth = glass_clamp(field.depth, 0, GLASS_E_MAX);
    return (struct glass_color){
        sample_channel(panel, column, row, field, glass_disp[0][depth], display->rpos),
        sample_channel(panel, column, row, field, glass_disp[1][depth], display->gpos),
        sample_channel(panel, column, row, field, glass_disp[2][depth], display->bpos),
    };
}

static const uint32_t *sample_row(const struct glass_panel *panel,
                                 int row, int direction, int displacement)
{
    int sample_y = glass_clamp(row - direction * displacement, 0, panel->height - 1);
    return panel->display->glass_buf + (long)sample_y * panel->width;
}

static void draw_span(const struct glass_panel *panel, const struct glass_style *style,
                      int row, struct glass_span span, struct glass_field field)
{
    if (span.start == span.end || !field.coverage) return;
    struct ol_display *display = panel->display;
    int depth = glass_clamp(field.depth, 0, GLASS_E_MAX);
    int direction = field.normal_y / 256;
    const uint32_t *red_row = sample_row(panel, row, direction, glass_disp[0][depth]);
    const uint32_t *green_row = sample_row(panel, row, direction, glass_disp[1][depth]);
    const uint32_t *blue_row = sample_row(panel, row, direction, glass_disp[2][depth]);
    uint32_t *destination = display->target.px +
                            (long)(panel->y + row) * display->target.w + panel->x;
    struct glass_light light = lighting(style, field);

    /* The field and lighting are constant across this straight-edge run.
     * Dispersion only needs three fetches when the channel rows differ. */
    if (red_row == green_row && green_row == blue_row) {
        for (int column = span.start; column < span.end; column++) {
            struct glass_color color = read_color(display, red_row[column]);
            color = shade_color(color, style, light);
            store_pixel(display, &destination[column], color, field.coverage);
        }
    } else {
        for (int column = span.start; column < span.end; column++) {
            struct glass_color color = {
                (red_row[column] >> display->rpos) & 255,
                (green_row[column] >> display->gpos) & 255,
                (blue_row[column] >> display->bpos) & 255,
            };
            color = shade_color(color, style, light);
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
        color = shade_color(color, style, lighting(style, field));
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

void glass_render(struct glass_panel *panel, struct glass_color tint, int opacity)
{
    int refraction = panel->edge_width < 18 ? panel->edge_width : 18;
    glass_build_lut(panel->edge_width, refraction);
    struct glass_style style;
    prepare_style(&style, panel, tint, opacity);

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
