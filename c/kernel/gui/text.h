#ifndef LOGIT_TEXT_H
#define LOGIT_TEXT_H

#include <stdint.h>
#include "ttf.h"
/* text_raster and the two COLR-layer entry points. They used to be declared
 * here and implemented in c/kernel/gui/raster.c -- a second scanline coverage
 * rasterizer. That file is gone; c/lib/text/glyphras.c converts an outline to a
 * gfx_path and Open Logit rasterizes it, like everything else that draws. */
#include "glyphras.h"

struct ol_display;

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
/* text_draw_run with a global alpha 0..255 drawn on display d. alpha 255 is
 * text_draw_run exactly (d unused); alpha < 255 uses ol_display_blit_glyph_a. */
int  text_draw_run_a(struct ol_display *d, int x, int y, const char *s, int len,
                     int px, unsigned face, uint32_t rgb, int alpha);
int  text_line_height(int px);
/* Device-pixel result; the WM fills the display scale before usercopy. */
struct logit_text_metrics;
int text_measure_run_metrics(const char *s, int len, int px, int face,
                             struct logit_text_metrics *out);

/* Caret stops of one run at grapheme-cluster boundaries, logical order, byte
 * offsets 0..len, device 24.8 x relative to the run origin. Returns the count,
 * or -1 on bad arguments or when more than  stops are needed. Kernel
 * buffers only: the syscall branch stages and copies out after the lock. */
int text_measure_carets(const char *s, int len, int px, unsigned face,
                        unsigned short *byte_off, int *x24_8, int max);

#endif /* LOGIT_TEXT_H */
