#include "ol_internal.h"

unsigned long ol_list_min_size(void)
{
    return align8(sizeof(struct ol_list)) + align8(sizeof(struct command));
}
int ol_list_create(struct ol_device *d, void *storage, unsigned long bytes, struct ol_list **out)
{
    if (!out)
        return OL_ARGUMENT;
    *out = 0;
    if (!device_ok(d) || !aligned(storage) || bytes < ol_list_min_size() || bytes > OL_COMMAND_MAX)
        return OL_ARGUMENT;
    struct ol_list *l = storage;
    gfx_zero(l, sizeof *l);
    l->magic = OL_LIST_MAGIC;
    l->device = d;
    l->capacity = bytes;
    l->used = align8(sizeof *l);
    d->objects++;
    *out = l;
    return OL_OK;
}
int ol_list_reset(struct ol_list *l)
{
    if (!list_ok(l))
        return OL_ARGUMENT;
    if (l->device->busy)
        return OL_BUSY;
    for (unsigned long o = align8(sizeof *l); o < l->used;) {
        struct command *c = (void *)((unsigned char *)l + o);
        if (c->image) c->image->refs--;
        if (c->resource) c->resource->refs--;
        if (c->mask) c->mask->refs--;
        o += c->bytes;
    }
    l->used = align8(sizeof *l);
    l->closed = 0;
    l->error = 0;
    return OL_OK;
}
int ol_list_destroy(struct ol_list *l)
{
    int r = ol_list_reset(l);
    if (r)
        return r;
    l->device->objects--;
    l->magic = 0;
    return OL_OK;
}
int ol_list_invalidate(struct ol_list *list, int error)
{
    if (error >= OL_OK || error < OL_BACKEND_FAILED)
        error = OL_ARGUMENT;
    return fail(list, error);
}
static struct command *record(struct ol_list *l, unsigned op, unsigned long extra)
{
    if (!list_ok(l))
        return 0;
    if (l->closed || l->device->busy) {
        fail(l, OL_STATE);
        return 0;
    }
    if (l->error)
        return 0;
    unsigned long n = align8(sizeof(struct command)) + align8(extra);
    if (n > l->capacity - l->used) {
        fail(l, OL_LIMIT);
        return 0;
    }
    struct command *c = (void *)((unsigned char *)l + l->used);
    gfx_zero(c, sizeof *c);
    c->op = op;
    c->bytes = n;
    l->used += n;
    return c;
}
int ol_cmd_clear(struct ol_list *l, unsigned rgb, int alpha)
{
    if (alpha < 0 || alpha > 255)
        return fail(l, OL_ARGUMENT);
    struct command *c = record(l, CMD_CLEAR, 0);
    if (!c)
        return record_error(l);
    c->rgb = rgb;
    c->alpha = alpha;
    return OL_OK;
}
int ol_cmd_clear_rect(struct ol_list *list, struct gfx_rect rect, unsigned rgb, int alpha)
{
    if (!rect_ok(rect) || alpha < 0 || alpha > 255)
        return fail(list, OL_ARGUMENT);
    struct command *command = record(list, CMD_CLEAR_RECT, 0);
    if (!command)
        return record_error(list);
    command->rect = rect;
    command->rgb = rgb;
    command->alpha = alpha;
    return OL_OK;
}
static int path_ok(const struct gfx_path *p)
{
    if (!p || p->overflow || p->npt < 0 || p->nsub < 0 || p->npt > p->ptcap ||
        p->nsub > p->subcap || p->npt > GFX_MAX_EDGES || p->nsub > p->npt)
        return 0;
    if (!p->npt)
        return p->nsub == 0;
    if (!p->pt || !p->sub || !p->nsub || gfx_path_sub_start(p, 0) != 0)
        return 0;
    for (int i = 0; i < p->nsub; i++) {
        /* The closed bit is the only flag allowed above the index; any other
         * high bit or a negative entry is corrupt. */
        if (p->sub[i] < 0 || (p->sub[i] & ~(GFX_SUB_CLOSED | (GFX_SUB_CLOSED - 1))) ||
            gfx_path_sub_start(p, i) >= p->npt ||
            (i && gfx_path_sub_start(p, i) <= gfx_path_sub_start(p, i - 1)))
            return 0;
    }
    for (int i = 0; i < 2 * p->npt; i++)
        if (p->pt[i] < -GFX_PX(32768) || p->pt[i] > GFX_PX(32768))
            return 0;
    return 1;
}
static int paint_ok(const struct gfx_paint *p)
{
    if (!p || p->kind < GFX_SOLID || p->kind > GFX_CONIC || p->kind == GFX_IMAGE ||
        p->spread > GFX_SPREAD_REFLECT || p->alpha < 0 || p->alpha > 255 ||
        p->global_alpha < 0 || p->global_alpha > 255 || p->nstop < 0 || p->nstop > GFX_MAX_STOPS)
        return 0;
    for (int i = 0; i < p->nstop; i++)
        if (p->stop[i].alpha < 0 || p->stop[i].alpha > 255 || p->stop[i].t < 0 ||
            p->stop[i].t > 65536 || (i && p->stop[i].t < p->stop[i - 1].t))
            return 0;
    return 1;
}
int ol_cmd_fill(struct ol_list *l, const struct gfx_path *p, int rule,
                const struct gfx_paint *paint, const struct gfx_rect *clip, int samples)
{
    if (p && p->overflow)
        return fail(l, OL_LIMIT);
    if (!path_ok(p) || !paint_ok(paint) || (rule != GFX_NONZERO && rule != GFX_EVENODD) ||
        samples < 1 || samples > GFX_MAX_SUBS || (clip && !rect_ok(*clip)))
        return fail(l, OL_ARGUMENT);
    unsigned long points = (unsigned long)p->npt * 2 * sizeof(int),
                  subs = (unsigned long)p->nsub * sizeof(int);
    struct command *c = record(l, CMD_FILL, points + subs);
    if (!c)
        return record_error(l);
    c->path = *p;
    /* Clang lowers this large struct assignment to memcpy even in a
     * freestanding build. The core also serves GUI clients without libc;
     * use its bounded copy primitive so that link never acquires a libc
     * dependency merely because a paint descriptor grew. */
    copy(&c->paint, paint, sizeof c->paint);
    c->rule = rule;
    c->samples = samples;
    c->has_clip = clip != 0;
    if (clip)
        c->rect = *clip;
    unsigned char *bytes = (void *)c;
    bytes += align8(sizeof *c);
    c->path.pt = (void *)bytes;
    c->path.sub = (void *)(bytes + points);
    copy(c->path.pt, p->pt, points);
    copy(c->path.sub, p->sub, subs);
    return OL_OK;
}
int ol_cmd_image(struct ol_list *l, struct ol_surface *image, struct gfx_rect dst, int alpha,
                 int bilinear)
{
    if (!list_ok(l) || !surface_ok(image) || image->device != l->device || !rect_ok(dst) ||
        dst.w == 0 || dst.h == 0 || alpha < 0 || alpha > 255 || (bilinear != 0 && bilinear != 1))
        return fail(l, OL_ARGUMENT);
    struct command *c = record(l, CMD_IMAGE, 0);
    if (!c)
        return record_error(l);
    c->image = image;
    c->generation = image->generation;
    c->rect = dst;
    c->alpha = alpha;
    c->samples = bilinear;
    image->refs++;
    return OL_OK;
}
int ol_list_close(struct ol_list *l)
{
    if (!list_ok(l))
        return OL_ARGUMENT;
    if (l->error)
        return l->error;
    if (l->closed)
        return OL_STATE;
    l->closed = 1;
    return OL_OK;
}
int ol_cmd_fill_ex(struct ol_list *l,const struct gfx_path *p,int rule,
                   const struct gfx_paint *paint,const struct ol_fill_options *o)
{
    if(!list_ok(l)||!o||o->size<sizeof *o||o->opacity<0||o->opacity>255)
        return fail(l,OL_ARGUMENT);
    if(o->coverage && (!image_ok(o->coverage)||o->coverage->device!=l->device ||
       o->coverage->desc.format!=OL_FORMAT_A8 ||
       o->coverage->desc.width>GFX_CLIP_MASK_MAX ||o->coverage->desc.height>GFX_CLIP_MASK_MAX ||
       o->mask_x < -32768 || o->mask_x > 32768 || o->mask_y < -32768 || o->mask_y > 32768))
        return fail(l,OL_ARGUMENT);
    unsigned long start=l->used;
    int r=ol_cmd_fill(l,p,rule,paint,o->clip,o->samples);
    if(r)return r;
    struct command *c=(void *)((unsigned char *)l+start);
    c->paint.global_alpha=(c->paint.global_alpha*o->opacity+127)/255;
    if(o->transform) {
        const struct gfx_matrix *m=o->transform;
        for(int i=0;i<c->path.npt;i++) {
            int x=c->path.pt[i*2],y=c->path.pt[i*2+1];
            long long xx=((long long)m->a*x+(long long)m->c*y)/GFX_MONE+m->e;
            long long yy=((long long)m->b*x+(long long)m->d*y)/GFX_MONE+m->f;
            if(xx < -GFX_PX(32768)||xx > GFX_PX(32768)||yy < -GFX_PX(32768)||yy > GFX_PX(32768))
                return fail(l,OL_LIMIT);
            c->path.pt[i*2]=(int)xx;c->path.pt[i*2+1]=(int)yy;
        }
    }
    c->mask=o->coverage;c->mask_x=o->mask_x;c->mask_y=o->mask_y;
    if(c->mask){c->mask->refs++;c->mask_generation=c->mask->generation;}
    return OL_OK;
}
int ol_cmd_stroke(struct ol_list *l,const struct gfx_path *p,const struct gfx_stroke *stroke,
                  const struct gfx_paint *paint,const struct ol_fill_options *o,struct gfx_path *scratch)
{
    if(!p||!stroke||!scratch||scratch==p)return fail(l,OL_ARGUMENT);
    gfx_path_reset(scratch);
    if(!gfx_stroke_path(scratch,p,stroke))return fail(l,OL_LIMIT);
    return ol_cmd_fill_ex(l,scratch,GFX_NONZERO,paint,o);
}
int ol_cmd_image_resource(struct ol_list *l,struct ol_image *im,struct gfx_rect r,int alpha,int bilinear)
{
    if(!list_ok(l)||!image_ok(im)||im->device!=l->device||im->desc.format!=OL_FORMAT_RGBA8_STRAIGHT||
       !rect_ok(r)||r.w<=0||r.h<=0||alpha<0||alpha>255||(bilinear!=0&&bilinear!=1))
        return fail(l,OL_ARGUMENT);
    struct command *c=record(l,CMD_RESOURCE,0);if(!c)return record_error(l);
    c->resource=im;c->resource_generation=im->generation;c->rect=r;c->alpha=alpha;
    c->samples=bilinear;im->refs++;return OL_OK;
}
int ol_cmd_glyph_mask(struct ol_list *l,struct ol_image *im,int x,int y,unsigned rgb,int alpha)
{
    if(!list_ok(l)||!image_ok(im)||im->device!=l->device||im->desc.format!=OL_FORMAT_A8||
       alpha<0||alpha>255)return fail(l,OL_ARGUMENT);
    struct gfx_rect r={x,y,im->desc.width,im->desc.height};if(!rect_ok(r))return fail(l,OL_ARGUMENT);
    struct command *c=record(l,CMD_GLYPH,0);if(!c)return record_error(l);
    c->resource=im;c->resource_generation=im->generation;c->rect=r;c->alpha=alpha;c->rgb=rgb;
    im->refs++;return OL_OK;
}

