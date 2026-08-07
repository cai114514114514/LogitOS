#ifndef LOGIT_CFF_H
#define LOGIT_CFF_H

#include <stdint.h>
#include "fontpath.h"

/* Compact Font Format: the outline container of OpenType/CFF (`OTTO`) fonts,
 * and of CFF2 in variable OpenType. A large share of professionally made fonts
 * ship CFF rather than glyf, and until this existed such a font was recognised
 * by ttf_parse and then could not be drawn at all.
 *
 * What is implemented: the INDEX structures, Header, Top DICT, String INDEX,
 * Private DICT (including per-FD Private DICTs of CID-keyed fonts), global and
 * local subroutines with the count-dependent bias, charset (formats 0/1/2 and
 * the predefined ones) and FDSelect (formats 0/3), and a complete Type 2
 * charstring interpreter -- every path operator, both flex pairs, seac through
 * the deprecated 4-argument endchar, and the arithmetic/storage operators.
 * Hints (hstem/vstem/hstemhm/vstemhm/hintmask/cntrmask) are parsed exactly far
 * enough to keep the instruction stream in sync -- the stem count decides how
 * many mask bytes follow -- and then discarded, because we do not hint.
 *
 * CFF2 is parsed and interpreted at the DEFAULT INSTANCE: `vsindex` is tracked
 * and `blend` correctly consumes its operands and keeps the default values,
 * which is exactly right for the default instance and wrong for any other. See
 * cff_font.is_cff2 and the note on cff_parse.
 *
 * Everything is a read of the font blob. No allocation, no globals. */

struct cff_font {
    const uint8_t *data;        /* whole font blob (borrowed) */
    uint32_t len;
    uint32_t base;              /* offset of the CFF table within the blob */
    uint32_t size;              /* its length */

    int is_cff2;
    int charstring_type;        /* 1 or 2; only 2 is interpreted */
    int nglyphs;
    int upem;                   /* from FontMatrix[0]; 1000 when absent */
    int is_cid;

    uint32_t charstrings;       /* absolute offset of the CharStrings INDEX */
    uint32_t gsubrs;            /* absolute offset of the Global Subr INDEX (0 = none) */
    uint32_t strings;           /* absolute offset of the String INDEX (0 = none) */
    uint32_t charset;           /* absolute; 0..2 encode the predefined charsets */
    uint32_t fdarray, fdselect; /* absolute; 0 = none */
    uint32_t vstore;            /* CFF2 ItemVariationStore; 0 = none */

    /* Default (non-CID) Private DICT results. */
    uint32_t subrs;             /* absolute offset of the local Subr INDEX */
    int32_t default_width, nominal_width;

    int32_t font_bbox[4];       /* xmin ymin xmax ymax, font units (0 if absent) */
};

/* Parse the CFF table at [off, off+size) of `data`.
 * Returns 0, or -1 if the font is malformed or uses a charstring type we do not
 * interpret. A CFF2 font parses with is_cff2 = 1 and renders its default
 * instance. */
int cff_parse(const uint8_t *data, uint32_t len, uint32_t off, uint32_t size,
              struct cff_font *f);

/* Build glyph `gid`'s outline into `p` (already fp_init'd). Returns 0 on
 * success, -1 on a malformed charstring, an out-of-range gid, or a path that
 * did not fit (p->overflow is then set). An empty glyph yields p->n == 0. */
int cff_glyph_path(const struct cff_font *f, int gid, struct fp_path *p);

/* Advance width the CFF itself declares for `gid`, in font units. OpenType/CFF
 * fonts carry the authoritative advances in hmtx and this is redundant there;
 * it exists for bare CFF and as a cross-check. Returns -1 if unknown. */
int cff_glyph_width(const struct cff_font *f, int gid);

/* gid -> SID (CFF1 charset). Returns -1 if unavailable. Exposed because the
 * charset is also how a glyph NAME is found, which some callers want. */
int cff_glyph_sid(const struct cff_font *f, int gid);
/* The first gid whose charset entry is `sid`, or -1. */
int cff_sid_glyph(const struct cff_font *f, int sid);

#endif /* LOGIT_CFF_H */
