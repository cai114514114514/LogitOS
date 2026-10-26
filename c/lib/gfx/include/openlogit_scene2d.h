#ifndef OPENLOGIT_SCENE2D_H
#define OPENLOGIT_SCENE2D_H

#include "openlogit_canvas.h"

#define OL_SCENE2D_LAYERS 32
#define OL_SCENE2D_DAMAGE 8
/* 8-byte words reserved per transformed layer for a borrowed ol_image header
 * (ol_scene2d_render returns OL_LIMIT if ol_image_size() ever exceeds it). */
#define OL_SCENE2D_IMAGE_WORDS 20
struct ol_scene2d_layer {
    struct ol_surface *surface;
    struct gfx_rect bounds;
    int opacity, visible;
    /* Appended. All zero keeps the legacy placement and output bit for bit.
     * tx, ty: 24.8 translation added to bounds.x/y.
     * scale_q8: 8.8 scale about the top-left of bounds, 0 means 256 (1.0).
     * effect_extent: pixels outside the placement that baked effects occupy.
     * It only widens damage; the pixels themselves must be in the surface.
     * A layer with a fractional translation or a scale other than 1.0 is
     * sampled bilinearly and gets an anti-aliased edge. */
    int tx, ty;
    unsigned short scale_q8;
    int effect_extent;
};
struct ol_scene2d_frame {
    struct ol_scene2d_layer layer;
    uint64_t generation;
};
struct ol_scene2d {
    struct ol_device *device;
    int width, height, initialized, force;
    struct ol_surface *target, *background;
    uint64_t target_generation, background_generation;
    struct ol_scene2d_layer layers[OL_SCENE2D_LAYERS];
    struct ol_scene2d_frame previous[OL_SCENE2D_LAYERS];
    /* Disjoint rects the last successful render recomposed (count 0 for an
     * unchanged frame). Storage is part of the scene, nothing is global. */
    struct gfx_rect damage[OL_SCENE2D_DAMAGE];
    int damage_count;
    /* Borrowed images over the pixels of transformed layers' surfaces. */
    struct ol_image *images[OL_SCENE2D_LAYERS];
    uint64_t image_store[OL_SCENE2D_LAYERS][OL_SCENE2D_IMAGE_WORDS];
};

/* Index is paint order, lowest first. Surfaces and their storage remain owned
 * by the caller until detached AND the last recorded list has been reset.
 * Bake expensive vector/effect groups into these surfaces, then move them.
 * Bounds include every baked shadow/blur pixel unless effect_extent says how
 * far beyond the placement they reach. set() replaces the whole layer and
 * zeroes the extended fields. */
int ol_scene2d_init(struct ol_scene2d *scene, struct ol_device *device, int width, int height);
int ol_scene2d_set(struct ol_scene2d *scene, unsigned index, struct ol_surface *surface,
                   struct gfx_rect bounds, int opacity, int visible);
/* Sets the extended fields of an existing layer. scale_q8 0 means 1.0. */
int ol_scene2d_set_xf(struct ol_scene2d *scene, unsigned index, int tx, int ty,
                      unsigned scale_q8, int effect_extent);
/* Z-reorder: layer `from` takes paint position `to`; the layers between shift
 * by one. The next render damages every slot whose content changed. */
int ol_scene2d_move(struct ol_scene2d *scene, int from, int to);
/* Topmost visible layer with opacity > 0 whose transformed rect contains the
 * centre of pixel (x, y); -1 if none. */
int ol_scene2d_hit(const struct ol_scene2d *scene, int x, int y);
/* Destroys the borrowed images made for transformed layers. Call it after the
 * last list that recorded them was reset and before ol_device_destroy. */
int ol_scene2d_release(struct ol_scene2d *scene);
void ol_scene2d_invalidate(struct ol_scene2d *scene);
/* Background may be NULL or transparent. Damage is cleared before recomposing
 * ALL overlapping layers, preserving correct source-over after move/hide/fade.
 * Damage is up to OL_SCENE2D_DAMAGE disjoint rects (scene->damage): placements
 * merge when they overlap, or when their union covers less than 25% more area
 * than the two together. Each rect is cleared and recomposed under its own clip.
 * Previous placements advance only after successful whole-frame publication.
 * An unchanged frame returns OL_OK with zero commands and does not submit. */
int ol_scene2d_render(struct ol_scene2d *scene, struct ol_list *list, struct ol_surface *target,
                      struct ol_surface *background, struct ol_submit_info *info);

#endif
