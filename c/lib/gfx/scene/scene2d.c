#include "openlogit_scene2d.h"

int ol_scene2d_init(struct ol_scene2d *scene, struct ol_device *device, int width, int height)
{
    struct ol_caps caps = {.size = sizeof caps};
    if (!scene || ol_device_caps(device, &caps) != OL_OK || width < 1 || height < 1 ||
        width > GFX_MAX_W || height > GFX_MAX_W)
        return OL_ARGUMENT;
    gfx_zero(scene, sizeof *scene);
    scene->device = device;
    scene->width = width;
    scene->height = height;
    return OL_OK;
}

int ol_scene2d_set(struct ol_scene2d *scene, unsigned index, struct ol_surface *surface,
                   struct gfx_rect bounds, int opacity, int visible)
{
    if (!scene || index >= OL_SCENE2D_LAYERS || opacity < 0 || opacity > 255 || bounds.w < 0 ||
        bounds.h < 0 || bounds.x < -32768 || bounds.y < -32768 || bounds.w > 32768 ||
        bounds.h > 32768 || bounds.x > 32768 - bounds.w || bounds.y > 32768 - bounds.h ||
        (visible && (!surface || !bounds.w || !bounds.h)))
        return OL_ARGUMENT;
    scene->layers[index] = (struct ol_scene2d_layer){surface, bounds, opacity, visible != 0};
    return OL_OK;
}

void ol_scene2d_invalidate(struct ol_scene2d *scene)
{
    if (scene)
        scene->force = 1;
}

static int same(const struct ol_scene2d_frame *a, const struct ol_scene2d_frame *b)
{
    const struct ol_scene2d_layer *x = &a->layer, *y = &b->layer;
    return x->surface == y->surface && x->bounds.x == y->bounds.x && x->bounds.y == y->bounds.y &&
           x->bounds.w == y->bounds.w && x->bounds.h == y->bounds.h && x->opacity == y->opacity &&
           x->visible == y->visible && a->generation == b->generation;
}

static struct gfx_rect visible_bounds(const struct ol_scene2d_frame *frame)
{
    return frame->layer.visible ? frame->layer.bounds : (struct gfx_rect){0, 0, 0, 0};
}

int ol_scene2d_render(struct ol_scene2d *scene, struct ol_list *list, struct ol_surface *target,
                      struct ol_surface *background, struct ol_submit_info *info)
{
    if (!scene || !info || info->size < sizeof *info)
        return OL_ARGUMENT;
    *info = (struct ol_submit_info){.size = sizeof *info};
    struct ol_surface_view target_view, background_view;
    if (ol_surface_view(target, &target_view) != OL_OK ||
        target_view.width != (unsigned)scene->width ||
        target_view.height != (unsigned)scene->height || target == background)
        return OL_ARGUMENT;
    gfx_zero(&background_view, sizeof background_view);
    if (background && ol_surface_view(background, &background_view) != OL_OK)
        return OL_ARGUMENT;
    struct gfx_rect full = {0, 0, scene->width, scene->height};
    struct gfx_rect damage = {0, 0, 0, 0};
    if (!scene->initialized || scene->force || target != scene->target ||
        background != scene->background || target_view.generation != scene->target_generation ||
        background_view.generation != scene->background_generation)
        damage = full;
    struct ol_scene2d_frame next[OL_SCENE2D_LAYERS];
    for (unsigned i = 0; i < OL_SCENE2D_LAYERS; i++) {
        next[i].layer = scene->layers[i];
        next[i].generation = 0;
        if (next[i].layer.visible) {
            struct ol_surface_view view;
            if (ol_surface_view(next[i].layer.surface, &view) != OL_OK)
                return OL_ARGUMENT;
            next[i].generation = view.generation;
        }
        if (!same(&next[i], &scene->previous[i])) {
            struct gfx_rect old = visible_bounds(&scene->previous[i]);
#ifdef OPENLOGIT_DAMAGE_OLD_DISABLED
            old = (struct gfx_rect){0, 0, 0, 0}; /* Moving-layer oracle must fail. */
#endif
            struct gfx_rect moved =
                ol_damage_move(old, visible_bounds(&next[i]), 0, scene->width, scene->height);
            damage = ol_damage_move(damage, moved, 0, scene->width, scene->height);
        }
    }
    if (!damage.w || !damage.h)
        return OL_OK;
    int status = ol_list_reset(list);
    if (status != OL_OK)
        return status;
    status = ol_cmd_clear_rect(list, damage, 0, 0);
    struct ol_layer_options options = {
        .size = sizeof options, .clip = &damage, .opacity = 255, .bilinear = 0};
    if (status == OL_OK && background)
        status = ol_cmd_layer(list, background, full, &options);
    for (unsigned i = 0; i < OL_SCENE2D_LAYERS && status == OL_OK; i++) {
        struct ol_scene2d_layer *layer = &next[i].layer;
        if (!layer->visible)
            continue;
        options.opacity = layer->opacity;
        status = ol_cmd_layer(list, layer->surface, layer->bounds, &options);
    }
    if (status == OL_OK)
        status = ol_list_close(list);
    if (status == OL_OK)
        status = ol_submit_damage(scene->device, list, target, info);
    if (status != OL_OK)
        return status;
    /* Do not record a failed attempt as displayed state. Retrying after an
     * arena exhaustion must still erase the last SUCCESSFUL placement. */
    for (unsigned i = 0; i < OL_SCENE2D_LAYERS; i++)
        scene->previous[i] = next[i];
    scene->target = target;
    scene->background = background;
    ol_surface_view(target, &target_view);
    scene->target_generation = target_view.generation;
    scene->background_generation = background_view.generation;
    scene->initialized = 1;
    scene->force = 0;
    return OL_OK;
}