int ol_cmd_sprite(struct ol_list *list, struct ol_image *image, struct gfx_rect source,
                  const struct gfx_matrix *transform, const struct gfx_rect *clip,
                  int opacity, int bilinear)
{
    if (!list_ok(list) || !image_ok(image) || image->device != list->device ||
        image->desc.format != OL_FORMAT_RGBA8_STRAIGHT || !transform ||
        !rect_ok(source) || source.x < 0 || source.y < 0 || source.w <= 0 || source.h <= 0 ||
        (unsigned)(source.x + source.w) > image->desc.width ||
        (unsigned)(source.y + source.h) > image->desc.height ||
        opacity < 0 || opacity > 255 || (bilinear != 0 && bilinear != 1))
        return fail(list, OL_ARGUMENT);
    const unsigned char *pixels = image->desc.pixels +
        (unsigned long)source.y * image->desc.stride + source.x * 4;
    struct gfx_paint image_paint, solid;
    if (!gfx_paint_image(&image_paint, pixels, source.w, source.h, image->desc.stride,
                         transform, bilinear))
        return fail(list, OL_ARGUMENT);
    int points[16], contours[2];
    struct gfx_path path;
    gfx_path_init(&path, points, 8, contours, 2);
    gfx_path_rect(&path, 0, 0, GFX_PX(source.w), GFX_PX(source.h));
    gfx_paint_solid(&solid, 0, 255);
    struct ol_fill_options options = {
        .size = sizeof options, .transform = transform, .clip = clip,
        .opacity = opacity, .samples = 4
    };
    unsigned long start = list->used;
    int status = ol_cmd_fill_ex(list, &path, GFX_NONZERO, &solid, &options);
    if (status != OL_OK)
        return status;
    /* Borrowed image paints are deliberately refused by ol_cmd_fill. Attach
     * this one only after validating and retaining its versioned resource. */
    struct command *command = (void *)((unsigned char *)list + start);
    copy(&command->paint, &image_paint, sizeof image_paint);
    command->paint.bilinear = bilinear ? GFX_FILTER_LINEAR_CLAMP : GFX_FILTER_NEAREST;
    command->paint.global_alpha = opacity;
    command->resource = image;
    command->resource_generation = image->generation;
    image->refs++;
    return OL_OK;
}

