#include "openlogit.h"
#include "openlogit_sw.h"

/* Software device runtime. The existing scan converter remains the only
 * implementation of coverage; each device owns its scratch storage. Commands
 * copy geometry because GUI builders routinely reuse stack buffers before
 * submit. Two caller-owned pixel buffers make a WHOLE list transactional:
 * the rasterizer's per-fill dry run cannot undo earlier successful commands.
 *
 * This is synchronous and in-process. Kernel handle validation, GPU queues,
 * memory import and display fences require a separate transport contract;
 * advertising those features here would make callers depend on absent work. */

#define OL_DEVICE_MAGIC 0x4f4c4431u
#define OL_SURFACE_MAGIC 0x4f4c5331u
#define OL_LIST_MAGIC 0x4f4c4c31u
#define OL_COMMAND_MAX (1024u * 1024u)
#define OL_FEATURES                                                                                \
    (OL_CAP_PATH_FILL | OL_CAP_RECT_CLIP | OL_CAP_GRADIENT | OL_CAP_IMAGE | OL_CAP_ATOMIC_FRAME | OL_CAP_PATH_CLIP | OL_CAP_STROKE | OL_CAP_TRANSFORM | OL_CAP_GLYPH_MASK | OL_CAP_DAMAGE | OL_CAP_LAYER | OL_CAP_BLUR | OL_CAP_BACKDROP)
struct ol_device {
    unsigned magic, objects;
    volatile int busy;
    uint64_t completed;
};
struct ol_surface {
    unsigned magic, refs;
    struct ol_device *device;
    struct ol_surface_desc desc;
    uint64_t generation;
};
#define OL_IMAGE_MAGIC 0x4f4c4931u
struct ol_image {
    unsigned magic, refs;
    struct ol_device *device;
    struct ol_image_desc desc;
    uint64_t generation;
};
struct ol_list {
    unsigned magic, closed;
    int error;
    struct ol_device *device;
    unsigned long capacity, used;
};
enum { CMD_CLEAR = 1, CMD_FILL, CMD_IMAGE, CMD_RESOURCE, CMD_GLYPH, CMD_LAYER, CMD_BACKDROP };
struct command {
    unsigned op, bytes;
    int alpha, rule, samples, has_clip;
    unsigned rgb;
    struct gfx_rect rect;
    struct gfx_path path;
    struct gfx_paint paint;
    struct ol_surface *image;
    uint64_t generation;
    struct ol_image *resource, *mask;
    uint64_t resource_generation, mask_generation;
    int mask_x, mask_y;
    struct ol_layer_options layer;
    struct gfx_rect effect_clip;
};
static unsigned long align8(unsigned long n)
{
    return (n + 7) & ~7ul;
}
static int aligned(const void *p)
{
    return p && !((unsigned long)p & 7);
}
static int device_ok(const struct ol_device *d)
{
    return d && d->magic == OL_DEVICE_MAGIC;
}
static int surface_ok(const struct ol_surface *s)
{
    return s && s->magic == OL_SURFACE_MAGIC && device_ok(s->device);
}
static int list_ok(const struct ol_list *l)
{
    return l && l->magic == OL_LIST_MAGIC && device_ok(l->device);
}
static void copy(void *dst, const void *src, unsigned long n)
{
    volatile unsigned char *d = dst;
    const unsigned char *s = src;
    for (unsigned long i = 0; i < n; i++)
        d[i] = s[i];
}
static int overlap(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long x = (unsigned long)a, y = (unsigned long)b;
    return x <= y ? y - x < an : x - y < bn;
}
static int lock(struct ol_device *d)
{
    return !__sync_lock_test_and_set(&d->busy, 1);
}
static void unlock(struct ol_device *d)
{
    __sync_lock_release(&d->busy);
}
static unsigned long image_bytes(unsigned w, unsigned h, unsigned stride)
{
    return (unsigned long)(h - 1) * stride + (unsigned long)w * 4;
}
static int buffer_ok(const void *p, unsigned long bytes, unsigned w, unsigned h, unsigned stride)
{
    return p && w && h && w <= GFX_MAX_W && h <= GFX_MAX_W && stride >= w * 4 &&
           stride <= 0x7fffffffu && bytes >= image_bytes(w, h, stride) &&
           (unsigned long)p <= ~0ul - bytes;
}
static void copy_image(unsigned char *dst, unsigned ds, const unsigned char *src, unsigned ss,
                       unsigned w, unsigned h)
{
    for (unsigned y = 0; y < h; y++)
        copy(dst + (unsigned long)y * ds, src + (unsigned long)y * ss, (unsigned long)w * 4);
}

