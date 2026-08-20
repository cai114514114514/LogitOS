/* Host stand-in for c/apps/logit.h, used ONLY by tests/unit/paint_test.c.
 *
 * browser_paint.c draws through five syscall wrappers and nothing else, so the
 * cheapest way to test it on the host is to replace those five with recorders
 * and assert on the ops. The real logit.h issues `int 0x80`, which a host
 * process obviously cannot do -- so the test build puts THIS directory first on
 * the include path and `#include "logit.h"` resolves here instead. (Same trick
 * as tests/unit/kheapstub for the kernel heap.)
 *
 * The recorded op list is the painter's real output: geometry, colour, and --
 * crucially for the blended paths -- the alpha the painter chose. */
#ifndef PAINTHOST_LOGIT_H
#define PAINTHOST_LOGIT_H

#include <stdint.h>

enum { OP_CLIP, OP_RECT, OP_RRECT, OP_BLIT, OP_TEXT };

/* Source samples kept per BLIT. A ramp (a gradient strip, a shadow falloff)
 * and a coverage tile both arrive as a MULTI-pixel source, and `color`/`alpha`
 * above only describe the 1x1 case -- so before this the recorder could see
 * that a gradient was blitted and not what was in it.
 *
 * SAMPLED, not copied: the source is up to 1024 entries and the buffer it
 * lives in is a file static the painter overwrites on its next call, so a
 * pointer would dangle in the most misleading possible way (it would read as
 * whatever was drawn LAST). 16 evenly-spaced samples answer the questions a
 * gate actually asks -- which end is which colour, does the alpha fall off,
 * where is the stop -- at 1/16 of the source's length, and cost 64 bytes an
 * op. */
#define PAINT_NSAMP 16

struct paintop {
    int kind;
    int x, y, w, h;
    unsigned color;        /* RECT/RRECT/TEXT: the colour asked for.
                            * BLIT of a 1x1 source: its RGB. */
    int alpha;             /* BLIT: the source alpha. 255 elsewhere. */
    int radius;            /* RRECT */
    int px, mono;          /* TEXT */
    const char *text; int len;
    int solid;             /* BLIT: 1 when the source was a single pixel, i.e.
                            * an alpha FILL rather than an image */
    int sw, sh;            /* BLIT: the SOURCE's dimensions. A 1 x n source is
                            * a ramp the compositor replicates across x; n x 1
                            * is the same along y; anything else is a tile. */
    unsigned char samp[PAINT_NSAMP][4];  /* straight RGBA, evenly spaced along
                                          * the source in row-major order */
    int nsamp;
};

#define PAINT_MAXOPS 4096
extern struct paintop paint_ops[PAINT_MAXOPS];
extern int paint_nops;

static inline struct paintop *paint_push(int kind)
{
    static struct paintop sink;
    if (paint_nops >= PAINT_MAXOPS) return &sink;
    struct paintop *o = &paint_ops[paint_nops++];
    o->kind = kind; o->x = o->y = o->w = o->h = 0;
    o->color = 0; o->alpha = 255; o->radius = 0;
    o->px = 0; o->mono = 0; o->text = 0; o->len = 0; o->solid = 0;
    o->sw = o->sh = 0; o->nsamp = 0;
    return o;
}

struct logit_run { int x, y, px, mono; unsigned color; const char *s; int len; };
struct logit_blit { int x, y, w, h; const unsigned char *rgba; int sw, sh; };

static inline void gui_rect(int x, int y, int w, int h, unsigned color)
{ struct paintop *o = paint_push(OP_RECT); o->x=x; o->y=y; o->w=w; o->h=h; o->color=color; }

static inline void gui_rrect(int x, int y, int w, int h, int radius, unsigned color)
{ struct paintop *o = paint_push(OP_RRECT); o->x=x; o->y=y; o->w=w; o->h=h;
  o->radius=radius; o->color=color; }

static inline void gui_clip(int x, int y, int w, int h)
{ struct paintop *o = paint_push(OP_CLIP); o->x=x; o->y=y; o->w=w; o->h=h; }

static inline void gui_text_run(int x, int y, int px, int mono, unsigned color,
                                const char *s, int len)
{ struct paintop *o = paint_push(OP_TEXT); o->x=x; o->y=y; o->px=px; o->mono=mono;
  o->color=color; o->text=s; o->len=len; }

static inline void gui_blit(int x, int y, int w, int h, const unsigned char *rgba,
                            int sw, int sh)
{
    struct paintop *o = paint_push(OP_BLIT);
    o->x=x; o->y=y; o->w=w; o->h=h; o->solid = (sw == 1 && sh == 1);
    o->sw = sw; o->sh = sh;
    if (rgba && o->solid) {
        o->color = ((unsigned)rgba[0] << 16) | ((unsigned)rgba[1] << 8) | rgba[2];
        o->alpha = rgba[3];
    }
    if (rgba && sw > 0 && sh > 0) {
        long n = (long)sw * sh;
        int k = n < PAINT_NSAMP ? (int)n : PAINT_NSAMP;
        for (int i = 0; i < k; i++) {
            /* Spaced so sample 0 is the first source pixel and sample k-1 the
             * LAST -- a ramp's two ends are the whole point, and an
             * i*n/k spacing would never reach the far end. */
            long idx = (k == 1) ? 0 : (long)i * (n - 1) / (k - 1);
            for (int c = 0; c < 4; c++) o->samp[i][c] = rgba[idx * 4 + c];
        }
        o->nsamp = k;
    }
}

/* The painter asks the compositor for the display's backing scale, because the
 * coverage masks it blits have to be generated at DEVICE size or the
 * compositor's rescale blurs them. On the host there is no display; 100 makes
 * points and device pixels the same thing, which is what the recorded op
 * geometry is asserted in. */
static inline int ui_scale(void) { return 100; }

#endif /* PAINTHOST_LOGIT_H */
