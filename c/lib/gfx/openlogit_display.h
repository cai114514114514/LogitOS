#ifndef OPENLOGIT_DISPLAY_H
#define OPENLOGIT_DISPLAY_H
#include "openlogit.h"
#define OL_GLASS_CUT_TOP 1u
#define OL_GLASS_CUT_BOTTOM 2u
#define OL_GLASS_CUT_LEFT 4u
#define OL_GLASS_CUT_RIGHT 8u
/* Opaque native display pixels. Packed 32-bit pixels, width pixels per row;
 * channels are explicit bit positions, normally BGRX (16,8,0) or RGBX
 * (0,8,16). Use RGBA resources/ol_surface for straight-alpha offscreen work.
 * Source surfaces of a surface-to-surface blit share this channel layout.
 * Borrowed target writes are batched by the owner and presented once/frame. */
struct ol_pixel_target {
    uint32_t *px;int w,h;
    int clip_on,clx0,cly0,clx1,cly1;
};
struct ol_display {
    struct ol_pixel_target target;
    unsigned rpos,gpos,bpos;
    void *(*allocate)(unsigned long);
    void (*release)(void *);
    uint32_t *blur_scratch,*glass_buf;
    int *glass_line;
    int blur_scratch_n,glass_buf_n,glass_line_n,error;
    unsigned shadow_clamped;
    uint64_t batches;
};
/* Initialize storage to zero, set allocator callbacks if effects are used.
 * Bind starts a batch; callers can inspect error after drawing. Explicit
 * channel positions also cover valid boot framebuffer layouts without
 * forcing every pixel through an RGBA staging conversion. */
int ol_display_bind(struct ol_display *, const struct ol_pixel_target *, unsigned rpos,unsigned gpos,unsigned bpos);
void ol_display_destroy(struct ol_display *);
uint32_t ol_display_rgb(struct ol_display *,uint8_t r,uint8_t g,uint8_t b);
void ol_display_put(struct ol_display *ctx, int x, int y, uint32_t color);
void ol_display_blit_surface(struct ol_display *ctx, int dx, int dy, const struct ol_pixel_target *src);
void ol_display_blit_surface_scaled(struct ol_display *ctx, int dx, int dy, int dw, int dh, const struct ol_pixel_target *src);
void ol_display_blit_surface_scaled_bl(struct ol_display *ctx, int dx, int dy, int dw, int dh, const struct ol_pixel_target *src);
void ol_display_clear(struct ol_display *ctx, uint32_t color);
void ol_display_fill_rect(struct ol_display *ctx, int x, int y, int w, int h, uint32_t color);
void ol_display_fill_circle(struct ol_display *ctx, int cx, int cy, int r, uint32_t color);
void ol_display_round_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius, uint32_t color);
void ol_display_blit_glyph(struct ol_display *ctx, int x, int y, const uint8_t *cov, int w, int h, uint32_t color);
void ol_display_blend_rect(struct ol_display *ctx, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void ol_display_blend_round_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a);
unsigned ol_display_shadow_clamp_count(struct ol_display *ctx);
void ol_display_shadow(struct ol_display *ctx, int x, int y, int w, int h, int radius, int dy, int blur, uint8_t alpha);
uint32_t ol_display_shade(struct ol_display *ctx, uint32_t c, int delta);
void ol_display_fill_vgrad(struct ol_display *ctx, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void ol_display_round_rect_vgrad(struct ol_display *ctx, int x, int y, int w, int h, int radius, uint32_t top, uint32_t bottom);
void ol_display_blur_rect(struct ol_display *ctx, int x, int y, int w, int h, int radius, int corner);
void ol_display_liquid_glass(struct ol_display *ctx, int x, int y, int w, int h, int radius,
                     uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta);
void ol_display_liquid_glass_cut(struct ol_display *ctx, int x, int y, int w, int h, int radius,
                         uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta,
                         unsigned cut);
void ol_display_blit_rgba(struct ol_display *ctx, int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh);
/* Native 32-bit color with alpha in the high byte (RGB channels <= bit 16).
 * Used for cursor planes; RGBA resources retain their independent byte order. */
int ol_display_pack_argb(struct ol_display *, uint32_t *out, const unsigned char *rgba, unsigned count);
void ol_display_blit_argb(struct ol_display *, int x, int y, const uint32_t *, int w, int h, int stride);
void ol_display_blit_surface_alpha(struct ol_display *, int x, int y, int w, int h,
                                   const struct ol_pixel_target *, int alpha);
#endif
