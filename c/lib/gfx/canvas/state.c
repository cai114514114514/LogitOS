#include "../core/ol_internal.h"

static int invalid(struct ol_canvas *canvas, int error)
{
    return ol_list_invalidate(canvas ? canvas->list : 0, error);
}

static struct ol_canvas_state *slots(struct ol_canvas *canvas)
{
    return canvas->stack ? canvas->stack : canvas->saved;
}

int ol_canvas_init(struct ol_canvas *canvas, struct ol_list *list, int width, int height)
{
    return ol_canvas_init_ex(canvas, list, width, height, 0, 0);
}

int ol_canvas_init_ex(struct ol_canvas *canvas, struct ol_list *list, int width, int height,
                      struct ol_canvas_state *stack, int depth)
{
    if (!canvas || (stack && (!aligned(stack) || depth < 1 || depth > 65535)))
        return OL_ARGUMENT;
    if (!canvas || !list || width < 1 || height < 1 || width > GFX_MAX_W || height > GFX_MAX_W)
        return OL_ARGUMENT;
    gfx_zero(canvas, sizeof *canvas);
    canvas->list = list;
    canvas->stack = stack;
    canvas->capacity = stack ? (unsigned)depth : OL_CANVAS_STACK;
    gfx_m_identity(&canvas->state.transform);
    canvas->state.clip = (struct gfx_rect){0, 0, width, height};
    canvas->state.opacity = 255;
    canvas->state.samples = 4;
    return OL_OK;
}

int ol_canvas_save(struct ol_canvas *canvas)
{
    if (!canvas || canvas->depth == canvas->capacity)
        return invalid(canvas, OL_LIMIT);
    slots(canvas)[canvas->depth++] = canvas->state;
    canvas->state.arena_own = 0; /* the pushes of this level start afresh */
    canvas->state.layer_parent = 0; /* only save_layer's own state closes a layer */
    return OL_OK;
}

int ol_canvas_set_samples(struct ol_canvas *canvas, int n)
{
    if (!canvas || n < 1 || n > 32)
        return invalid(canvas, OL_ARGUMENT);
    canvas->state.samples = n;
    return OL_OK;
}

#define LAYER_LIST_DEFAULT 32768ul

int ol_canvas_init_layers(struct ol_canvas *canvas, void *scratch, unsigned long bytes)
{
    if (!canvas || !canvas->list || (scratch && !aligned(scratch)) || (!scratch && bytes))
        return OL_ARGUMENT;
    canvas->layer_arena = scratch;
    canvas->layer_bytes = bytes & ~7ul;
    canvas->layer_used = 0;
    canvas->layer_list_bytes = LAYER_LIST_DEFAULT;
    return OL_OK;
}

