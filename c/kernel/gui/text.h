#ifndef LOGIT_TEXT_H
#define LOGIT_TEXT_H

#include <stdint.h>
#include "ttf.h"
/* text_raster and the two COLR-layer entry points. They used to be declared
 * here and implemented in c/kernel/gui/raster.c -- a second scanline coverage
 * rasterizer. That file is gone; c/lib/text/glyphras.c converts an outline to a
 * gfx_path and Open Logit rasterizes it, like everything else that draws. */
#include "glyphras.h"

#define TEXT_UI_PX 16                 /* default UI pixel size */

/* --- text engine (kernel/text.c) --- */
void text_init(void);                 /* load /fonts/ui.ttf + /fonts/mono.ttf */
int  text_draw(int x, int y, const char *utf8, uint32_t color);
int  text_draw_sz(int x, int y, const char *utf8, int px, uint32_t color);
int  text_draw_mono(int x, int y, const char *utf8, int cell_w, uint32_t color);
int  text_draw_mono_sz(int x, int y, const char *utf8, int px, int cell_w, uint32_t color);
int  text_width(const char *utf8);
int  text_width_sz(const char *utf8, int px);
/* The last int of these two is a FACE MASK, not a boolean: LOGIT_FACE_MONO (1)
 * and LOGIT_FACE_BOLD (2) from include/abi/logit_abi.h, ORed. Bit 0 is the
 * `mono` flag these two used to take, unchanged, so every caller that passes
 * 0 or 1 -- which is every caller outside the browser, and every host stub --
 * means what it always meant.
 *
 * MEASURE AND DRAW MUST BE HANDED THE SAME MASK. Bold advances are wider than
 * regular ones, so a run measured at one weight and drawn at another does not
 * fail, it overflows its own box: text off the right edge of every heading. */
int  text_measure(const char *s, int len, int px, int face);  /* length-delimited run */
int  text_draw_run(int x, int y, const char *s, int len, int px, int face, uint32_t color);
int  text_line_height(int px);

#endif /* LOGIT_TEXT_H */
