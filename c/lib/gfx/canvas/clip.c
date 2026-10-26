/* Canvas clip stack: rounded-rect and path clips that MULTIPLY with the
 * current A8 mask. Masks live in a caller-provided arena referenced from the
 * canvas, so this file has no file-scope state and is per context by
 * construction.
 *
 * Arena layout: masks grow from the front (an ol_image header plus w*h bytes
 * each). A path clip stages its device-space copy of the path at the very end
 * of the arena for the duration of the call only.
 *
 * Lifetime: a recorded fill references its mask by pointer until the list is
 * reset, so ol_canvas_restore only rewinds the arena when nothing was recorded
 * since the popped state pushed (see ol_canvas_clip_pop). Otherwise the space
 * is kept until ol_canvas_init_clip_arena is called again. */
#include "../core/ol_internal.h"

#define CLIP_EDGE_MAX 32768

static int invalid(struct ol_canvas *canvas, int error)
{
    return ol_list_invalidate(canvas ? canvas->list : 0, error);
}

static int rect_inside(struct gfx_rect r)
{
    return r.w >= 0 && r.h >= 0 && r.w <= CLIP_EDGE_MAX && r.h <= CLIP_EDGE_MAX &&
           r.x >= -CLIP_EDGE_MAX && r.y >= -CLIP_EDGE_MAX && r.x <= CLIP_EDGE_MAX - r.w &&
           r.y <= CLIP_EDGE_MAX - r.h;
}

static struct gfx_rect isect(struct gfx_rect a, struct gfx_rect b)
{
    int x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int r = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int d = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct gfx_rect){x, y, r > x ? r - x : 0, d > y ? d - y : 0};
}

int ol_canvas_init_clip_arena(struct ol_canvas *canvas, void *a8_scratch, unsigned long bytes)
{
    if (!canvas || !canvas->list || (a8_scratch && !aligned(a8_scratch)) || (!a8_scratch && bytes))
        return OL_ARGUMENT;
    canvas->clip_arena = a8_scratch;
    canvas->clip_arena_bytes = bytes & ~7ul;
    canvas->clip_arena_used = 0;
    canvas->state.arena_own = 0;
    return OL_OK;
}

/* Reserve a mask of w*h bytes plus header, keeping `tail` bytes free at the end. */
static int reserve(struct ol_canvas *canvas, int w, int h, unsigned long tail, struct ol_image **im,
                   unsigned char **cov)
{
    if (!canvas->clip_arena)
        return OL_LIMIT;
    unsigned long need = align8(sizeof(struct ol_image)) + align8((unsigned long)w * h);
    unsigned long room = canvas->clip_arena_bytes - canvas->clip_arena_used;
    if (canvas->clip_arena_bytes < canvas->clip_arena_used || tail > room || need > room - tail)
        return OL_LIMIT;
    unsigned char *base = canvas->clip_arena + canvas->clip_arena_used;
    *im = (struct ol_image *)base;
    *cov = base + align8(sizeof(struct ol_image));
    return OL_OK;
}

static void commit(struct ol_canvas *canvas, struct ol_image *im, unsigned char *cov,
                   struct gfx_rect box)
{
    struct ol_canvas_state *s = &canvas->state;
    struct ol_image_desc d = {sizeof d, OL_FORMAT_A8, box.w, box.h, box.w, cov,
                              (unsigned long)box.w * box.h};
    /* Built directly, not through ol_image_create: popped masks are never
     * destroyed one by one, so they must not count as live device objects. */
    *im = (struct ol_image){OL_IMAGE_MAGIC, 0, canvas->list->device, d, 1};
    unsigned long end = (unsigned long)((unsigned char *)cov - canvas->clip_arena) +
                        align8((unsigned long)box.w * box.h);
    if (!s->arena_own || canvas->list->used != s->arena_stamp) {
        s->arena_prev = canvas->clip_arena_used;
        s->arena_stamp = canvas->list->used;
    }
    s->arena_own = 1;
    canvas->clip_arena_used = end;
    s->coverage = im;
    s->mask_x = box.x;
    s->mask_y = box.y;
    s->clip = box;
}

void ol_canvas_clip_pop(struct ol_canvas *canvas, const struct ol_canvas_state *popped)
{
    if (popped->arena_own && canvas->list && canvas->list->used == popped->arena_stamp &&
        popped->arena_prev <= canvas->clip_arena_used)
        canvas->clip_arena_used = popped->arena_prev;
}

