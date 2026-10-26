#include "ol_internal.h"

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
