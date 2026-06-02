#ifndef AQUA_TEXT_H
#define AQUA_TEXT_H

#include <stdint.h>
#include "ttf.h"

#define TEXT_UI_PX 16                 /* default UI pixel size */

/* --- rasterizer (kernel/raster.c, pure integer, host-testable) --- */
/* Rasterize glyph `gid` of `f` at `px` pixels into an 8-bit coverage bitmap.
 * On success: *w,*h = bitmap size; *ox = x offset from the pen origin to the
 * bitmap's left; *oy = pixels from the baseline up to the bitmap's top (so the
 * bitmap's top-left sits at (pen_x + *ox, baseline_y - *oy)). `cov` must hold
 * >= (*w)*(*h) bytes (covcap). Returns 0 ok, -1 on error / cov too small. */
int text_raster(const struct ttf_font *f, int gid, int px,
                uint8_t *cov, int covcap, int *w, int *h, int *ox, int *oy);

/* --- text engine (kernel/text.c) --- */
void text_init(void);                 /* load /fonts/ui.ttf + /fonts/mono.ttf */
int  text_draw(int x, int y, const char *utf8, uint32_t color);
int  text_draw_sz(int x, int y, const char *utf8, int px, uint32_t color);
int  text_draw_mono(int x, int y, const char *utf8, int cell_w, uint32_t color);
int  text_width(const char *utf8);
int  text_width_sz(const char *utf8, int px);
int  text_measure(const char *s, int len, int px, int mono);  /* length-delimited run */
int  text_draw_run(int x, int y, const char *s, int len, int px, int mono, uint32_t color);
int  text_line_height(int px);

#endif /* AQUA_TEXT_H */
