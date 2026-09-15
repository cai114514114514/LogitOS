#ifndef OPENLOGIT_CANVAS_H
#define OPENLOGIT_CANVAS_H

#include "openlogit.h"

/* Composite recorders can poison a list on failure, preserving the same
 * whole-frame refusal contract as native single-command recorders. */
int ol_list_invalidate(struct ol_list *list, int error);
int ol_cmd_clear_rect(struct ol_list *list, struct gfx_rect rect, unsigned rgb, int alpha);

/* Source is a pixel rectangle inside a versioned RGBA image. The transform
 * maps crop-local pixels to device coordinates; filtering clamps to the crop,
 * so an atlas neighbor never leaks across an edge. Reflection is supported.
 * Geometry, transform and clip are copied; the image is retained until reset. */
int ol_cmd_sprite(struct ol_list *list, struct ol_image *image, struct gfx_rect source,
                  const struct gfx_matrix *transform, const struct gfx_rect *clip, int opacity,
                  int bilinear);
int ol_cmd_image_region(struct ol_list *list, struct ol_image *image, struct gfx_rect source,
                        struct gfx_rect destination, const struct gfx_rect *clip, int opacity,
                        int bilinear);
struct ol_borders {
    int left, top, right, bottom;
};
/* Corners keep their pixel sizes. Destination must fit the border sums. */
int ol_cmd_nine_slice(struct ol_list *list, struct ol_image *image, struct gfx_rect source,
                      struct gfx_rect destination, struct ol_borders borders,
                      const struct gfx_rect *clip, int opacity, int bilinear);

#define OL_CANVAS_STACK 16
struct ol_canvas_state {
    struct gfx_matrix transform;
    struct gfx_rect clip;
    struct ol_image *coverage;
    int mask_x, mask_y, opacity, samples;
};
struct ol_canvas {
    struct ol_list *list;
    struct ol_canvas_state state, saved[OL_CANVAS_STACK];
    unsigned depth;
};
/* This recorder owns no allocator, clock or pixel loop. Paths are flattened
 * by gfx geometry builders. Paint coordinates stay in device space, matching
 * ol_cmd_fill_ex; sprite image coordinates follow their explicit transform.
 * Clip and A8 coverage origins are explicitly device-space, independent of CTM. */
int ol_canvas_init(struct ol_canvas *canvas, struct ol_list *list, int width, int height);
int ol_canvas_save(struct ol_canvas *canvas);
int ol_canvas_restore(struct ol_canvas *canvas);
int ol_canvas_transform(struct ol_canvas *canvas, const struct gfx_matrix *matrix);
int ol_canvas_clip(struct ol_canvas *canvas, struct gfx_rect device_rect);
int ol_canvas_opacity(struct ol_canvas *canvas, int opacity);
int ol_canvas_mask(struct ol_canvas *canvas, struct ol_image *coverage, int x, int y);
int ol_canvas_fill(struct ol_canvas *canvas, const struct gfx_path *path, int rule,
                   const struct gfx_paint *paint);
int ol_canvas_stroke(struct ol_canvas *canvas, const struct gfx_path *path,
                     const struct gfx_stroke *stroke, const struct gfx_paint *paint,
                     struct gfx_path *scratch);
int ol_canvas_round_rect(struct ol_canvas *canvas, struct gfx_rect rect, int radius,
                         const struct gfx_paint *paint);

#endif
