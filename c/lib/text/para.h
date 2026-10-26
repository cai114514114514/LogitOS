#ifndef LOGIT_TEXT_PARA_H
#define LOGIT_TEXT_PARA_H

/* Shared paragraph layout: greedy wrap over UAX #14 opportunities, line
 * height, max lines and head / middle / tail ellipsis, always at caret
 * (grapheme-cluster) boundaries.  Pure and freestanding: no syscalls, no libc,
 * no malloc.  Ring-3 only: this file is filtered out of the kernel's C_SRC and
 * reaches apps through $(AUI_LIBS) (libtl.a).
 *
 * All measuring goes through ONE callback, the SYS_TEXT_MEASURE carets query
 * (or a host fake).  tl_para_layout calls it once for the text and, only when
 * an ellipsis is placed, once more for U+2026: at most 2 calls per paragraph.
 *
 * Not reentrant (tl_break_utf8 uses static scratch, and so does this file).
 * Text longer than TL_PARA_MAX_BYTES, or with more than TL_BREAK_MAX code
 * points, is refused (-1), never mis-laid-out.
 *
 * Lines are in logical order; bidi reordering is the caller's job.  The y of
 * line i is i * line_height24_8 (tl_para_line_y); the layout never needs it. */

#define TL_PARA_MAX_BYTES 8192

enum { TL_ALIGN_L = 0, TL_ALIGN_C = 1, TL_ALIGN_R = 2 };
enum { TL_TRUNC_TAIL = 0, TL_TRUNC_HEAD = 1, TL_TRUNC_MIDDLE = 2, TL_TRUNC_NONE = 3 };
enum { TL_ELL_NONE = 0, TL_ELL_TAIL = 1, TL_ELL_HEAD = 2, TL_ELL_MIDDLE = 3 };

/* Caret stops of one run, as SYS_TEXT_MEASURE's carets query: byte offsets
 * 0..len at grapheme-cluster boundaries in logical order, and device 24.8 x
 * relative to the run origin.  Returns the stop count, or -1. */
struct tl_measure_cb {
    int (*carets)(void *ctx, const char *s, int len, int px, unsigned face,
                  unsigned short *off, int *x24_8, int max);
    void *ctx;
};

struct tl_para_opts {
    int width24_8;        /* line box width; <= 0 means unlimited (only mandatory breaks) */
    int px, face;
    int line_height24_8;  /* carried for the caller; see tl_para_line_y */
    int max_lines;        /* 0 = unlimited; the last allowed line is truncated per `trunc` */
    int align;            /* TL_ALIGN_* */
    int trunc;            /* TL_TRUNC_*; NONE just stops after max_lines */
};

/* One output line.  The source bytes are [start, start+len).  When `ellipsis`
 * is not TL_ELL_NONE the bytes [cut_off, cut_off+cut_len) inside that range
 * are replaced by U+2026, so the drawn text is
 *     s[start .. cut_off) + "\xe2\x80\xa6" + s[cut_off+cut_len .. start+len)
 * (TAIL: the second piece is empty; HEAD: the first is empty).  Without an
 * ellipsis cut_off = start+len and cut_len = 0.  width24_8 is the drawn width
 * (trailing spaces and line ends excluded, ellipsis included); x_off24_8 is
 * the alignment offset inside the box. */
struct tl_line {
    unsigned short start, len;
    int width24_8, x_off24_8;
    unsigned char ellipsis;
    unsigned short cut_off, cut_len;
};

/* Returns the number of lines written to `out` (>= 1 for any valid input,
 * an empty paragraph is one empty line), or -1 on bad arguments, text too
 * large, the callback failing, `out` too small (max_out) or the caret scratch
 * too small (scratch_max must be >= the stop count, at most len+1). */
int tl_para_layout(const char *s, int len, const struct tl_para_opts *o,
                   const struct tl_measure_cb *cb, struct tl_line *out, int max_out,
                   unsigned short *scratch_off, int *scratch_x, int scratch_max);

static inline int tl_para_line_y(const struct tl_para_opts *o, int line)
{
    return line * o->line_height24_8;
}

#endif /* LOGIT_TEXT_PARA_H */
