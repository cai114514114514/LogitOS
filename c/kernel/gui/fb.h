#ifndef LOGIT_FB_H
#define LOGIT_FB_H

#include <stdint.h>

/* Find GRUB's framebuffer tag, map the framebuffer, and prepare drawing.
 * Returns 0 if no usable 32-bpp RGB framebuffer is available. */
int fb_init(uint64_t mb_info_addr);

uint32_t fb_width(void);
uint32_t fb_height(void);

/* ---- UI scale: points vs device pixels ---------------------------------
 *
 * The desktop is authored in POINTS -- a resolution-independent unit -- and the
 * compositor rasterizes at the display's device resolution. A point is one
 * pixel at scale 100; at scale 200 it is a 2x2 block of pixels, and text, vector
 * icons and rounded corners are RE-RASTERIZED at the larger size rather than
 * magnified, so they get sharper instead of blockier. This is the only reason
 * the whole thing is possible without redrawing any art: every primitive under
 * this header already takes a size (text_draw_run's px, icon_draw's px,
 * fb_blit_rgba's dest rect), so handing it a bigger number is all that "retina"
 * means here.
 *
 * fb_pt() FLOORS. Convert a rectangle as (fb_pt(x), fb_pt(x+w) - fb_pt(x)),
 * never as (fb_pt(x), fb_pt(w)): two abutting logical rects must stay abutting
 * at every scale, and rounding each width independently leaves seams.
 *
 * The scale is chosen from the mode in fb_init() -- see pick_scale() there --
 * so raising the resolution raises the density instead of shrinking the UI. */
void fb_set_scale(int pct);
int  fb_scale(void);        /* percent; 100 = 1x */
int  fb_pt(int points);     /* points -> device pixels (floor, negatives too) */
int  fb_dev2pt(int px);     /* device pixels -> points (the inverse, for input) */
int  fb_ui_px(void);        /* the default UI text size, in device pixels */

/* The logical desktop size, in points: what an app should size itself against. */
int  fb_width_pt(void);
int  fb_height_pt(void);

#define LOGIT_FONT_W 8
#define LOGIT_FONT_H 16

/* Pack 8-bit channels into the framebuffer's native pixel format. */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Double buffering: route drawing to `buf` (a width*height*4 RAM buffer),
 * then blit to the visible framebuffer with fb_present(). */
void fb_set_backbuffer(uint32_t *buf);
void fb_present(void);
void fb_present_rect(int x, int y, int w, int h);   /* push one rect back->framebuffer */
void fb_copy_rect(int x, int y, int w, int h);      /* raw band copy (parallel workers) */
void fb_set_present_par(void (*fn)(int, int, int, int));  /* register SMP parallel present */
void fb_fb_put(int x, int y, uint32_t color);       /* write straight to the framebuffer */
void fb_flush_rect(int x, int y, int w, int h);     /* display a rect drawn straight into fb_mem */

/* ---- the pointer, as a display plane --------------------------------------
 * A cursor composited into the scanout makes the highest-frequency event in the
 * system (mouse motion) cost a whole frame: every pixel under the old arrow has
 * to be restored and every pixel under the new one overwritten, and with a
 * single-buffer compositor that means recompositing the screen. A display that
 * owns a cursor plane makes it cost nothing instead.
 *
 * fb_cursor_hw() is the question every caller must ask before relying on the
 * other two: when it is 0 (multiboot LFB, or a virtio-gpu without a cursor
 * queue) the pointer has to be drawn into the frame like any other pixel.
 * `argb` is 0xAARRGGBB, and (hot_x,hot_y) is the pixel inside it that sits on
 * the reported pointer position. */
int  fb_cursor_hw(void);
int  fb_cursor_image(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void fb_cursor_move(int x, int y);

/* An off-screen drawing target (e.g. an application window's canvas). The clip
 * rectangle is a PROPERTY OF THE TARGET, not a global: app A setting a clip on
 * its own surface must never affect a draw into app B's surface. The old global
 * clip leaked across apps -- between an app's gui_clip(set) and gui_clip(clear)
 * (two separate syscalls) the scheduler could switch to another app, whose
 * gui_clear then inherited the stale rect and left most of its window at the
 * white window-create fill -> the "white Terminal" bug. clip_on == 0 = unclipped. */
struct surface {
    uint32_t *px;
    int w, h;
    int clip_on, clx0, cly0, clx1, cly1;   /* per-target clip, half-open */
};

/* Route subsequent drawing to `s`, or NULL for the screen back buffer. */
void fb_target(struct surface *s);

/* Restrict drawing to a rectangle on the CURRENT TARGET (half-open); the clip
 * is stored on that surface, so it does not bleed onto other draw targets. */
void fb_set_clip(int x, int y, int w, int h);
void fb_clear_clip(void);

/* Copy `src` into the current target at (dx,dy), clipped. */
void fb_blit_surface(int dx, int dy, const struct surface *src);

/* Nearest-neighbour scaled, opaque blit of `src` into dest rect (dx,dy,dw,dh). */
void fb_blit_surface_scaled(int dx, int dy, int dw, int dh, const struct surface *src);

/* Text (8x16 bitmap font). */
void fb_char(int x, int y, char c, uint32_t color);
void fb_text(int x, int y, const char *s, uint32_t color);
int  fb_text_width(const char *s);

void fb_clear(uint32_t color);
void fb_put(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_fill_circle(int cx, int cy, int r, uint32_t color);
void fb_round_rect(int x, int y, int w, int h, int radius, uint32_t color);

/* Blit an 8-bit coverage bitmap (w*h) as anti-aliased text in `color`. */
void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color);

/* Blit a straight-RGBA image (sw*sh) into dest rect (dx,dy,dw,dh), scaled
 * (nearest) with per-pixel alpha. */
void fb_blit_rgba(int dx, int dy, int dw, int dh, const uint8_t *rgba, int sw, int sh);

/* Alpha-blended (0 = transparent, 255 = opaque) — for frosted-glass surfaces. */
void fb_blend_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void fb_blend_round_rect(int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Real-time separable box blur of a rect on the current target (frost the live
 * backdrop behind a translucent panel). O(w*h), radius-independent. corner > 0
 * rounds the written region so a rounded panel leaves no blurred corner nubs. */
void fb_blur_rect(int x, int y, int w, int h, int radius, int corner);

/* "Liquid Glass" material: frost + rim refraction + specular highlight + tint of a
 * rounded-rect panel over the live backdrop on the current target. tint = body
 * colour (white for light glass, dark for dark glass), ta = its 0..255 strength. */
void fb_liquid_glass(int x, int y, int w, int h, int radius,
                     uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta);

/* Vertical gradient fills (top color at the first row -> bottom color). */
void fb_fill_vgrad(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void fb_round_rect_vgrad(int x, int y, int w, int h, int radius, uint32_t top, uint32_t bottom);

/* Lighten (delta>0) / darken (delta<0) a packed color by delta per channel. */
uint32_t fb_shade(uint32_t c, int delta);

#endif /* LOGIT_FB_H */
