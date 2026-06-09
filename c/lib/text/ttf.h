#ifndef AETHER_TTF_H
#define AETHER_TTF_H

#include <stdint.h>

/* A parsed TrueType font. `data` must outlive the struct (we point into it). */
struct ttf_font {
    const uint8_t *data; int len;
    int units_per_em;
    int ascent, descent, line_gap;          /* font units */
    uint32_t off_cmap, off_glyf, off_loca, off_hmtx, off_hhea, off_maxp, off_head;
    uint32_t cmap_sub;                       /* offset of the chosen cmap subtable */
    int loca_long;                           /* 1 = 32-bit loca entries, 0 = 16-bit */
    int num_glyphs, num_hmetrics;
};

/* Parse the table directory + head/hhea/maxp + pick a Unicode cmap subtable.
 * Returns 0 on success, -1 on a malformed/unsupported font. */
int ttf_parse(const uint8_t *data, int len, struct ttf_font *f);

/* Map a Unicode code point to a glyph id (0 = .notdef / absent). */
int ttf_glyph_id(const struct ttf_font *f, uint32_t codepoint);

/* Advance width of a glyph, in font units. */
int ttf_advance(const struct ttf_font *f, int gid);

/* A glyph outline in font units (y up). Contours are runs of points delimited by
 * contour_end[] (index of the last point of each contour). `on[i]` = 1 for an
 * on-curve point, 0 for an off-curve (quadratic control) point. The arrays point
 * into the caller-provided `scratch` buffer. */
struct ttf_outline {
    short *x, *y; uint8_t *on;
    int *contour_end;
    int npts, ncontours;
    int xmin, ymin, xmax, ymax;
};

/* Extract glyph `gid`'s outline (resolving composites). Returns 0 on success,
 * -1 on error or if `scratch` (scratchlen bytes) is too small. A blank glyph
 * yields ncontours = 0. */
int ttf_glyph_outline(const struct ttf_font *f, int gid,
                      struct ttf_outline *out, void *scratch, int scratchlen);

#endif /* AETHER_TTF_H */
