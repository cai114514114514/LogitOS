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
int  text_measure(const char *s, int len, int px, int mono);  /* length-delimited run */
int  text_draw_run(int x, int y, const char *s, int len, int px, int mono, uint32_t color);
int  text_line_height(int px);

#endif /* LOGIT_TEXT_H */
