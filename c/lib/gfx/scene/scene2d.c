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
    scene->layers[index] = (struct ol_scene2d_layer){.surface = surface,
                                                     .bounds = bounds,
                                                     .opacity = opacity,
                                                     .visible = visible != 0};
    return OL_OK;
}

#define XF_LIMIT (32768 * 256)
int ol_scene2d_set_xf(struct ol_scene2d *scene, unsigned index, int tx, int ty,
                      unsigned scale_q8, int effect_extent)
{
    if (!scene || index >= OL_SCENE2D_LAYERS || tx < -XF_LIMIT || tx > XF_LIMIT ||
        ty < -XF_LIMIT || ty > XF_LIMIT || scale_q8 > 65535 || effect_extent < 0 ||
        effect_extent > 32768)
        return OL_ARGUMENT;
    struct ol_scene2d_layer *l = &scene->layers[index];
    l->tx = tx;
    l->ty = ty;
    l->scale_q8 = (unsigned short)scale_q8;
    l->effect_extent = effect_extent;
    return OL_OK;
}

int ol_scene2d_move(struct ol_scene2d *scene, int from, int to)
{
    if (!scene || from < 0 || to < 0 || from >= OL_SCENE2D_LAYERS || to >= OL_SCENE2D_LAYERS)
        return OL_ARGUMENT;
    struct ol_scene2d_layer moved = scene->layers[from];
    for (int i = from; i < to; i++)
        scene->layers[i] = scene->layers[i + 1];
    for (int i = from; i > to; i--)
        scene->layers[i] = scene->layers[i - 1];
    scene->layers[to] = moved;
    return OL_OK;
}

void ol_scene2d_invalidate(struct ol_scene2d *scene)
{
    if (scene)
        scene->force = 1;
}

int ol_scene2d_release(struct ol_scene2d *scene)
{
    if (!scene)
        return OL_ARGUMENT;
    int result = OL_OK;
    for (unsigned i = 0; i < OL_SCENE2D_LAYERS; i++) {
        if (!scene->images[i])
            continue;
        int status = ol_image_destroy(scene->images[i]);
        if (status == OL_OK || status == OL_ARGUMENT) /* ARGUMENT: already dead */
            scene->images[i] = 0;
        else
            result = status;
    }
    return result;
}

/* ---- placement: 24.8 box of a layer, scale about the top-left of bounds ---- */
static int scale_of(const struct ol_scene2d_layer *l)
{
    return l->scale_q8 ? l->scale_q8 : 256;
}

static int is_xf(const struct ol_scene2d_layer *l)
{
    return l->tx || l->ty || scale_of(l) != 256;
}

static void box24(const struct ol_scene2d_layer *l, long long b[4])
{
    long long s = scale_of(l);
    b[0] = (long long)l->bounds.x * 256 + l->tx;
    b[1] = (long long)l->bounds.y * 256 + l->ty;
    b[2] = b[0] + (long long)l->bounds.w * s;
    b[3] = b[1] + (long long)l->bounds.h * s;
}

/* Outward-rounded pixel rect, grown by `extent`, clamped to the scene. A layer
 * with default fields yields exactly its bounds, as ol_damage_move did. */
static struct gfx_rect outward(const struct ol_scene2d_layer *l, int extent, int w, int h)
{
    long long b[4];
    box24(l, b);
    long long x0 = (b[0] >> 8) - extent, y0 = (b[1] >> 8) - extent;
    long long x1 = ((b[2] + 255) >> 8) + extent, y1 = ((b[3] + 255) >> 8) + extent;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > w)
        x1 = w;
    if (y1 > h)
        y1 = h;
    if (x1 <= x0 || y1 <= y0)
        return (struct gfx_rect){0, 0, 0, 0};
    return (struct gfx_rect){(int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0)};
}

int ol_scene2d_hit(const struct ol_scene2d *scene, int x, int y)
{
    if (!scene)
        return -1;
    long long sx = (long long)x * 256 + 128, sy = (long long)y * 256 + 128;
    for (int i = OL_SCENE2D_LAYERS - 1; i >= 0; i--) {
        const struct ol_scene2d_layer *l = &scene->layers[i];
        long long b[4];
        if (!l->visible || !l->surface || l->opacity <= 0)
            continue;
        box24(l, b);
        if (sx >= b[0] && sx < b[2] && sy >= b[1] && sy < b[3])
            return i;
    }
    return -1;
}

static int same(const struct ol_scene2d_frame *a, const struct ol_scene2d_frame *b)
{
    const struct ol_scene2d_layer *x = &a->layer, *y = &b->layer;
    return x->surface == y->surface && x->bounds.x == y->bounds.x && x->bounds.y == y->bounds.y &&
           x->bounds.w == y->bounds.w && x->bounds.h == y->bounds.h && x->opacity == y->opacity &&
           x->visible == y->visible && x->tx == y->tx && x->ty == y->ty &&
           scale_of(x) == scale_of(y) && x->effect_extent == y->effect_extent &&
           a->generation == b->generation;
}