int ol_canvas_save_layer(struct ol_canvas *canvas, struct gfx_rect bounds, int opacity,
                         enum gfx_op op)
{
    if (!canvas)
        return OL_ARGUMENT;
    if (op != GFX_OP_SRC_OVER)
        return OL_UNSUPPORTED; /* records nothing, poisons nothing */
    if (opacity < 0 || opacity > 255 || !rect_ok(bounds))
        return invalid(canvas, OL_ARGUMENT);
    if (canvas->depth == canvas->capacity)
        return invalid(canvas, OL_LIMIT);
#ifdef OL_CANVAS_LAYER_PER_CHILD
    /* Negative control: the opacity lands on every child instead of the group. */
    int status = ol_canvas_save(canvas);
    if (status == OL_OK)
        canvas->state.opacity = (canvas->state.opacity * opacity + 127) / 255;
    return status;
#else
    struct gfx_rect c = canvas->state.clip;
    int x0 = bounds.x > c.x ? bounds.x : c.x, y0 = bounds.y > c.y ? bounds.y : c.y;
    int x1 = bounds.x + bounds.w < c.x + c.w ? bounds.x + bounds.w : c.x + c.w;
    int y1 = bounds.y + bounds.h < c.y + c.h ? bounds.y + bounds.h : c.y + c.h;
    struct gfx_rect dst = {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    if (dst.w <= 0 || dst.h <= 0) { /* nothing can show: an empty clip level, no group */
        int status = ol_canvas_save(canvas);
        if (status == OL_OK)
            canvas->state.clip = (struct gfx_rect){0, 0, 0, 0};
        return status;
    }
    if (!canvas->layer_arena || !canvas->list || !device_ok(canvas->list->device) ||
        canvas->layer_list_bytes < ol_list_min_size())
        return OL_LIMIT;
    unsigned long px = (unsigned long)dst.w * dst.h * 4;
    unsigned long room = canvas->layer_bytes - canvas->layer_used;
    unsigned long surf = align8(sizeof(struct ol_surface)), list_bytes = align8(canvas->layer_list_bytes);
    if (canvas->layer_bytes < canvas->layer_used || px > 0x7fffffffu / 2 ||
        surf + 2 * align8(px) + list_bytes > room)
        return OL_LIMIT;
    struct ol_device *device = canvas->list->device;
    unsigned char *base = canvas->layer_arena + canvas->layer_used;
    struct ol_surface *surface = (struct ol_surface *)base;
    unsigned char *front = base + surf, *work = front + align8(px);
    struct ol_list *sub;
    if (ol_list_create(device, work + align8(px), list_bytes, &sub) != OL_OK)
        return OL_LIMIT;
    gfx_zero(surface, sizeof *surface); /* like clip masks: not counted as a device object */
    surface->magic = OL_SURFACE_MAGIC;
    surface->device = device;
    surface->desc = (struct ol_surface_desc){sizeof(struct ol_surface_desc),
                                             OL_FORMAT_RGBA8_STRAIGHT, (unsigned)dst.w,
                                             (unsigned)dst.h, (unsigned)dst.w * 4, front, work,
                                             px, px};
    surface->generation = 1;
    gfx_zero(front, px);
    if (ol_cmd_clear(sub, 0, 0) != OL_OK || ol_canvas_save(canvas) != OL_OK) {
        ol_list_destroy(sub);
        return OL_LIMIT;
    }
    struct ol_canvas_state *s = &canvas->state;
    s->layer_parent = canvas->list;
    s->layer_surface = surface;
    s->layer_dst = dst;
    s->layer_opacity = (opacity * s->opacity + 127) / 255;
    s->layer_mark = canvas->layer_used + surf + align8(px);
    canvas->layer_used += surf + 2 * align8(px) + list_bytes;
    /* Group-local device space: shift CTM, clip and mask origin. */
    s->transform.e -= GFX_PX(dst.x);
    s->transform.f -= GFX_PX(dst.y);
    s->clip = (struct gfx_rect){0, 0, dst.w, dst.h};
    s->mask_x -= dst.x;
    s->mask_y -= dst.y;
    s->opacity = 255;
    canvas->list = sub;
    return OL_OK;
#endif
}

/* Close the group opened by `popped`: submit its sub-list into the group
 * surface and record the single composite on the parent list. */
static int layer_close(struct ol_canvas *canvas, const struct ol_canvas_state *popped)
{
    struct ol_list *sub = canvas->list, *parent = popped->layer_parent;
    struct ol_device *device = sub->device;
    int status = ol_list_close(sub);
    if (status == OL_OK)
        status = ol_submit(device, sub, popped->layer_surface, 0);
    ol_list_destroy(sub);
    canvas->list = parent;
    canvas->layer_used = popped->layer_mark;
    if (status != OL_OK)
        return ol_list_invalidate(parent, status);
    struct ol_layer_options options = {.size = sizeof options, .opacity = popped->layer_opacity};
    return ol_cmd_layer(parent, popped->layer_surface, popped->layer_dst, &options);
}

int ol_canvas_restore(struct ol_canvas *canvas)
{
    if (!canvas || !canvas->depth)
        return invalid(canvas, OL_STATE);
    struct ol_canvas_state popped = canvas->state;
    canvas->state = slots(canvas)[--canvas->depth];
    ol_canvas_clip_pop(canvas, &popped); /* against the list the pushes recorded into */
    return popped.layer_parent ? layer_close(canvas, &popped) : OL_OK;
}

int ol_canvas_transform(struct ol_canvas *canvas, const struct gfx_matrix *matrix)
{
    if (!canvas || !matrix)
        return invalid(canvas, OL_ARGUMENT);
    const struct gfx_matrix *a = &canvas->state.transform;
    const struct gfx_matrix *b = matrix;
    /* Bound operands before multiplication: accepting arbitrary signed 32-bit
     * matrices can overflow a sum of two signed 64-bit products. */
    const int entries[] = {b->a, b->b, b->c, b->d};
    for (unsigned i = 0; i < 4; i++)
        if (entries[i] < -64 * GFX_MONE || entries[i] > 64 * GFX_MONE)
            return invalid(canvas, OL_LIMIT);
    if (b->e < -GFX_PX(32768) || b->e > GFX_PX(32768) || b->f < -GFX_PX(32768) ||
        b->f > GFX_PX(32768))
        return invalid(canvas, OL_LIMIT);
    /* Operands are bounded above, so the products fit gfx_m_mul's int results. */
    struct gfx_matrix m;
    gfx_m_mul(&m, a, b);
    const int values[] = {m.a, m.b, m.c, m.d, m.e, m.f};
    for (unsigned i = 0; i < 6; i++) {
        long long limit = i < 4 ? 64 * GFX_MONE : GFX_PX(32768);
        if (values[i] < -limit || values[i] > limit)
            return invalid(canvas, OL_LIMIT);
    }
    canvas->state.transform = m;
    return OL_OK;
}

int ol_canvas_clip(struct ol_canvas *canvas, struct gfx_rect rect)
{
    if (!canvas || rect.w < 0 || rect.h < 0 || rect.x < -32768 || rect.y < -32768 ||
        rect.w > 32768 || rect.h > 32768 || rect.x > 32768 - rect.w || rect.y > 32768 - rect.h)
        return invalid(canvas, OL_ARGUMENT);
    struct gfx_rect old = canvas->state.clip;
    int x = old.x > rect.x ? old.x : rect.x;
    int y = old.y > rect.y ? old.y : rect.y;
    int right = old.x + old.w < rect.x + rect.w ? old.x + old.w : rect.x + rect.w;
    int bottom = old.y + old.h < rect.y + rect.h ? old.y + old.h : rect.y + rect.h;
    canvas->state.clip =
        (struct gfx_rect){x, y, right > x ? right - x : 0, bottom > y ? bottom - y : 0};
    return OL_OK;
}

int ol_canvas_opacity(struct ol_canvas *canvas, int opacity)
{
    if (!canvas || opacity < 0 || opacity > 255)
        return invalid(canvas, OL_ARGUMENT);
    canvas->state.opacity = opacity;
    return OL_OK;
}

int ol_canvas_mask(struct ol_canvas *canvas, struct ol_image *coverage, int x, int y)
{
    struct ol_surface_view view;
    if (!canvas || x < -32768 || x > 32768 || y < -32768 || y > 32768 ||
        (coverage && (ol_image_view(coverage, &view) != OL_OK || view.format != OL_FORMAT_A8)))
        return invalid(canvas, OL_ARGUMENT);
    canvas->state.coverage = coverage;
    canvas->state.mask_x = x;
    canvas->state.mask_y = y;
    return OL_OK;
}

static struct ol_fill_options options(const struct ol_canvas *canvas)
{
    return (struct ol_fill_options){.size = sizeof(struct ol_fill_options),
                                    .transform = &canvas->state.transform,
                                    .clip = &canvas->state.clip,
                                    .coverage = canvas->state.coverage,
                                    .mask_x = canvas->state.mask_x,
                                    .mask_y = canvas->state.mask_y,
                                    .opacity = canvas->state.opacity,
                                    .samples = canvas->state.samples};
}

int ol_canvas_path_begin(struct ol_canvas *canvas, struct gfx_path *path)
{
    if (!canvas || !path)
        return OL_ARGUMENT;
    if (path->npt > 0 || path->open)
        return invalid(canvas, OL_STATE);
#ifdef OL_CANVAS_POST_FLATTEN
    /* Negative control: leave the path in user space; the fill transforms the
     * flattened points, so a curve under a large scale turns polygonal. */
    return OL_OK;
#else
    const struct gfx_matrix *ctm = &canvas->state.transform;
    int base = path->tol;
    int scale = gfx_m_scale_of(ctm);
    gfx_path_matrix(path, ctm);
    if (scale > GFX_MONE)
        gfx_path_tolerance(path, (int)((long long)base * GFX_MONE / scale));
    canvas->device_path = path;
    return OL_OK;
#endif
}

int ol_canvas_path_is_device(const struct ol_canvas *canvas, const struct gfx_path *path)
{
    if (!canvas || !path || path != canvas->device_path)
        return 0;
    const struct gfx_matrix *a = &path->m, *b = &canvas->state.transform;
    return a->a == b->a && a->b == b->b && a->c == b->c && a->d == b->d && a->e == b->e &&
           a->f == b->f;
}

/* A gradient is written in the canvas's user space, so a fill recorded under a CTM carries
 * that CTM as the paint matrix (composed after any pmat the caller set): the gradient then
 * rotates and scales with the shape. An identity CTM records the paint untouched. Image
 * paints are not touched (they carry their own matrix). `*copy` is scratch for the result. */
static const struct gfx_paint *paint_in_ctm(const struct ol_canvas *canvas,
                                            const struct gfx_paint *paint, struct gfx_paint *out)
{
    if (!paint || paint->kind < GFX_LINEAR || paint->kind == GFX_IMAGE)
        return paint;
    const struct gfx_matrix *m = &canvas->state.transform;
    if (m->a == GFX_MONE && m->b == 0 && m->c == 0 && m->d == GFX_MONE && m->e == 0 && m->f == 0)
        return paint;
    copy(out, paint, sizeof *out);
    if (paint->flags & GFX_PAINT_HAS_PMAT)
        gfx_m_mul(&out->pmat, m, &paint->pmat);
    else
        out->pmat = *m;
    out->flags |= GFX_PAINT_HAS_PMAT;
    return out;
}

int ol_canvas_fill(struct ol_canvas *canvas, const struct gfx_path *path, int rule,
                   const struct gfx_paint *paint)
{
    if (!canvas)
        return OL_ARGUMENT;
    struct gfx_paint in_ctm;
    paint = paint_in_ctm(canvas, paint, &in_ctm);
    struct ol_fill_options fill = options(canvas);
    struct gfx_matrix identity;
    if (ol_canvas_path_is_device(canvas, path)) {
        gfx_m_identity(&identity);
        fill.transform = &identity;
    }
    return ol_cmd_fill_ex(canvas->list, path, rule, paint, &fill);
}

int ol_canvas_stroke(struct ol_canvas *canvas, const struct gfx_path *path,
                     const struct gfx_stroke *stroke, const struct gfx_paint *paint,
                     struct gfx_path *scratch)
{
    if (!canvas)
        return OL_ARGUMENT;
    struct ol_fill_options fill = options(canvas);
    return ol_cmd_stroke(canvas->list, path, stroke, paint, &fill, scratch);
}

int ol_canvas_round_rect(struct ol_canvas *canvas, struct gfx_rect rect, int radius,
                         const struct gfx_paint *paint)
{
    if (rect.x < -32768 || rect.y < -32768 || rect.w < 0 || rect.h < 0 || rect.w > 32768 ||
        rect.h > 32768 || rect.x > 32768 - rect.w || rect.y > 32768 - rect.h || radius < 0 ||
        radius > 32768)
        return invalid(canvas, OL_ARGUMENT);
    int points[1024], contours[8];
    struct gfx_path path;
    gfx_path_init(&path, points, 512, contours, 8);
    gfx_path_rrect(&path, GFX_PX(rect.x), GFX_PX(rect.y), GFX_PX(rect.w), GFX_PX(rect.h),
                   GFX_PX(radius));
    return ol_canvas_fill(canvas, &path, GFX_NONZERO, paint);
}
