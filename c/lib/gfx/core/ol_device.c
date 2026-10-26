#include "ol_internal.h"

/* Software device runtime. The existing scan converter remains the only
 * implementation of coverage; each device owns its scratch storage. Commands
 * copy geometry because GUI builders routinely reuse stack buffers before
 * submit. Two caller-owned pixel buffers make a WHOLE list transactional:
 * the rasterizer's per-fill dry run cannot undo earlier successful commands.
 *
 * This is synchronous and in-process. Kernel handle validation, GPU queues,
 * memory import and display fences require a separate transport contract;
 * advertising those features here would make callers depend on absent work. */

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