static unsigned char old_at(const struct ol_surface_view *v, int ox, int oy, int x, int y)
{
    unsigned lx = (unsigned)(x - ox), ly = (unsigned)(y - oy);
    return v && lx < v->width && ly < v->height ? v->pixels[(unsigned long)ly * v->stride + lx] : 0;
}

static int old_view(const struct ol_canvas *canvas, struct ol_surface_view *v)
{
    return canvas->state.coverage && ol_image_view(canvas->state.coverage, v) == OL_OK;
}

static struct gfx_rect view_rect(const struct ol_canvas *canvas, const struct ol_surface_view *v)
{
    return (struct gfx_rect){canvas->state.mask_x, canvas->state.mask_y, v->width, v->height};
}

/* Rasterise a DEVICE-space path into a new mask multiplied by the current one. */
static int push_device_path(struct ol_canvas *canvas, const struct gfx_path *dev, int rule,
                            unsigned long tail)
{
    int x0, y0, x1, y1;
    struct gfx_rect box = canvas->state.clip;
    struct ol_surface_view ov;
    int has_old = old_view(canvas, &ov);
    if (dev->overflow || (rule != GFX_NONZERO && rule != GFX_EVENODD))
        return OL_ARGUMENT;
    if (!gfx_path_bounds(dev, &x0, &y0, &x1, &y1)) {
        box.w = box.h = 0;
    } else {
        box = isect(box, (struct gfx_rect){x0, y0, x1 - x0, y1 - y0});
        if (has_old)
            box = isect(box, view_rect(canvas, &ov));
    }
    if (box.w <= 0 || box.h <= 0) {
        canvas->state.clip = (struct gfx_rect){0, 0, 0, 0};
        return OL_OK;
    }
    if (box.w > GFX_CLIP_MASK_MAX || box.h > GFX_CLIP_MASK_MAX)
        return OL_LIMIT;
    struct ol_image *im;
    unsigned char *cov;
    int r = reserve(canvas, box.w, box.h, tail, &im, &cov);
    if (r)
        return r;
    int subs = canvas->state.samples;
    subs = subs < 1 ? 1 : subs > GFX_MAX_SUBS ? GFX_MAX_SUBS : subs;
    if (!gfx_fill_mask_subs(dev, rule, cov, box.w, box.h, box.x, box.y, subs))
        return OL_ARGUMENT;
#ifndef OL_CANVAS_CLIP_REPLACE
    for (int y = 0; y < box.h; y++)
        for (int x = 0; x < box.w; x++) {
            if (has_old) {
                unsigned c = old_at(&ov, canvas->state.mask_x, canvas->state.mask_y, box.x + x,
                                    box.y + y);
                cov[y * box.w + x] = (cov[y * box.w + x] * c + 127) / 255;
            }
        }
#endif
    commit(canvas, im, cov, box);
    return OL_OK;
}

int ol_canvas_clip_rrect(struct ol_canvas *canvas, struct gfx_rect r, const int radius[4])
{
    if (!canvas || !canvas->list || !radius || !rect_inside(r))
        return invalid(canvas, OL_ARGUMENT);
    for (int i = 0; i < 4; i++)
        if (radius[i] < 0 || radius[i] > CLIP_EDGE_MAX)
            return invalid(canvas, OL_ARGUMENT);
    int points[1024], contours[8];
    struct gfx_path path;
    gfx_path_init(&path, points, 512, contours, 8);
    gfx_path_matrix(&path, &canvas->state.transform);
    /* Chords sit inside the arc, so the default 1/32 px tolerance darkens a
     * curved edge by up to 8/255. Clips are multiplied, so tighten it while
     * the point budget still holds (segments grow with sqrt(radius/tol)). */
    int maxr = radius[0];
    for (int i = 1; i < 4; i++)
        maxr = radius[i] > maxr ? radius[i] : maxr;
    gfx_path_tolerance(&path, maxr <= 64 ? 1 : maxr <= 256 ? 2 : GFX_ONE / 32);
    gfx_path_rrect4(&path, GFX_PX(r.x), GFX_PX(r.y), GFX_PX(r.w), GFX_PX(r.h), GFX_PX(radius[0]),
                    GFX_PX(radius[1]), GFX_PX(radius[2]), GFX_PX(radius[3]));
    int e = push_device_path(canvas, &path, GFX_NONZERO, 0);
    return e == OL_LIMIT ? e : e ? invalid(canvas, e) : OL_OK;
}