int ol_cmd_image_region(struct ol_list *list, struct ol_image *image, struct gfx_rect source,
                        struct gfx_rect destination, const struct gfx_rect *clip,
                        int opacity, int bilinear)
{
    if (!list_ok(list) || !rect_ok(destination) || destination.w <= 0 || destination.h <= 0 ||
        source.w < 1 || source.h < 1)
        return fail(list, OL_ARGUMENT);
    long long scale_x = (long long)destination.w*GFX_MONE/source.w;
    long long scale_y = (long long)destination.h*GFX_MONE/source.h;
    if (scale_x > 0x7fffffff || scale_y > 0x7fffffff)
        return fail(list, OL_LIMIT);
    struct gfx_matrix matrix = {scale_x,0,0,scale_y,GFX_PX(destination.x),GFX_PX(destination.y)};
    unsigned long start = list->used;
    int status = ol_cmd_sprite(list,image,source,&matrix,clip,opacity,bilinear);
    if (status != OL_OK) return status;
    struct command *command = (void *)((unsigned char *)list+start);
    /* The inverse texture matrix is quantized to 16.16. Coverage must still
     * end at the exact destination edge, especially between nine-slice tiles. */
    gfx_path_reset(&command->path);
    gfx_path_rect(&command->path,GFX_PX(destination.x),GFX_PX(destination.y),
                   GFX_PX(destination.w),GFX_PX(destination.h));
    return OL_OK;
}
int ol_cmd_layer(struct ol_list *l,struct ol_surface *source,struct gfx_rect dst,
                  const struct ol_layer_options *options)
{
    if(!options||options->size<sizeof *options || options->blur_radius<0 ||
       options->blur_radius>OL_MAX_BLUR_RADIUS || options->shadow_radius<0 ||
       options->shadow_radius>OL_MAX_BLUR_RADIUS || options->shadow_dx < -32768 ||
       options->shadow_dx>32768 || options->shadow_dy < -32768 || options->shadow_dy>32768 ||
       options->shadow_opacity<0 || options->shadow_opacity>255 ||
       (options->clip&&!rect_ok(*options->clip)))return fail(l,OL_ARGUMENT);
    unsigned long start=list_ok(l)?l->used:0;
    int r=ol_cmd_image(l,source,dst,options->opacity,options->bilinear);if(r)return r;
    struct command *c=(void *)((unsigned char *)l+start);
    c->op=CMD_LAYER;c->layer=*options;c->layer.clip=0;c->has_clip=options->clip!=0;
    if(options->clip)c->effect_clip=*options->clip;
    return OL_OK;
}
int ol_cmd_backdrop(struct ol_list *l,struct gfx_rect r,int radius,unsigned rgb,int opacity)
{
    if(!rect_ok(r)||radius<0||radius>OL_MAX_BLUR_RADIUS||opacity<0||opacity>255)
        return fail(l,OL_ARGUMENT);
    struct command *c=record(l,CMD_BACKDROP,0);if(!c)return record_error(l);
    c->rect=r;c->layer.blur_radius=radius;c->rgb=rgb;c->alpha=opacity;return OL_OK;
}
int ol_cmd_backdrop_rrect(struct ol_list *l,struct gfx_rect r,const int corner[4],int blur_radius,
                           int passes,unsigned rgb,int opacity)
{
    if(!corner||!rect_ok(r)||blur_radius<0||blur_radius>OL_MAX_BLUR_RADIUS||
       (passes!=1&&passes!=3)||opacity<0||opacity>255)return fail(l,OL_ARGUMENT);
    int cap=(r.w<r.h?r.w:r.h)/2,k[4];
    for(int i=0;i<4;i++) {
        if(corner[i]<0)return fail(l,OL_ARGUMENT);
        k[i]=corner[i]<cap?corner[i]:cap;
    }
    struct command *c=record(l,CMD_BACKDROP,0);if(!c)return record_error(l);
    c->rect=r;c->layer.blur_radius=blur_radius;c->rgb=rgb;c->alpha=opacity;c->passes=passes;
    for(int i=0;i<4;i++)c->corner[i]=k[i];
    return OL_OK;
}
