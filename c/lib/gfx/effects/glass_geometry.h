#ifndef OPENLOGIT_GLASS_GEOMETRY_H
#define OPENLOGIT_GLASS_GEOMETRY_H

#include "glass_internal.h"

static inline int axis_distance(int offset, int inset, unsigned cut,
                                unsigned first_edge, unsigned last_edge)
{
    int distance = offset < 0 ? -offset : offset;
    /* A cut side continues into a neighbouring panel; it must have no rim. */
    if ((offset < 0 && (cut & first_edge)) ||
        (offset > 0 && (cut & last_edge))) distance = 0;
    return distance - inset;
}

static inline struct glass_field surface_field(int distance)
{
    /* Signed distance is 8.8. Centering the one-pixel coverage ramp on zero
     * preserves half coverage at the boundary instead of a stepped outline. */
    struct glass_field field = {0};
    field.coverage = glass_clamp(128 - distance, 0, 255);
    field.depth = distance < 0 ? -distance / 256 : 0;
    return field;
}

static inline struct glass_field row_field(const struct glass_panel *panel,
                                           int distance_y, int direction_y)
{
    /* On a straight horizontal run sqrt(y*y*65536) is exactly y*256. */
    struct glass_field field = surface_field((distance_y - panel->radius) * 256);
    field.normal_y = direction_y * 256;
    return field;
}

static inline struct glass_field pixel_field(const struct glass_panel *panel,
                                             int distance_x, int distance_y,
                                             int direction_x, int direction_y)
{
    int outside_x = distance_x > 0 ? distance_x : 0;
    int outside_y = distance_y > 0 ? distance_y : 0;
    unsigned long squared = (unsigned long)outside_x * outside_x +
                            (unsigned long)outside_y * outside_y;
    int outside = squared ? (int)gl_isqrt(squared << 16) : 0;
    int inside = distance_x > distance_y ? distance_x : distance_y;
    if (inside > 0) inside = 0;

    struct glass_field field = surface_field(outside + (inside - panel->radius) * 256);
    if (!field.coverage) return field;

    if (outside_x || outside_y) {
        /* The old 32-bit root saturated large corners, once dismissed as
         * "a panel nobody draws". With the corrected root we can also reuse
         * it for the normal: floor(256*sqrt(v))>>8 == floor(sqrt(v)). */
        int length = outside >> 8;
        if (!length) length = 1;
        field.normal_x = direction_x * outside_x * 256 / length;
        field.normal_y = direction_y * outside_y * 256 / length;
    } else if (distance_x > distance_y) {
        field.normal_x = direction_x * 256;
    } else {
        field.normal_y = direction_y * 256;
    }
    return field;
}

static inline struct glass_span row_span(const struct glass_panel *panel, int distance_y)
{
    struct glass_span span = {panel->clip_right, panel->clip_right};
#ifndef GLASS_FIELD_SLOW
    const int *columns = panel->display->glass_line;
    int threshold = distance_y > 0 ? 0 : distance_y;
    int pivot = glass_clamp(panel->center_x, 0, panel->width - 1);
    if (columns[pivot] > threshold) return span;

    /* The folded column distances form a V, possibly with one flat arm.
     * Columns below the threshold form one run where only the row contributes
     * to the field. The clipped center is still the minimum for offscreen
     * panels. Equality belongs to the row, matching pixel_field's branch. */
    int low = 0;
    int high = pivot;
    while (low < high) {
        int middle = (low + high) / 2;
        if (columns[middle] <= threshold) high = middle;
        else low = middle + 1;
    }
    span.start = glass_clamp(low, panel->clip_left, panel->clip_right);

    low = pivot;
    high = panel->width - 1;
    while (low < high) {
        int middle = (low + high + 1) / 2;
        if (columns[middle] <= threshold) low = middle;
        else high = middle - 1;
    }
    span.end = high + 1 < panel->clip_right ? high + 1 : panel->clip_right;
    if (span.end < span.start) span.end = span.start;
#else
    (void)distance_y;
#endif
    return span;
}

#endif
