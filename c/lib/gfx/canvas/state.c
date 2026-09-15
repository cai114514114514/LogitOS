#include "openlogit_canvas.h"

static int invalid(struct ol_canvas *canvas, int error)
{
    return ol_list_invalidate(canvas ? canvas->list : 0, error);
}

int ol_canvas_init(struct ol_canvas *canvas, struct ol_list *list, int width, int height)
{
    if (!canvas || !list || width < 1 || height < 1 || width > GFX_MAX_W || height > GFX_MAX_W)
        return OL_ARGUMENT;
    gfx_zero(canvas, sizeof *canvas);
    canvas->list = list;
    gfx_m_identity(&canvas->state.transform);
    canvas->state.clip = (struct gfx_rect){0, 0, width, height};
    canvas->state.opacity = 255;
    canvas->state.samples = 4;
    return OL_OK;
}

int ol_canvas_save(struct ol_canvas *canvas)
{
    if (!canvas || canvas->depth == OL_CANVAS_STACK)
        return invalid(canvas, OL_LIMIT);
    canvas->saved[canvas->depth++] = canvas->state;
    return OL_OK;
}

int ol_canvas_restore(struct ol_canvas *canvas)
{
    if (!canvas || !canvas->depth)
        return invalid(canvas, OL_STATE);
    canvas->state = canvas->saved[--canvas->depth];
    return OL_OK;
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
    long long values[] = {((long long)a->a * b->a + (long long)a->c * b->b) / GFX_MONE,
                          ((long long)a->b * b->a + (long long)a->d * b->b) / GFX_MONE,
                          ((long long)a->a * b->c + (long long)a->c * b->d) / GFX_MONE,
                          ((long long)a->b * b->c + (long long)a->d * b->d) / GFX_MONE,
                          ((long long)a->a * b->e + (long long)a->c * b->f) / GFX_MONE + a->e,
                          ((long long)a->b * b->e + (long long)a->d * b->f) / GFX_MONE + a->f};
    for (unsigned i = 0; i < 6; i++) {
        long long limit = i < 4 ? 64 * GFX_MONE : GFX_PX(32768);
        if (values[i] < -limit || values[i] > limit)
            return invalid(canvas, OL_LIMIT);
    }
    canvas->state.transform =
        (struct gfx_matrix){values[0], values[1], values[2], values[3], values[4], values[5]};
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

int ol_canvas_fill(struct ol_canvas *canvas, const struct gfx_path *path, int rule,
                   const struct gfx_paint *paint)
{
    if (!canvas)
        return OL_ARGUMENT;
    struct ol_fill_options fill = options(canvas);
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
