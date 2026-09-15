#ifndef OPENLOGIT_SCENE2D_H
#define OPENLOGIT_SCENE2D_H

#include "openlogit_canvas.h"

#define OL_SCENE2D_LAYERS 32
struct ol_scene2d_layer {
    struct ol_surface *surface;
    struct gfx_rect bounds;
    int opacity, visible;
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
};

/* Index is paint order, lowest first. Surfaces and their storage remain owned
 * by the caller until detached AND the last recorded list has been reset.
 * Bake expensive vector/effect groups into these surfaces, then move them.
 * Bounds include every baked shadow/blur pixel; no hidden effect extent exists. */
int ol_scene2d_init(struct ol_scene2d *scene, struct ol_device *device, int width, int height);
int ol_scene2d_set(struct ol_scene2d *scene, unsigned index, struct ol_surface *surface,
                   struct gfx_rect bounds, int opacity, int visible);
void ol_scene2d_invalidate(struct ol_scene2d *scene);
/* Background may be NULL or transparent. Damage is cleared before recomposing
 * ALL overlapping layers, preserving correct source-over after move/hide/fade.
 * Previous placements advance only after successful whole-frame publication.
 * An unchanged frame returns OL_OK with zero commands and does not submit. */
int ol_scene2d_render(struct ol_scene2d *scene, struct ol_list *list, struct ol_surface *target,
                      struct ol_surface *background, struct ol_submit_info *info);

#endif
