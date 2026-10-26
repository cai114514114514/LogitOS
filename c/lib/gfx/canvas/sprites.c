#include "../core/ol_internal.h"

int ol_cmd_nine_slice(struct ol_list *list, struct ol_image *image, struct gfx_rect source,
                      struct gfx_rect destination, struct ol_borders borders,
                      const struct gfx_rect *clip, int opacity, int bilinear)
{
    struct ol_surface_view view;
    if (ol_image_view(image, &view) != OL_OK || view.format != OL_FORMAT_RGBA8_STRAIGHT ||
        source.x < 0 || source.y < 0 || source.w < 1 || source.h < 1 ||
        source.w > (int)view.width || source.h > (int)view.height ||
        source.x > (int)view.width - source.w || source.y > (int)view.height - source.h ||
        borders.left < 0 || borders.top < 0 || borders.right < 0 || borders.bottom < 0 ||
        borders.left >= source.w || borders.right >= source.w - borders.left ||
        borders.top >= source.h || borders.bottom >= source.h - borders.top ||
        destination.w < borders.left + borders.right ||
        destination.h < borders.top + borders.bottom || destination.x < -32768 ||
        destination.y < -32768 || destination.w > 32768 || destination.h > 32768 ||
        destination.x > 32768 - destination.w || destination.y > 32768 - destination.h)
        return ol_list_invalidate(list, OL_ARGUMENT);
    int sx[] = {source.x, source.x + borders.left, source.x + source.w - borders.right,
                source.x + source.w};
    int sy[] = {source.y, source.y + borders.top, source.y + source.h - borders.bottom,
                source.y + source.h};
    int dx[] = {destination.x, destination.x + borders.left,
                destination.x + destination.w - borders.right, destination.x + destination.w};
    int dy[] = {destination.y, destination.y + borders.top,
                destination.y + destination.h - borders.bottom, destination.y + destination.h};
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 3; col++) {
            int sw = sx[col + 1] - sx[col], sh = sy[row + 1] - sy[row];
            int dw = dx[col + 1] - dx[col], dh = dy[row + 1] - dy[row];
            if (!sw || !sh || !dw || !dh)
                continue;
            int status = ol_cmd_image_region(
                list, image, (struct gfx_rect){sx[col], sy[row], sw, sh},
                (struct gfx_rect){dx[col], dy[row], dw, dh}, clip, opacity, bilinear);
            if (status != OL_OK)
                return status;
        }
    }
    return OL_OK;
}

/* Canvas wrappers: clip and opacity come from the state, geometry goes through
 * the CTM. A sprite cannot carry a coverage mask, so refuse rather than draw
 * unmasked pixels (nothing is recorded and the list is not poisoned). */
static int bad_matrix(const struct gfx_matrix *m)
{
    const int e[] = {m->a, m->b, m->c, m->d};
    for (unsigned i = 0; i < 4; i++)
        if (e[i] < -64 * GFX_MONE || e[i] > 64 * GFX_MONE)
            return 1;
    return m->e < -GFX_PX(32768) || m->e > GFX_PX(32768) || m->f < -GFX_PX(32768) ||
           m->f > GFX_PX(32768);
}

int ol_canvas_sprite(struct ol_canvas *canvas, struct ol_image *image, struct gfx_rect src,
                     const struct gfx_matrix *transform, int filter)
{
    if (!canvas)
        return OL_ARGUMENT;
    struct ol_list *list = canvas->list;
    if (!transform || bad_matrix(transform) || (filter != 0 && filter != 1))
        return ol_list_invalidate(list, OL_ARGUMENT);
    if (canvas->state.coverage)
        return OL_UNSUPPORTED;
    struct gfx_matrix m;
    gfx_m_mul(&m, &canvas->state.transform, transform);
    if (bad_matrix(&m))
        return ol_list_invalidate(list, OL_LIMIT);
    return ol_cmd_sprite(list, image, src, &m, &canvas->state.clip, canvas->state.opacity, filter);
}

int ol_canvas_image(struct ol_canvas *canvas, struct ol_image *image, struct gfx_rect src,
                    struct gfx_rect dst, int filter)
{
    if (!canvas)
        return OL_ARGUMENT;
    if (dst.w < 1 || dst.h < 1 || src.w < 1 || src.h < 1 || !rect_ok(dst) || !rect_ok(src))
        return ol_list_invalidate(canvas->list, OL_ARGUMENT);
    const struct gfx_matrix *t = &canvas->state.transform;
    if (t->a == GFX_MONE && t->d == GFX_MONE && !t->b && !t->c && !t->e && !t->f) {
        /* Identity CTM: the exact-edge path the nine-slice already uses. */
        if (filter != 0 && filter != 1)
            return ol_list_invalidate(canvas->list, OL_ARGUMENT);
        if (canvas->state.coverage)
            return OL_UNSUPPORTED;
        return ol_cmd_image_region(canvas->list, image, src, dst, &canvas->state.clip,
                                   canvas->state.opacity, filter);
    }
    long long sx = (long long)dst.w * GFX_MONE / src.w, sy = (long long)dst.h * GFX_MONE / src.h;
    if (sx > 64 * GFX_MONE || sy > 64 * GFX_MONE)
        return ol_list_invalidate(canvas->list, OL_LIMIT);
    struct gfx_matrix local = {(int)sx, 0, 0, (int)sy, GFX_PX(dst.x), GFX_PX(dst.y)};
    return ol_canvas_sprite(canvas, image, src, &local, filter);
}