int ol_canvas_clip_path(struct ol_canvas *canvas, const struct gfx_path *p, int rule)
{
    if (!canvas || !canvas->list || !p || p->overflow || p->npt < 0 || p->nsub < 0 ||
        (rule != GFX_NONZERO && rule != GFX_EVENODD))
        return invalid(canvas, OL_ARGUMENT);
    /* Paths are recorded in canvas user space and carried to device space by
     * the CTM, exactly as ol_cmd_fill_ex does. Stage the copy at the arena end. */
    unsigned long ptb = align8((unsigned long)p->npt * 8), sb = align8((unsigned long)p->nsub * 4);
    unsigned long tail = ptb + sb;
    if (!canvas->clip_arena || tail > canvas->clip_arena_bytes)
        return OL_LIMIT;
    int *tp = (int *)(canvas->clip_arena + canvas->clip_arena_bytes - tail);
    int *ts = (int *)((unsigned char *)tp + ptb);
    const struct gfx_matrix *m = &canvas->state.transform;
    for (int i = 0; i < p->npt; i++) {
        int x = p->pt[i * 2], y = p->pt[i * 2 + 1];
        long long xx = ((long long)m->a * x + (long long)m->c * y) / GFX_MONE + m->e;
        long long yy = ((long long)m->b * x + (long long)m->d * y) / GFX_MONE + m->f;
        if (xx < -GFX_PX(32768) || xx > GFX_PX(32768) || yy < -GFX_PX(32768) ||
            yy > GFX_PX(32768))
            return OL_LIMIT;
        tp[i * 2] = (int)xx;
        tp[i * 2 + 1] = (int)yy;
    }
    for (int i = 0; i < p->nsub; i++)
        ts[i] = p->sub[i];
    struct gfx_path dev = *p;
    dev.pt = tp;
    dev.sub = ts;
    dev.ptcap = p->npt;
    dev.subcap = p->nsub;
    int e = push_device_path(canvas, &dev, rule, tail);
    return e == OL_LIMIT ? e : e ? invalid(canvas, e) : OL_OK;
}

int ol_canvas_mask_ex(struct ol_canvas *canvas, struct ol_image *coverage, int x, int y,
                      int intersect)
{
    struct ol_surface_view nv, ov;
    if (!intersect)
        return ol_canvas_mask(canvas, coverage, x, y);
    if (!canvas || !canvas->list || x < -32768 || x > 32768 || y < -32768 || y > 32768 ||
        (coverage && (ol_image_view(coverage, &nv) != OL_OK || nv.format != OL_FORMAT_A8)))
        return invalid(canvas, OL_ARGUMENT);
    if (!coverage)
        return OL_OK;
    if (!old_view(canvas, &ov))
        return ol_canvas_mask(canvas, coverage, x, y);
    struct gfx_rect box = isect((struct gfx_rect){x, y, nv.width, nv.height},
                                view_rect(canvas, &ov));
    if (box.w <= 0 || box.h <= 0) {
        canvas->state.clip = (struct gfx_rect){0, 0, 0, 0};
        return OL_OK;
    }
    if (box.w > GFX_CLIP_MASK_MAX || box.h > GFX_CLIP_MASK_MAX)
        return OL_LIMIT;
    struct ol_image *im;
    unsigned char *cov;
    int r = reserve(canvas, box.w, box.h, 0, &im, &cov);
    if (r)
        return r;
    for (int j = 0; j < box.h; j++)
        for (int i = 0; i < box.w; i++) {
            unsigned s = old_at(&nv, x, y, box.x + i, box.y + j);
#ifdef OL_CANVAS_CLIP_REPLACE
            cov[j * box.w + i] = s;
#else
            unsigned c = old_at(&ov, canvas->state.mask_x, canvas->state.mask_y, box.x + i,
                                box.y + j);
            cov[j * box.w + i] = (s * c + 127) / 255;
#endif
        }
    struct gfx_rect keep = canvas->state.clip;
    commit(canvas, im, cov, box);
    canvas->state.clip = isect(keep, box);
    return OL_OK;
}
