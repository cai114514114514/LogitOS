#ifndef LOGIT_TTF_H
#define LOGIT_TTF_H

#include <stdint.h>
#include "fontpath.h"
#include "cff.h"

/* Outline container of the face. */
enum { TTF_OUTLINE_NONE = 0, TTF_OUTLINE_GLYF = 1, TTF_OUTLINE_CFF = 2 };

/* A parsed OpenType face. `data` must outlive the struct (we point into it).
 * Both outline flavours are handled: `glyf` (TrueType quadratics) and `CFF `
 * (Type 2 charstrings, cubics) -- see outline_fmt. Everything else here is a
 * table OFFSET; the tables themselves are read by the module that owns them
 * (otlayout.c for GSUB/GPOS/GDEF/kern, fontcolor.c for COLR/CPAL/CBDT/sbix,
 * fontvar.c for fvar/avar). Offsets are absolute within `data`, 0 = absent. */
struct ttf_font {
    const uint8_t *data; int len;
    int units_per_em;
    int ascent, descent, line_gap;          /* font units */
    uint32_t off_cmap, off_glyf, off_loca, off_hmtx, off_hhea, off_maxp, off_head;
    uint32_t cmap_sub;                       /* offset of the chosen cmap subtable */
    int loca_long;                           /* 1 = 32-bit loca entries, 0 = 16-bit */
    int num_glyphs, num_hmetrics;

    int outline_fmt;                         /* TTF_OUTLINE_* */
    uint32_t off_cff, len_cff;               /* the `CFF ` or `CFF2` table */
    struct cff_font cff;                     /* valid when outline_fmt == TTF_OUTLINE_CFF */

    /* OpenType Layout (read by otlayout.c). */
    uint32_t off_gsub, off_gpos, off_gdef, off_kern;
    /* Colour (read by fontcolor.c). */
    uint32_t off_colr, off_cpal, off_cblc, off_cbdt, off_sbix, off_svg;
    /* Variations (read by fontvar.c). */
    uint32_t off_fvar, off_avar, off_gvar, off_hvar;
    /* Misc, exposed because callers ask for them. */
    uint32_t off_os2, off_post, off_vhea, off_vmtx, off_name;
};

/* Parse the table directory + head/hhea/maxp, pick a Unicode cmap subtable, and
 * set up whichever outline source the font carries. Returns 0 on success, -1 on
 * a malformed or unsupported font. Accepts 0x00010000, 'true' and 'OTTO'; a
 * 'ttcf' collection is rejected (extract a face first). */
int ttf_parse(const uint8_t *data, int len, struct ttf_font *f);

/* Absolute offset (and length) of a table by tag, e.g. ttf_table(f,
 * FONT_TAG('M','A','T','H'), &off, &len). Returns 0 if absent. Exists so a
 * caller can reach a table this header does not name. */
int ttf_table(const struct ttf_font *f, uint32_t tag, uint32_t *off, uint32_t *len);

/* Map a Unicode code point to a glyph id (0 = .notdef / absent). */
int ttf_glyph_id(const struct ttf_font *f, uint32_t codepoint);

/* Advance width of a glyph, in font units. */
int ttf_advance(const struct ttf_font *f, int gid);

/* Glyph `gid`'s outline as drawing commands in font units, y up. Works for both
 * glyf and CFF; a glyf outline yields FP_QUAD, a CFF outline FP_CUBIC. `p` must
 * already be fp_init'd over caller-owned memory, and `scratch` (scratchlen
 * bytes) holds the glyf point arrays while they are converted -- it is unused
 * for CFF, where the charstring interpreter emits commands directly. Returns 0
 * on success, -1 on a malformed glyph or if the path did not fit. An empty glyph
 * yields p->n == 0.
 *
 * This is what the rasterizer uses. ttf_glyph_outline below is the older
 * point-array form, kept because it is a cheaper way to ask glyf-specific
 * questions (contour counts, on-curve flags). */
int ttf_glyph_path(const struct ttf_font *f, int gid, struct fp_path *p,
                   void *scratch, int scratchlen);

/* A glyf outline in font units (y up). Contours are runs of points delimited by
 * contour_end[] (index of the last point of each contour). `on[i]` = 1 for an
 * on-curve point, 0 for an off-curve (quadratic control) point. The arrays point
 * into the caller-provided `scratch` buffer. */
struct ttf_outline {
    short *x, *y; uint8_t *on;
    int *contour_end;
    int npts, ncontours;
    int xmin, ymin, xmax, ymax;
};

/* Extract glyph `gid`'s glyf outline (resolving composites). Returns 0 on
 * success, -1 on error, if `scratch` (scratchlen bytes) is too small, or if the
 * font has CFF outlines rather than glyf. A blank glyph yields ncontours = 0. */
int ttf_glyph_outline(const struct ttf_font *f, int gid,
                      struct ttf_outline *out, void *scratch, int scratchlen);

#endif /* LOGIT_TTF_H */
