#ifndef LOGIT_SHAPE_H
#define LOGIT_SHAPE_H

#include <stdint.h>
#include "ttf.h"
#include "script.h"

/* Text shaping: code points in, POSITIONED GLYPHS out.
 *
 * This is the half of the text line that needs the font. It applies the
 * OpenType Layout tables through c/lib/text/otlayout.c -- GSUB for contextual
 * forms and ligatures, GPOS for kerning and mark attachment -- plus the legacy
 * `kern` table for the many fonts that carry only that.
 *
 * The point of it, concretely: Arabic letters have initial/medial/final/
 * isolated forms chosen by context. Without GSUB you get disconnected isolated
 * letters, which no Arabic reader will accept. The bidi half (c/lib/text/bidi.c)
 * already puts the characters in the right ORDER; this puts the right GLYPHS
 * there.
 *
 * Correctness is measured, not asserted: tests/unit/shape_test.c shapes a
 * corpus with this code and with HarfBuzz and compares glyph id by glyph id and
 * position by position.
 *
 * Zero allocation and zero global state: the caller owns the glyph buffer.
 *
 * Cost, measured rather than guessed (host, -O2, DejaVu Sans): 20 us for a
 * 43-character Latin line, 2.4 us for a five-character label, 4.4 us for a
 * seven-code-point Arabic word. Most of that is rebuilding the feature plan on
 * every call -- there is no cache, because a cache is global state and this is
 * called from the kernel. It does not show today because the two Noto subsets
 * the UI actually draws Latin and CJK with have no GSUB or GPOS at all (0.56 us
 * for the same 43 characters), so only Arabic and Hebrew runs pay it. A font
 * with layout tables for Latin would make this the thing to fix.
 *
 * NOT here, and deliberately named rather than silently wrong:
 *   - Indic/Khmer/Myanmar syllable reordering. Those scripts get the plain
 *     path, which is not enough for them.
 *   - Unicode normalization (HarfBuzz decomposes/recomposes before shaping).
 *   - The Arabic presentation-forms fallback for fonts with no joining
 *     features.
 *   - Vertical writing.
 */

/* One shaped glyph. The first six fields are the output; the rest are the
 * shaper's working state, public only because the buffer is caller memory. */
struct shape_glyph {
    uint16_t gid;
    uint16_t _pad;
    int32_t  cluster;                       /* index of the first code point */
    int32_t  x_advance, y_advance;          /* font units */
    int32_t  x_offset,  y_offset;           /* font units */

    uint32_t cp;                            /* the code point it came from */
    uint32_t mask;                          /* feature masks that apply here */
    uint32_t props;                         /* SH_P_* below */
    int32_t  lig_id, lig_comp;
    int32_t  attach_chain;                  /* relative index of the parent */
    uint32_t attach_type;
};

/* Glyph properties, laid out so the low bits line up with the LookupFlag
 * Ignore* bits, which is what makes the flag test a single AND. */
#define SH_P_BASE       0x02u
#define SH_P_LIGATURE   0x04u
#define SH_P_MARK       0x08u
#define SH_P_CLASS_MASK 0x0Eu
#define SH_P_SUBSTITUTED 0x10u
#define SH_P_LIGATED     0x20u
#define SH_P_MULTIPLIED  0x40u
#define SH_P_IGNORABLE   0x80u              /* Default_Ignorable_Code_Point */
/* Mark attachment class lives in bits 8..15, as LookupFlag does. */

/* Shape one run of one script, one direction and one font.
 *
 *   cps/n    the run's code points, in LOGICAL order
 *   script   SC_* from script.h
 *   rtl      non-zero for a right-to-left run
 *   buf/cap  caller-owned output; cap must be >= n and wants headroom for
 *            one-to-many substitutions (2*n + 8 is always enough in practice)
 *
 * Output is in VISUAL order, left to right, exactly as HarfBuzz returns it: an
 * RTL run comes back with its last logical character first. Advances and
 * offsets are in FONT UNITS; scale by px/units_per_em at the drawing end.
 *
 * Returns the glyph count, or -1 if the run did not fit `cap`.
 *
 * With SHAPE_NEGATIVE_CONTROL defined at compile time this degrades to what
 * the text layer did before shaping existed -- one glyph per code point out of
 * cmap, advances summed, no GSUB and no GPOS. That is the control the HarfBuzz
 * differential has to reject. */
int shape_run(const struct ttf_font *f, const uint32_t *cps, int n,
              int script, int rtl, struct shape_glyph *buf, int cap);

/* Total advance of a shaped run, in font units. Goes through shape_run, so it
 * cannot disagree with what shape_run draws -- which is the whole point: a
 * measurement that summed per-glyph advances would miss every ligature and
 * every kern pair. */
int shape_run_width(const struct ttf_font *f, const uint32_t *cps, int n,
                    int script, int rtl, struct shape_glyph *buf, int cap);

/* ---------------------------------------------------------- whole lines -- *
 *
 * shape_line() is the entry point the text layer uses. It does UTF-8 decoding,
 * bidi, script/font segmentation, shaping and scaling to device pixels, and it
 * either draws (through a callback) or only measures.
 *
 * There is exactly ONE of it on purpose. text_measure and text_draw_run must
 * agree at the same px -- wm.c measures at S(px) and divides the answer back --
 * and shaping breaks any measurement that sums per-character advances, because
 * a ligature is one glyph for two characters and a kern pair is narrower than
 * its parts. Making measure and draw the same function makes them agree by
 * construction rather than by inspection.
 */

/* Fonts in preference order; the first that has a glyph for a code point wins,
 * and a font change starts a new run. */
#define SHAPE_MAX_FONTS 4
struct shape_font_set {
    const struct ttf_font *f[SHAPE_MAX_FONTS];
    int n;
};

/* Called once per glyph, left to right, when drawing.
 *   fidx     index into the font set
 *   gid      glyph id
 *   x        pen x plus the glyph's x offset, in device pixels
 *   y_off    how far ABOVE the baseline the glyph origin sits, device pixels
 *            (GPOS y offset; 0 for everything without mark attachment) */
struct shape_emit {
    void (*glyph)(void *ud, int fidx, int gid, int x, int y_off);
    void *ud;
};

/* Caller-owned working memory. Zero allocation is not a slogan here: this is
 * called from the kernel's compositor loop. */
struct shape_scratch {
    uint32_t *cps;                 /* ncp_cap entries */
    uint8_t  *levels;              /* ncp_cap entries */
    int      *order;               /* ncp_cap entries */
    struct shape_glyph *glyphs;    /* nglyph_cap entries */
    struct text_run *runs;         /* nrun_cap entries */
    void *bidi;                    /* bidi_scratch_size(ncp_cap) bytes */
    int ncp_cap, nglyph_cap, nrun_cap, bidi_cap;
};

/* Lay out `len` bytes of UTF-8 at `px` device pixels starting at pen x = `x0`.
 *
 *   cell     0 for proportional text. Non-zero selects the terminal's fixed
 *            grid: one cell per code point, two for a wide one, and NO shaping
 *            -- a cell grid and a ligature cannot both be honoured, and the
 *            terminal needs the grid. Arabic in the Terminal is therefore still
 *            unshaped; that is a property of cell grids, and it is named here
 *            rather than hidden.
 *   em       NULL to measure only.
 *
 * Returns the pen x after the last glyph, so `ret - x0` is the width. */
int shape_line(const struct shape_font_set *fs, const char *utf8, int len,
               int px, int cell, int x0, const struct shape_emit *em,
               struct shape_scratch *sc);

#endif /* LOGIT_SHAPE_H */
