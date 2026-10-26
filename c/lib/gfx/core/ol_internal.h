#ifndef OL_INTERNAL_H
#define OL_INTERNAL_H
/* Shared definitions for the software OpenLogit device runtime split across
 * ol_device.c, ol_record.c, ol_exec.c and ol_image.c. Helpers are stateless. */
#include "openlogit.h"
#include "openlogit_canvas.h"
#include "openlogit_sw.h"

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
enum { CMD_CLEAR = 1, CMD_FILL, CMD_IMAGE, CMD_RESOURCE, CMD_GLYPH, CMD_LAYER, CMD_BACKDROP,
       CMD_CLEAR_RECT };
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
static inline unsigned long align8(unsigned long n)
{
    return (n + 7) & ~7ul;
}
static inline int aligned(const void *p)
{
    return p && !((unsigned long)p & 7);
}
static inline int device_ok(const struct ol_device *d)
{
    return d && d->magic == OL_DEVICE_MAGIC;
}
static inline int surface_ok(const struct ol_surface *s)
{
    return s && s->magic == OL_SURFACE_MAGIC && device_ok(s->device);
}
static inline int list_ok(const struct ol_list *l)
{
    return l && l->magic == OL_LIST_MAGIC && device_ok(l->device);
}
static inline void copy(void *dst, const void *src, unsigned long n)
{
    /* Volatile stores keep freestanding builds independent of libc memcpy.
     * Pixel rows are usually word aligned; retain a byte path for arbitrary
     * image strides and descriptor tails. may_alias permits object-byte copies. */
    typedef unsigned int alias_word __attribute__((__may_alias__));
    volatile unsigned char *d = dst;
    const unsigned char *s = src;
    if (!(((unsigned long)d | (unsigned long)s) & 3)) {
        while (n >= 4) {
            *(volatile alias_word *)d = *(const alias_word *)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while (n--) *d++ = *s++;
}
static inline int overlap(const void *a, unsigned long an, const void *b, unsigned long bn)
{
    unsigned long x = (unsigned long)a, y = (unsigned long)b;
    return x <= y ? y - x < an : x - y < bn;
}
static inline int lock(struct ol_device *d)
{
    return !__sync_lock_test_and_set(&d->busy, 1);
}
static inline void unlock(struct ol_device *d)
{
    __sync_lock_release(&d->busy);
}
static inline unsigned long image_bytes(unsigned w, unsigned h, unsigned stride)
{
    return (unsigned long)(h - 1) * stride + (unsigned long)w * 4;
}
static inline int buffer_ok(const void *p, unsigned long bytes, unsigned w, unsigned h, unsigned stride)
{
    return p && w && h && w <= GFX_MAX_W && h <= GFX_MAX_W && stride >= w * 4 &&
           stride <= 0x7fffffffu && bytes >= image_bytes(w, h, stride) &&
           (unsigned long)p <= ~0ul - bytes;
}
static inline void copy_image(unsigned char *dst, unsigned ds, const unsigned char *src, unsigned ss,
                       unsigned w, unsigned h)
{
    for (unsigned y = 0; y < h; y++)
        copy(dst + (unsigned long)y * ds, src + (unsigned long)y * ss, (unsigned long)w * 4);
}
static inline int fail(struct ol_list *l, int error)
{
    if (list_ok(l) && !l->error)
        l->error = error;
    return error;
}
static inline int record_error(struct ol_list *l)
{
    return list_ok(l) ? l->error : OL_ARGUMENT;
}
static inline int image_ok(const struct ol_image *im)
{ return im && im->magic==OL_IMAGE_MAGIC && device_ok(im->device); }
static inline int rect_ok(struct gfx_rect r)
{
    return r.w >= 0 && r.h >= 0 && r.x >= -32768 && r.y >= -32768 && r.w <= 32768 && r.h <= 32768 &&
           r.x <= 32768 - r.w && r.y <= 32768 - r.h;
}
#endif