/* ---- disjoint damage set ---- */
#define RAW_DAMAGE (2 * OL_SCENE2D_LAYERS + 1)
static long long area(struct gfx_rect r)
{
    return (long long)r.w * r.h;
}

static struct gfx_rect runion(struct gfx_rect a, struct gfx_rect b)
{
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct gfx_rect){x0, y0, x1 - x0, y1 - y0};
}

static int overlaps(struct gfx_rect a, struct gfx_rect b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/* Merge when the rects overlap, or when covering both with their union adds
 * less than a quarter of the two areas. */
static int should_merge(struct gfx_rect a, struct gfx_rect b)
{
    if (overlaps(a, b))
        return 1;
    long long sum = area(a) + area(b);
    return (area(runion(a, b)) - sum) * 4 < sum;
}

static int normalize(struct gfx_rect *d, int n)
{
    for (int again = 1; again;) {
        again = 0;
        for (int i = 0; i < n && !again; i++)
            for (int j = i + 1; j < n && !again; j++)
                if (should_merge(d[i], d[j])) {
                    d[i] = runion(d[i], d[j]);
                    d[j] = d[--n];
                    again = 1;
                }
    }
#ifdef OL_SCENE_SINGLE_DAMAGE /* Negative control: one union rect, as before. */
    while (n > 1) {
        d[0] = runion(d[0], d[n - 1]);
        n--;
    }
#endif
    while (n > OL_SCENE2D_DAMAGE) {
        int bi = 0, bj = 1;
        long long best = -1;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                long long waste = area(runion(d[i], d[j])) - area(d[i]) - area(d[j]);
                if (best < 0 || waste < best) {
                    best = waste;
                    bi = i;
                    bj = j;
                }
            }
        d[bi] = runion(d[bi], d[bj]);
        d[bj] = d[--n];
        for (int again = 1; again;) { /* the grown rect may now overlap others */
            again = 0;
            for (int i = 0; i < n && !again; i++)
                for (int j = i + 1; j < n && !again; j++)
                    if (overlaps(d[i], d[j])) {
                        d[i] = runion(d[i], d[j]);
                        d[j] = d[--n];
                        again = 1;
                    }
        }
    }
    return n;
}

/* ---- borrowed image over a transformed layer's surface pixels ---- */
static int image_for(struct ol_scene2d *scene, unsigned i, const struct ol_surface_view *v,
                     struct ol_image **out)
{
    struct ol_image *im = scene->images[i];
    if (im) {
        struct ol_surface_view iv;
        if (ol_image_view(im, &iv) == OL_OK) {
            if (iv.pixels == v->pixels && iv.width == v->width && iv.height == v->height &&
                iv.stride == v->stride) {
                *out = im;
                return OL_OK;
            }
            int status = ol_image_destroy(im);
            if (status != OL_OK)
                return status;
        }
        scene->images[i] = 0;
    }
    if (ol_image_size() > sizeof scene->image_store[i])
        return OL_LIMIT;
    struct ol_image_desc desc = {
        .size = sizeof desc, .format = OL_FORMAT_RGBA8_STRAIGHT, .width = v->width,
        .height = v->height, .stride = v->stride, .pixels = (unsigned char *)v->pixels,
        .bytes = (unsigned long)(v->height - 1) * v->stride + (unsigned long)v->width * 4};
    int status = ol_image_create(scene->device, scene->image_store[i],
                                 sizeof scene->image_store[i], &desc, &im);
    if (status != OL_OK)
        return status;
    scene->images[i] = im;
    *out = im;
    return OL_OK;
}

