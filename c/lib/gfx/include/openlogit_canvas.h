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
    /* Clip arena bookkeeping for the mask this state pushed (clip.c). */
    unsigned long arena_prev, arena_stamp;
    int arena_own;
    /* Set only on the state that ol_canvas_save_layer opened (state.c): the
     * list to return to, the group surface, its parent-device rect and the
     * layer arena mark to rewind to once the group is composed. */
    struct ol_list *layer_parent;
    struct ol_surface *layer_surface;
    struct gfx_rect layer_dst;
    int layer_opacity;
    unsigned long layer_mark;
};
struct ol_canvas {
    struct ol_list *list;
    struct ol_canvas_state state, saved[OL_CANVAS_STACK];
    /* ol_canvas_init_ex: caller stack of `capacity` slots (else saved[]). */
    struct ol_canvas_state *stack;
    unsigned depth, capacity;
    /* Caller scratch for save_layer groups; see ol_canvas_init_layers. */
    unsigned char *layer_arena;
    unsigned long layer_bytes, layer_used, layer_list_bytes;
    /* Caller-provided A8 scratch for clip masks; see ol_canvas_init_clip_arena. */
    unsigned char *clip_arena;
    unsigned long clip_arena_bytes, clip_arena_used;
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

/* Configurable stack and samples. init_ex keeps the stack in caller storage:
 * `depth` slots (1..65535) of `stack`, 8-byte aligned. stack == NULL means the
 * embedded OL_CANVAS_STACK (16) slots, exactly as ol_canvas_init. set_samples
 * takes 1..32 and is part of the saved state. */
int ol_canvas_init_ex(struct ol_canvas *canvas, struct ol_list *list, int width, int height,
                      struct ol_canvas_state *stack, int depth);
int ol_canvas_set_samples(struct ol_canvas *canvas, int n);

/* Group opacity (state.c). save_layer pushes a state like save and redirects
 * recording into a nested sub-list rendered into a transparent scratch surface;
 * ol_canvas_restore submits it and records ONE ol_cmd_layer on the parent list,
 * so overlapping children are composed first and `opacity` (multiplied with the
 * current opacity) is applied once. bounds is in device space and is clipped to
 * the current clip. User space is unchanged inside the layer (the CTM absorbs
 * the shift), but DEVICE space is layer-local, origin at the clipped bounds:
 * device rects (ol_canvas_clip, nested save_layer bounds), device-space paint
 * and mask origins use it. The current opacity restarts at 255 inside. A mask keeps applying to each child.
 * op: only GFX_OP_SRC_OVER, anything else returns OL_UNSUPPORTED and records
 * nothing. Scratch (init_layers, 8-byte aligned) holds per layer a surface,
 * w*h*4 front, w*h*4 work and layer_list_bytes (default 32768, set the field
 * after init_layers to change it) of sub-list; too small returns OL_LIMIT with
 * state and list untouched. The group surface stays in the scratch until the
 * parent list is reset: call init_layers again after ol_list_reset. */
int ol_canvas_init_layers(struct ol_canvas *canvas, void *scratch, unsigned long bytes);
int ol_canvas_save_layer(struct ol_canvas *canvas, struct gfx_rect bounds, int opacity,
                         enum gfx_op op);

/* Images and sprites (sprites.c). src is a pixel rect of the image. image maps
 * it onto dst (user space, through the CTM); sprite maps crop-local pixels
 * through `transform` (user space) then the CTM. filter: 0 nearest, 1 bilinear.
 * Clip and opacity come from the state. A coverage mask cannot be applied to
 * sprites: with one set they return OL_UNSUPPORTED and record nothing. */
int ol_canvas_image(struct ol_canvas *canvas, struct ol_image *image, struct gfx_rect src,
                    struct gfx_rect dst, int filter);
int ol_canvas_sprite(struct ol_canvas *canvas, struct ol_image *image, struct gfx_rect src,
                     const struct gfx_matrix *transform, int filter);

/* Clip stack (canvas/clip.c). Masks are A8, live in a caller-provided arena
 * (8-byte aligned, each mask costs a small header plus w*h bytes) and are
 * MULTIPLIED with the current mask, (s*c+127)/255. save/restore push and pop
 * them. Rects, radii (tl,tr,br,bl) and path points are in the current user
 * space and go through the CTM. A mask edge larger than GFX_CLIP_MASK_MAX or
 * an exhausted arena returns OL_LIMIT and leaves the state AND the list intact.
 * An arena slot is reclaimed by restore only when no command was recorded
 * since the push (recorded fills point at the mask until the list is reset);
 * call ol_canvas_init_clip_arena again after ol_list_reset to reclaim all. */
int ol_canvas_init_clip_arena(struct ol_canvas *canvas, void *a8_scratch, unsigned long bytes);
int ol_canvas_clip_rrect(struct ol_canvas *canvas, struct gfx_rect r, const int radius[4]);
int ol_canvas_clip_path(struct ol_canvas *canvas, const struct gfx_path *path, int rule);
/* intersect == 0 is exactly ol_canvas_mask (replace). intersect != 0 multiplies
 * with the current mask over the overlap of both extents (needs the arena). */
int ol_canvas_mask_ex(struct ol_canvas *canvas, struct ol_image *coverage, int x, int y,
                      int intersect);
/* Internal: called by ol_canvas_restore with the state being discarded. */
void ol_canvas_clip_pop(struct ol_canvas *canvas, const struct ol_canvas_state *popped);

#endif