unsigned long ol_device_size(void)
{
    return align8(sizeof(struct ol_device)) + gfx_raster_workspace_size();
}
int ol_device_create(void *storage, unsigned long bytes, uint32_t version, uint32_t required,
                     struct ol_device **out)
{
    if (!out)
        return OL_ARGUMENT;
    *out = 0;
    if (version != OL_VERSION(1,0) && version != OL_API_VERSION)
        return OL_VERSION_UNSUPPORTED;
    if (required & ~OL_FEATURES)
        return OL_UNSUPPORTED;
    if (!aligned(storage) || bytes < ol_device_size())
        return OL_ARGUMENT;
    struct ol_device *d = storage;
    gfx_zero(d, sizeof *d);
    d->magic = OL_DEVICE_MAGIC;
    *out = d;
    return OL_OK;
}
int ol_device_caps(const struct ol_device *d, struct ol_caps *out)
{
    if (!device_ok(d) || !out || out->size < sizeof *out)
        return OL_ARGUMENT;
    *out = (struct ol_caps){
        .size = sizeof *out,
        .api_version = OL_API_VERSION,
        .implementation_revision = 3,
        .backend = OL_BACKEND_SOFTWARE,
        .features = OL_FEATURES,
        .max_width = GFX_MAX_W,
        .max_height = GFX_MAX_W,
        .max_path_points = GFX_MAX_EDGES,
        .max_command_bytes = OL_COMMAND_MAX,
        .max_active_edges = GFX_MAX_ACTIVE,
        .max_samples = GFX_MAX_SUBS,
    };
    return OL_OK;
}
uint64_t ol_device_completed(const struct ol_device *d)
{
    return device_ok(d) ? d->completed : 0;
}
int ol_device_destroy(struct ol_device *d)
{
    if (!device_ok(d))
        return OL_ARGUMENT;
    if (d->objects || d->busy)
        return OL_BUSY;
    d->magic = 0;
    return OL_OK;
}
unsigned long ol_surface_size(void)
{
    return sizeof(struct ol_surface);
}
int ol_surface_create(struct ol_device *d, void *storage, unsigned long bytes,
                      const struct ol_surface_desc *a, struct ol_surface **out)
{
    if (!out)
        return OL_ARGUMENT;
    *out = 0;
    if (!device_ok(d) || !aligned(storage) || bytes < sizeof(struct ol_surface) || !a ||
        a->size < sizeof *a)
        return OL_ARGUMENT;
    if (a->format != OL_FORMAT_RGBA8_STRAIGHT)
        return OL_UNSUPPORTED;
    if (!buffer_ok(a->front, a->front_bytes, a->width, a->height, a->stride) ||
        !buffer_ok(a->work, a->work_bytes, a->width, a->height, a->stride) ||
        overlap(a->front, a->front_bytes, a->work, a->work_bytes))
        return OL_ARGUMENT;
    struct ol_surface *s = storage;
    gfx_zero(s, sizeof *s);
    s->magic = OL_SURFACE_MAGIC;
    s->device = d;
    s->desc = *a;
    s->generation = 1;
    d->objects++;
    *out = s;
    return OL_OK;
}
int ol_surface_view(const struct ol_surface *s, struct ol_surface_view *out)
{
    if (!surface_ok(s) || !out)
        return OL_ARGUMENT;
    *out = (struct ol_surface_view){s->desc.front,  s->desc.width,  s->desc.height,
                                    s->desc.stride, s->desc.format, s->generation};
    return OL_OK;
}
int ol_surface_upload(struct ol_surface *s, const unsigned char *p, unsigned long bytes,
                      unsigned stride)
{
    if (!surface_ok(s) || !buffer_ok(p, bytes, s->desc.width, s->desc.height, stride))
        return OL_ARGUMENT;
    if (overlap(p, bytes, s->desc.front, s->desc.front_bytes) ||
        overlap(p, bytes, s->desc.work, s->desc.work_bytes))
        return OL_ARGUMENT;
    if (!lock(s->device))
        return OL_BUSY;
    copy_image(s->desc.front, s->desc.stride, p, stride, s->desc.width, s->desc.height);
    s->generation++;
    unlock(s->device);
    return OL_OK;
}
int ol_surface_destroy(struct ol_surface *s)
{
    if (!surface_ok(s))
        return OL_ARGUMENT;
    if (s->refs || s->device->busy)
        return OL_BUSY;
    s->device->objects--;
    s->magic = 0;
    return OL_OK;
}
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
static int fail(struct ol_list *l, int error)
{
    if (list_ok(l) && !l->error)
        l->error = error;
    return error;
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
static int record_error(struct ol_list *l)
{
    return list_ok(l) ? l->error : OL_ARGUMENT;
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
static int rect_ok(struct gfx_rect r)
{
    return r.w >= 0 && r.h >= 0 && r.x >= -32768 && r.y >= -32768 && r.w <= 32768 && r.h <= 32768 &&
           r.x <= 32768 - r.w && r.y <= 32768 - r.h;
}
static int path_ok(const struct gfx_path *p)
{
    if (!p || p->overflow || p->npt < 0 || p->nsub < 0 || p->npt > p->ptcap ||
        p->nsub > p->subcap || p->npt > GFX_MAX_EDGES || p->nsub > p->npt)
        return 0;
    if (!p->npt)
        return p->nsub == 0;
    if (!p->pt || !p->sub || !p->nsub || p->sub[0] != 0)
        return 0;
    for (int i = 0; i < p->nsub; i++)
        if (p->sub[i] < 0 || p->sub[i] >= p->npt || (i && p->sub[i] <= p->sub[i - 1]))
            return 0;
    for (int i = 0; i < 2 * p->npt; i++)
        if (p->pt[i] < -GFX_PX(32768) || p->pt[i] > GFX_PX(32768))
            return 0;
    return 1;
}
static int paint_ok(const struct gfx_paint *p)
{
    if (!p || p->kind < GFX_SOLID || p->kind > GFX_RADIAL || p->alpha < 0 || p->alpha > 255 ||
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
    c->paint = *paint;
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
static int render(struct ol_device *d, struct gfx_surface *dst, const struct gfx_path *p, int rule,
                  const struct gfx_paint *paint, const struct gfx_rect *clip, int samples)
{
    void *workspace = (unsigned char *)d + align8(sizeof *d);
    return ol_sw_raster_fill(workspace, dst, p, rule, paint, clip, samples, 0);
}
static int execute(struct ol_device *d, const struct command *c, struct gfx_surface *target, void *scratch)
{
    if(c->op==CMD_LAYER) {
        const struct ol_surface_desc *a=&c->image->desc;
        struct gfx_surface source={a->front,a->width,a->height,a->stride};
        struct ol_layer_options options=c->layer;
        options.clip=c->has_clip?&c->effect_clip:0;
        ol_sw_layer(target,&source,c->rect,&options,scratch);return 1;
    }
    if(c->op==CMD_BACKDROP) {
        ol_sw_backdrop(target,c->rect,c->layer.blur_radius,c->rgb,c->alpha,scratch);return 1;
    }
    if (c->op == CMD_CLEAR) {
        for (int y = 0; y < target->h; y++) {
            unsigned char *row = target->px + (unsigned long)y * target->stride;
            for (int x = 0; x < target->w; x++) {
                row[4 * x] = GFX_R(c->rgb);
                row[4 * x + 1] = GFX_G(c->rgb);
                row[4 * x + 2] = GFX_B(c->rgb);
                row[4 * x + 3] = c->alpha;
            }
        }
        return 1;
    }
    if (c->op == CMD_FILL) {
        if (!c->mask) return render(d,target,&c->path,c->rule,&c->paint,
                                   c->has_clip ? &c->rect : 0,c->samples);
        const struct ol_image_desc *a = &c->mask->desc;
        struct gfx_clip_mask mask = {a->pixels,a->width,a->height,c->mask_x,c->mask_y};
        return ol_sw_raster_fill((unsigned char *)d+align8(sizeof *d), target,&c->path,
                                c->rule,&c->paint,c->has_clip?&c->rect:0,c->samples,&mask);
    }
    if (c->op == CMD_GLYPH) {
        const struct ol_image_desc *a = &c->resource->desc;
        for (int y=0;y<(int)a->height;y++) {
            int dy=c->rect.y+y;
            if(dy<0 || dy>=target->h) continue;
            for(int x=0;x<(int)a->width;x++) {
                int dx=c->rect.x+x;
                if(dx<0 || dx>=target->w) continue;
                gfx_over(target->px+(unsigned long)dy*target->stride+dx*4,
                         GFX_R(c->rgb), GFX_G(c->rgb), GFX_B(c->rgb), c->alpha,
                         a->pixels[(unsigned long)y*a->stride+x]);
            }
        }
        return 1;
    }
    unsigned aw,ah,astride; const unsigned char *apixels;
    if(c->op==CMD_RESOURCE) {
        aw=c->resource->desc.width; ah=c->resource->desc.height;
        astride=c->resource->desc.stride; apixels=c->resource->desc.pixels;
    } else {
        aw=c->image->desc.width; ah=c->image->desc.height;
        astride=c->image->desc.stride; apixels=c->image->desc.front;
    }
    struct gfx_matrix m;
    gfx_m_identity(&m);
    m.a = (int)((long long)c->rect.w * GFX_MONE / aw);
    m.d = (int)((long long)c->rect.h * GFX_MONE / ah);
    m.e = GFX_PX(c->rect.x);
    m.f = GFX_PX(c->rect.y);
    struct gfx_paint paint;
    if (!gfx_paint_image(&paint, apixels, aw, ah, astride, &m, c->samples))
        return 0;
    paint.global_alpha = c->alpha;
    int pts[16], subs[2];
    struct gfx_path p;
    gfx_path_init(&p, pts, 8, subs, 2);
    gfx_path_rect(&p, GFX_PX(c->rect.x), GFX_PX(c->rect.y), GFX_PX(c->rect.w), GFX_PX(c->rect.h));
    return render(d, target, &p, GFX_NONZERO, &paint, 0, GFX_SUBS);
}
static int submit(struct ol_device *d, const struct ol_list *l, struct ol_surface *s, uint64_t *serial,
                  struct ol_submit_info *info,void *scratch,unsigned long scratch_bytes)
{
    if (serial)
        *serial = 0;
    if (!device_ok(d) || !list_ok(l) || !surface_ok(s) || l->device != d || s->device != d)
        return OL_ARGUMENT;
    if (l->error)
        return l->error;
    if (!l->closed)
        return OL_STATE;
    unsigned long required=0;
    int requirement=ol_submit_workspace_size(l,s,&required);
    if(requirement)return requirement;
    if(required && (!scratch || scratch_bytes<required))return OL_LIMIT;
    if(required && (!aligned(scratch) || (unsigned long)scratch>~0ul-scratch_bytes ||
       overlap(scratch,scratch_bytes,s->desc.front,s->desc.front_bytes) ||
       overlap(scratch,scratch_bytes,s->desc.work,s->desc.work_bytes) ||
       overlap(scratch,scratch_bytes,l,l->capacity) || overlap(scratch,scratch_bytes,d,ol_device_size()) ||
       overlap(scratch,scratch_bytes,s,ol_surface_size())))return OL_ARGUMENT;
    if (!lock(d))
        return OL_BUSY;
    int result = OL_OK;
    for (unsigned long o = align8(sizeof *l); o < l->used;) {
        const struct command *c = (const void *)((const unsigned char *)l + o);
        const struct ol_image *sources[2]={c->resource,c->mask};
        uint64_t versions[2]={c->resource_generation,c->mask_generation};
        for(int i=0;i<2;i++) if(sources[i]) {
            const struct ol_image *im=sources[i];
            if(im->magic!=OL_IMAGE_MAGIC || im->generation!=versions[i]) {result=OL_STALE_RESOURCE;break;}
            if(overlap(im->desc.pixels,im->desc.bytes,s->desc.front,s->desc.front_bytes) ||
               overlap(im->desc.pixels,im->desc.bytes,s->desc.work,s->desc.work_bytes)) {result=OL_ARGUMENT;break;}
            if(required && (overlap(scratch,scratch_bytes,im,ol_image_size()) ||
               overlap(scratch,scratch_bytes,im->desc.pixels,im->desc.bytes))) {result=OL_ARGUMENT;break;}
        }
        if(result)break;
        if (c->image) {
            const struct ol_surface *image = c->image;
            if (!surface_ok(image) || image->generation != c->generation) {
                result = OL_STALE_RESOURCE;
                break;
            }
            if (image == s ||
                overlap(image->desc.front, image->desc.front_bytes, s->desc.front,
                        s->desc.front_bytes) ||
                overlap(image->desc.front, image->desc.front_bytes, s->desc.work,
                        s->desc.work_bytes)) {
                result = OL_ARGUMENT;
                break;
            }
            if(required && (overlap(scratch,scratch_bytes,image,ol_surface_size()) ||
               overlap(scratch,scratch_bytes,image->desc.front,image->desc.front_bytes) ||
               overlap(scratch,scratch_bytes,image->desc.work,image->desc.work_bytes))) {
                result=OL_ARGUMENT;break;
            }
        }
        o += c->bytes;
    }
    if (result) {
        unlock(d);
        return result;
    }
    struct gfx_surface target = {s->desc.work, s->desc.width, s->desc.height, s->desc.stride};
    struct gfx_rect damage={0,0,0,0},reads={0,0,0,0}; uint64_t count=0;
    for(unsigned long o=align8(sizeof *l);o<l->used;) {
        const struct command *c=(const void *)((const unsigned char *)l+o);
        struct gfx_rect r=c->rect;
        if(c->op==CMD_CLEAR) r=(struct gfx_rect){0,0,target.w,target.h};
        if(c->op==CMD_FILL) {
            int x0,y0,x1,y1;
            if(!gfx_path_bounds(&c->path,&x0,&y0,&x1,&y1)) r=(struct gfx_rect){0,0,0,0};
            else r=(struct gfx_rect){x0,y0,x1-x0,y1-y0};
        }
        if(c->op==CMD_LAYER) {
            r=ol_sw_effect_bounds(c->rect,c->layer.blur_radius,target.w,target.h);
            if(c->layer.shadow_opacity) {
                struct gfx_rect shadow=c->rect;shadow.x+=c->layer.shadow_dx;shadow.y+=c->layer.shadow_dy;
                shadow=ol_sw_effect_bounds(shadow,c->layer.shadow_radius,target.w,target.h);
                r=ol_damage_move(r,shadow,0,target.w,target.h);
            }
            if(c->has_clip) {
                int right=r.x+r.w,bottom=r.y+r.h;
                if(r.x<c->effect_clip.x)r.x=c->effect_clip.x;
                if(r.y<c->effect_clip.y)r.y=c->effect_clip.y;
                if(right>c->effect_clip.x+c->effect_clip.w)right=c->effect_clip.x+c->effect_clip.w;
                if(bottom>c->effect_clip.y+c->effect_clip.h)bottom=c->effect_clip.y+c->effect_clip.h;
                r.w=right>r.x?right-r.x:0;r.h=bottom>r.y?bottom-r.y:0;
            }
        }
        if(c->op==CMD_BACKDROP)reads=ol_damage_move(reads,
            ol_sw_effect_bounds(r,c->layer.blur_radius,target.w,target.h),0,target.w,target.h);
        damage=ol_damage_move(damage,r,0,target.w,target.h);
        count++; o+=c->bytes;
    }
    /* Backdrop dependencies can lie outside output damage. Initialize their
     * halo from front too; uninitialized work pixels otherwise leak into blur
     * at the edge, even though the committed rectangle itself is correct. */
    struct gfx_rect initial=ol_damage_move(damage,reads,0,target.w,target.h);
    for(int y=initial.y;y<initial.y+initial.h;y++)
        copy(target.px+(unsigned long)y*target.stride+initial.x*4,
             s->desc.front+(unsigned long)y*s->desc.stride+initial.x*4,initial.w*4);

    for (unsigned long o = align8(sizeof *l); o < l->used;) {
        const struct command *c = (const void *)((const unsigned char *)l + o);
        if (!execute(d, c, &target, scratch)) {
            result = OL_RENDER_FAILED;
            break;
        }
        o += c->bytes;
    }
#ifdef OPENLOGIT_NO_COMMIT
    result = OL_RENDER_FAILED; /* Private control: ordinary rendering must fail. */
#endif
    if (!result) {
        for(int y=damage.y;y<damage.y+damage.h;y++)
            copy(s->desc.front+(unsigned long)y*s->desc.stride+damage.x*4,
                 target.px+(unsigned long)y*target.stride+damage.x*4,damage.w*4);
        if(info){info->damage=damage; info->copied_bytes=((uint64_t)damage.w*damage.h+(uint64_t)initial.w*initial.h)*4;
                 info->commands=count; info->completion_serial=d->completed+1;}
        s->generation++;
        d->completed++;
        if (serial)
            *serial = d->completed;
    }
    unlock(d);
    return result;
}
const char *ol_status_string(int s)
{
    switch (s) {
    case OL_OK:
        return "ok";
    case OL_ARGUMENT:
        return "invalid argument";
    case OL_VERSION_UNSUPPORTED:
        return "API version unsupported";
    case OL_UNSUPPORTED:
        return "capability unsupported";
    case OL_LIMIT:
        return "storage limit";
    case OL_STATE:
        return "invalid state";
    case OL_BUSY:
        return "resource busy";
    case OL_STALE_RESOURCE:
        return "recorded resource changed";
    case OL_RENDER_FAILED:
        return "render failed; frame not committed";
    case OL_BACKEND_FAILED:
        return "presentation backend failed";
    default:
        return "unknown status";
    }
}

static int image_ok(const struct ol_image *im)
{ return im && im->magic==OL_IMAGE_MAGIC && device_ok(im->device); }
unsigned long ol_image_size(void) {return sizeof(struct ol_image);}
int ol_image_create(struct ol_device *d, void *storage, unsigned long bytes,
                     const struct ol_image_desc *a, struct ol_image **out)
{
    if(!out)return OL_ARGUMENT;
    *out=0;
    if(!device_ok(d)||!aligned(storage)||bytes<sizeof(struct ol_image)||!a||a->size<sizeof *a)
        return OL_ARGUMENT;
    if(a->format!=OL_FORMAT_RGBA8_STRAIGHT && a->format!=OL_FORMAT_A8)return OL_UNSUPPORTED;
    unsigned bpp=a->format==OL_FORMAT_A8?1:4;
    if(!a->pixels || !a->width || !a->height || a->width>GFX_MAX_W || a->height>GFX_MAX_W ||
       a->stride<a->width*bpp || a->stride>0x7fffffffu ||
       a->bytes<(unsigned long)(a->height-1)*a->stride+a->width*bpp ||
       (unsigned long)a->pixels>~0ul-a->bytes ||
       (bpp==1 && a->stride!=a->width))return OL_ARGUMENT;
    struct ol_image *im=storage;
    *im=(struct ol_image){OL_IMAGE_MAGIC,0,d,*a,1};d->objects++;*out=im;return OL_OK;
}
int ol_image_view(const struct ol_image *im, struct ol_surface_view *out)
{
    if(!image_ok(im)||!out)return OL_ARGUMENT;
    *out=(struct ol_surface_view){im->desc.pixels,im->desc.width,im->desc.height,
        im->desc.stride,im->desc.format,im->generation};return OL_OK;
}
int ol_image_update(struct ol_image *im, struct gfx_rect r, const unsigned char *p,
                     unsigned long bytes, unsigned stride)
{
    if(!image_ok(im)||!p||!rect_ok(r)||r.x<0||r.y<0||r.w<=0||r.h<=0||
       (unsigned)r.x+r.w>im->desc.width||(unsigned)r.y+r.h>im->desc.height)return OL_ARGUMENT;
    unsigned bpp=im->desc.format==OL_FORMAT_A8?1:4;
    if(stride<(unsigned)r.w*bpp || stride>0x7fffffffu ||
       bytes<(unsigned long)(r.h-1)*stride+r.w*bpp ||
       overlap(p,bytes,im->desc.pixels,im->desc.bytes))return OL_ARGUMENT;
    if(!lock(im->device))return OL_BUSY;
    for(int y=0;y<r.h;y++)copy(im->desc.pixels+(unsigned long)(y+r.y)*im->desc.stride+r.x*bpp,
                              p+(unsigned long)y*stride,r.w*bpp);
    im->generation++;unlock(im->device);return OL_OK;
}
int ol_image_destroy(struct ol_image *im)
{
    if(!image_ok(im))return OL_ARGUMENT;
    if(im->refs||im->device->busy)return OL_BUSY;
    im->device->objects--;im->magic=0;return OL_OK;
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
struct gfx_rect ol_damage_move(struct gfx_rect a,struct gfx_rect b,int extent,int w,int h)
{
    long long x0=w,y0=h,x1=0,y1=0;
    if(extent<0)extent=0;
    struct gfx_rect regions[2]={a,b};
    for(int i=0;i<2;i++) {
        struct gfx_rect r=regions[i];if(r.w<=0||r.h<=0)continue;
        long long l=(long long)r.x-extent,t=(long long)r.y-extent;
        long long rr=(long long)r.x+r.w+extent,bb=(long long)r.y+r.h+extent;
        if(l<x0)x0=l;if(t<y0)y0=t;if(rr>x1)x1=rr;if(bb>y1)y1=bb;
    }
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>w)x1=w;if(y1>h)y1=h;
    if(x1<=x0||y1<=y0)return (struct gfx_rect){0,0,0,0};
    return (struct gfx_rect){x0,y0,x1-x0,y1-y0};
}
int ol_submit(struct ol_device *d,const struct ol_list *l,struct ol_surface *s,uint64_t *serial)
{return submit(d,l,s,serial,0,0,0);}
int ol_submit_damage(struct ol_device *d,const struct ol_list *l,struct ol_surface *s,struct ol_submit_info *info)
{
    if(!info||info->size<sizeof *info)return OL_ARGUMENT;
    *info=(struct ol_submit_info){.size=sizeof *info};return submit(d,l,s,0,info,0,0);
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
int ol_submit_workspace_size(const struct ol_list *l,const struct ol_surface *s,unsigned long *out)
{
    if(!out)return OL_ARGUMENT;*out=0;
    if(!list_ok(l)||!surface_ok(s)||l->device!=s->device)return OL_ARGUMENT;
    if(l->error)return l->error;if(!l->closed)return OL_STATE;
    for(unsigned long o=align8(sizeof *l);o<l->used;) {
        const struct command *c=(const void *)((const unsigned char *)l+o);
        unsigned long need=0;
        if(c->op==CMD_LAYER) {
            int radius=c->layer.blur_radius;
            if(c->layer.shadow_opacity&&c->layer.shadow_radius>radius)radius=c->layer.shadow_radius;
            if(radius)need=(s->desc.width+2*radius)*4*sizeof(unsigned);
        } else if(c->op==CMD_BACKDROP)
            need=ol_sw_backdrop_size(c->rect,c->layer.blur_radius,s->desc.width,s->desc.height);
        if(need>*out)*out=need;o+=c->bytes;
    }
    return OL_OK;
}
int ol_submit_workspace(struct ol_device *d,const struct ol_list *l,struct ol_surface *s,
                         void *scratch,unsigned long bytes,struct ol_submit_info *info)
{
    if(info) {if(info->size<sizeof *info)return OL_ARGUMENT;*info=(struct ol_submit_info){.size=sizeof *info};}
    return submit(d,l,s,0,info,scratch,bytes);
}
