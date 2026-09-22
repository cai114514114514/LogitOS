/* Private pixel access shared by display primitives and backdrop materials.
 * Keep channel packing and allocation failure reporting identical in both. */
#ifndef OPENLOGIT_DISPLAY_PIXELS_H
#define OPENLOGIT_DISPLAY_PIXELS_H
#include "openlogit_display.h"

/* Moving the material to its own file otherwise turns every blur/store pack
 * into an external call (seven call sites in the x86 object). Keep the pixel
 * operation inline; the public entry point delegates to this same mapping. */
static inline uint32_t display_rgb(struct ol_display *ctx,
                                   uint8_t r, uint8_t g, uint8_t b)
{
    return (uint32_t)r << ctx->rpos | (uint32_t)g << ctx->gpos |
           (uint32_t)b << ctx->bpos;
}

static inline void *display_alloc(struct ol_display *ctx, unsigned long bytes)
{
    void *p = ctx->allocate ? ctx->allocate(bytes) : 0;
    if (!p) ctx->error = OL_LIMIT;
    return p;
}

static inline void display_free(struct ol_display *ctx, void *p)
{
    if (ctx->release) ctx->release(p);
}

static inline void unpack(struct ol_display *ctx, uint32_t pixel,
                          int *r, int *g, int *b)
{
    *r = (pixel >> ctx->rpos) & 255;
    *g = (pixel >> ctx->gpos) & 255;
    *b = (pixel >> ctx->bpos) & 255;
}
#endif