static int record_layer(struct ol_scene2d *scene, struct ol_list *list, unsigned index,
                        const struct ol_scene2d_layer *layer, struct gfx_rect clip,
                        struct ol_layer_options *options)
{
    long long s = scale_of(layer);
    if (s == 256 && !(layer->tx & 255) && !(layer->ty & 255)) {
        /* Whole-pixel placement: the legacy layer command, bit-identical. */
        struct gfx_rect dst = layer->bounds;
        dst.x += layer->tx >> 8;
        dst.y += layer->ty >> 8;
        options->opacity = layer->opacity;
        return ol_cmd_layer(list, layer->surface, dst, options);
    }
    struct ol_surface_view view;
    int status = ol_surface_view(layer->surface, &view);
    if (status != OL_OK)
        return status;
    if (view.format != OL_FORMAT_RGBA8_STRAIGHT)
        return OL_UNSUPPORTED;
    /* Surface pixels (view.width x view.height) map onto bounds * scale. */
    long long a = ((long long)layer->bounds.w * s * 256 + view.width / 2) / view.width;
    long long d = ((long long)layer->bounds.h * s * 256 + view.height / 2) / view.height;
    if (a > 0x7fffffff || d > 0x7fffffff)
        return OL_LIMIT;
    if (a < 1 || d < 1)
        return OL_OK; /* collapsed to nothing */
    struct ol_image *image;
    status = image_for(scene, index, &view, &image);
    if (status != OL_OK)
        return status;
    struct gfx_matrix m = {(int)a, 0, 0, (int)d, layer->bounds.x * 256 + layer->tx,
                           layer->bounds.y * 256 + layer->ty};
    return ol_cmd_sprite(list, image, (struct gfx_rect){0, 0, (int)view.width, (int)view.height},
                         &m, &clip, layer->opacity, 1);
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
    struct gfx_rect raw[RAW_DAMAGE];
    int nraw = 0;
    int everything = !scene->initialized || scene->force || target != scene->target ||
                     background != scene->background ||
                     target_view.generation != scene->target_generation ||
                     background_view.generation != scene->background_generation;
    struct ol_scene2d_frame next[OL_SCENE2D_LAYERS];
    for (unsigned i = 0; i < OL_SCENE2D_LAYERS; i++) {
        next[i].layer = scene->layers[i];
        next[i].generation = 0;
        if (next[i].layer.visible) {
            struct ol_surface_view view;
            if (ol_surface_view(next[i].layer.surface, &view) != OL_OK)
                return OL_ARGUMENT;
            next[i].generation = view.generation;
            if (is_xf(&next[i].layer)) {
                long long s = scale_of(&next[i].layer);
                if (view.width == 0 || view.height == 0 ||
                    (long long)next[i].layer.bounds.w * s * 256 / view.width > 0x7fffffff ||
                    (long long)next[i].layer.bounds.h * s * 256 / view.height > 0x7fffffff)
                    return OL_ARGUMENT;
            }
        }
        if (!same(&next[i], &scene->previous[i])) {
            const struct ol_scene2d_layer *o = &scene->previous[i].layer;
            struct gfx_rect old = o->visible
                                      ? outward(o, o->effect_extent, scene->width, scene->height)
                                      : (struct gfx_rect){0, 0, 0, 0};
#ifdef OPENLOGIT_DAMAGE_OLD_DISABLED
            old = (struct gfx_rect){0, 0, 0, 0}; /* Moving-layer oracle must fail. */
#endif
            const struct ol_scene2d_layer *n = &next[i].layer;
            struct gfx_rect now = n->visible
                                      ? outward(n, n->effect_extent, scene->width, scene->height)
                                      : (struct gfx_rect){0, 0, 0, 0};
            if (old.w > 0 && old.h > 0)
                raw[nraw++] = old;
            if (now.w > 0 && now.h > 0)
                raw[nraw++] = now;
        }
    }
    struct gfx_rect damage[OL_SCENE2D_DAMAGE];
    int ndamage;
    if (everything) {
        damage[0] = full;
        ndamage = 1;
    } else {
        ndamage = normalize(raw, nraw);
        for (int i = 0; i < ndamage; i++)
            damage[i] = raw[i];
    }
    if (!ndamage) {
        scene->damage_count = 0;
        return OL_OK;
    }
    int status = ol_list_reset(list);
    struct ol_layer_options options = {.size = sizeof options, .opacity = 255, .bilinear = 0};
    for (int k = 0; k < ndamage && status == OL_OK; k++) {
        struct gfx_rect *rect = &damage[k];
        options.clip = rect;
        options.opacity = 255;
        status = ol_cmd_clear_rect(list, *rect, 0, 0);
        if (status == OL_OK && background)
            status = ol_cmd_layer(list, background, full, &options);
        for (unsigned i = 0; i < OL_SCENE2D_LAYERS && status == OL_OK; i++) {
            const struct ol_scene2d_layer *layer = &next[i].layer;
            if (!layer->visible)
                continue;
            struct gfx_rect own = outward(layer, 0, scene->width, scene->height);
            if (!overlaps(own, *rect))
                continue;
            status = record_layer(scene, list, i, layer, *rect, &options);
        }
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
    for (int i = 0; i < ndamage; i++)
        scene->damage[i] = damage[i];
    scene->damage_count = ndamage;
    scene->target = target;
    scene->background = background;
    ol_surface_view(target, &target_view);
    scene->target_generation = target_view.generation;
    scene->background_generation = background_view.generation;
    scene->initialized = 1;
    scene->force = 0;
    return OL_OK;
}
