#ifndef LOGIT_OTLAYOUT_H
#define LOGIT_OTLAYOUT_H

#include <stdint.h>

/* OpenType Layout table ACCESS: GSUB, GPOS, GDEF and the legacy `kern`.
 *
 * This header is the boundary between the font-file line (which owns parsing)
 * and the shaping line (which owns applying what is parsed). Nothing here
 * decides anything about text: there is no shaper, no glyph buffer, no
 * script/language policy, no mark positioning. Every call is a pure read of the
 * font bytes.
 *
 * Properties the shaper can rely on:
 *  - Zero allocation and zero global state. Everything is either returned by
 *    value or written into a caller-supplied buffer, so it is safe to call from
 *    the kernel, from ring 3, and from several threads at once.
 *  - The font blob is never modified and is not copied; `struct otl_table`
 *    borrows it and must not outlive it.
 *  - Untrusted input is assumed. Out-of-range offsets read as absent, arrays
 *    are clamped to what actually fits the blob, and recursion is bounded.
 *    A malformed font makes calls return -1/0, never a wild pointer.
 *  - Glyph ids are uint16_t, offsets are absolute (from the start of the font
 *    blob), and every "index" is a dense 0-based index into the table's own
 *    lists, so it can be cached.
 *
 * Typical use by a shaper:
 *    otl_open(data, len, gsub_off, &gsub);
 *    s = otl_find_script(&gsub, FONT_TAG('l','a','t','n'));
 *    otl_langsys_features(&gsub, s, -1, &required, feats, 64);
 *    for each feature: otl_feature_tag / otl_feature_lookups
 *    for each lookup:  otl_lookup_info, then otl_subtable + otl_subtable_type
 *                      and the otl_gsub_* / otl_gpos_* readers below.
 */

/* ---------------------------------------------------------------- limits --
 * Contextual rules longer than this are reported as unusable rather than
 * truncated (a truncated rule would match text it must not match). 16 is far
 * past what real fonts use; the longest sequences in Noto/Source are ~6. */
#define OTL_CTX_MAX 16

/* Lookup flags (LookupFlag bit enumeration). */
#define OTL_LF_RIGHT_TO_LEFT       0x0001
#define OTL_LF_IGNORE_BASE         0x0002
#define OTL_LF_IGNORE_LIGATURES    0x0004
#define OTL_LF_IGNORE_MARKS        0x0008
#define OTL_LF_USE_MARK_FILTER_SET 0x0010
#define OTL_LF_MARK_ATTACH_TYPE    0xFF00   /* class in the high byte */

/* GSUB lookup types. */
enum { OTL_GSUB_SINGLE = 1, OTL_GSUB_MULTIPLE, OTL_GSUB_ALTERNATE,
       OTL_GSUB_LIGATURE, OTL_GSUB_CONTEXT, OTL_GSUB_CHAIN,
       OTL_GSUB_EXTENSION, OTL_GSUB_REVERSE_CHAIN };
/* GPOS lookup types. */
enum { OTL_GPOS_SINGLE = 1, OTL_GPOS_PAIR, OTL_GPOS_CURSIVE,
       OTL_GPOS_MARK_BASE, OTL_GPOS_MARK_LIG, OTL_GPOS_MARK_MARK,
       OTL_GPOS_CONTEXT, OTL_GPOS_CHAIN, OTL_GPOS_EXTENSION };

/* GDEF glyph classes. */
enum { OTL_GC_UNCLASSIFIED = 0, OTL_GC_BASE = 1, OTL_GC_LIGATURE = 2,
       OTL_GC_MARK = 3, OTL_GC_COMPONENT = 4 };

/* ------------------------------------------------------------ the table -- */

struct otl_table {
    const uint8_t *data;        /* whole font blob (borrowed) */
    uint32_t len;
    uint32_t off;               /* absolute offset of GSUB/GPOS */
    uint32_t scripts, features, lookups;   /* absolute; 0 if absent */
    uint32_t feature_vars;      /* absolute; 0 unless version 1.1 */
    uint16_t major, minor;
    int is_gpos;                /* 0 = GSUB, 1 = GPOS. Set by the caller's choice
                                 * of otl_open_gsub/otl_open_gpos; only affects
                                 * how lookup types are named. */
};

/* Open GSUB (is_gpos=0) or GPOS (is_gpos=1) at absolute offset `table_off`.
 * Returns 0, or -1 if the header does not parse. `table_off` of 0 = absent. */
int otl_open(const uint8_t *data, uint32_t len, uint32_t table_off, int is_gpos,
             struct otl_table *t);

/* ----------------------------------------------------------- script list -- */

int      otl_script_count(const struct otl_table *t);
uint32_t otl_script_tag(const struct otl_table *t, int si);
/* Index of the script with `tag`, or -1. Tags are 4 bytes, e.g.
 * FONT_TAG('l','a','t','n'); pad short tags with spaces as the spec does. */
int      otl_find_script(const struct otl_table *t, uint32_t tag);

/* Language systems within a script. Index -1 denotes the script's DefaultLangSys
 * (present in nearly every font); 0..otl_langsys_count-1 are the named ones. */
int      otl_langsys_count(const struct otl_table *t, int si);
uint32_t otl_langsys_tag(const struct otl_table *t, int si, int li);
int      otl_find_langsys(const struct otl_table *t, int si, uint32_t tag);

/* Feature indices a (script, langsys) selects. Writes up to `cap` indices into
 * `out` and returns how many the font lists (which may exceed `cap`; only `cap`
 * were written). `*required` receives the required-feature index or -1.
 * Returns -1 if the script/langsys does not exist. */
int otl_langsys_features(const struct otl_table *t, int si, int li,
                         int *required, uint16_t *out, int cap);

/* ---------------------------------------------------------- feature list -- */

int      otl_feature_count(const struct otl_table *t);
uint32_t otl_feature_tag(const struct otl_table *t, int fi);
/* Lookup indices of feature `fi`. Same "returns the true count" contract. */
int      otl_feature_lookups(const struct otl_table *t, int fi, uint16_t *out, int cap);

/* ----------------------------------------------------------- lookup list -- */

struct otl_lookup {
    uint16_t type;              /* raw type; 7 (GSUB) / 9 (GPOS) = Extension */
    uint16_t flags;             /* OTL_LF_* */
    uint16_t mark_filter_set;   /* valid only if OTL_LF_USE_MARK_FILTER_SET */
    int      nsub;              /* subtable count */
    uint32_t off;               /* absolute offset of the Lookup table */
};

int otl_lookup_count(const struct otl_table *t);
int otl_lookup_info(const struct otl_table *t, int li, struct otl_lookup *out);

/* Absolute offset of subtable `i` of a lookup, or 0. This is the RAW subtable:
 * for an Extension lookup it is the ExtensionPosFormat1/ExtensionSubstFormat1
 * wrapper. Use otl_subtable_type() to see through it. */
uint32_t otl_subtable(const struct otl_table *t, const struct otl_lookup *l, int i);

/* Resolve one subtable through at most one Extension indirection (the spec
 * forbids chaining them). On success *real_type is the effective lookup type
 * and *real_off the offset of the real subtable. Returns 0 or -1. */
int otl_subtable_type(const struct otl_table *t, const struct otl_lookup *l, int i,
                      int *real_type, uint32_t *real_off);

/* ------------------------------------------- coverage and class definition --
 * The two primitives every subtable is built from. Both take the ABSOLUTE
 * offset of the table, which is what the readers below hand back. */

/* Coverage index of `gid` (0..count-1), or -1 if the glyph is not covered. */
int otl_coverage_index(const struct otl_table *t, uint32_t cov, uint16_t gid);
/* How many glyphs the coverage lists (-1 if malformed). */
int otl_coverage_count(const struct otl_table *t, uint32_t cov);
/* The glyph at coverage index `i`, or -1. Lets a shaper enumerate a lookup's
 * inputs without probing all 65536 glyph ids. */
int otl_coverage_glyph(const struct otl_table *t, uint32_t cov, int i);

/* Class value of `gid` in a ClassDef (0 when not listed, which is the spec's
 * meaning of "class 0"). A cov/classdef offset of 0 yields class 0. */
int otl_class_of(const struct otl_table *t, uint32_t cd, uint16_t gid);

/* ------------------------------------------------------------- GSUB reads --
 * Each takes the RESOLVED subtable offset (from otl_subtable_type) and does one
 * lookup. They do not walk the glyph buffer, skip marks, or recurse: that is
 * the shaper's job. */

/* Type 1. Returns the substitute glyph, or -1 if `gid` is not covered. */
int otl_gsub_single(const struct otl_table *t, uint32_t sub, uint16_t gid);

/* Type 2 (one glyph -> a sequence). Writes up to `cap`, returns the true
 * sequence length, or -1 if not covered. */
int otl_gsub_multiple(const struct otl_table *t, uint32_t sub, uint16_t gid,
                      uint16_t *out, int cap);

/* Type 3 (one of several alternates). Returns the count; *out gets alternate
 * `alt` (clamped to the last) when count > 0. -1 if not covered. */
int otl_gsub_alternate(const struct otl_table *t, uint32_t sub, uint16_t gid,
                       int alt, uint16_t *out);

/* Type 4. `rest`/`nrest` are the glyphs FOLLOWING `first` in the buffer, with
 * ignored glyphs already removed by the caller. On a match returns the ligature
 * glyph and sets *consumed to the total component count (>= 1, including
 * `first`); returns -1 with no match. Longest match wins, as the spec's
 * ordering requires. */
int otl_gsub_ligature(const struct otl_table *t, uint32_t sub, uint16_t first,
                      const uint16_t *rest, int nrest, int *consumed);

/* Type 8 (reverse chaining single substitution). Returns the substitute for
 * `gid`, or -1 if not covered; the backtrack/lookahead coverages come back
 * through otl_revchain(). */
int otl_gsub_reverse(const struct otl_table *t, uint32_t sub, uint16_t gid);
struct otl_revchain {
    int nback, nahead;
    uint32_t back[OTL_CTX_MAX], ahead[OTL_CTX_MAX];   /* coverage offsets */
    uint32_t coverage;
};
int otl_revchain(const struct otl_table *t, uint32_t sub, struct otl_revchain *out);

/* ------------------------------------------------------------- GPOS reads -- */

/* A ValueRecord, already expanded to the four metrics in font units. Device /
 * VariationIndex tables are NOT applied (we do not hint and do not resolve
 * variations here); `has_device` says the font carried some, so a shaper can
 * decide whether that matters to it. */
struct otl_value {
    int x_placement, y_placement, x_advance, y_advance;
    int has_device;
};

/* An Anchor. `fmt` is 1, 2 (with a contour point in `point`) or 3 (device
 * tables, not applied). */
struct otl_anchor { int x, y, fmt, point; };

/* Type 1. Fills *v and returns 0, or -1 if not covered. */
int otl_gpos_single(const struct otl_table *t, uint32_t sub, uint16_t gid,
                    struct otl_value *v);

/* Type 2 (pair). Fills the two records (either may end up all-zero) and returns
 * 0 on a match, -1 otherwise. Handles both format 1 (glyph pairs) and format 2
 * (class pairs). */
int otl_gpos_pair(const struct otl_table *t, uint32_t sub, uint16_t g1, uint16_t g2,
                  struct otl_value *v1, struct otl_value *v2);

/* Type 3 (cursive attachment). Either anchor may be absent; *has_entry /
 * *has_exit say which. -1 if not covered. */
int otl_gpos_cursive(const struct otl_table *t, uint32_t sub, uint16_t gid,
                     struct otl_anchor *entry, int *has_entry,
                     struct otl_anchor *exit_, int *has_exit);

/* Types 4/5/6 (mark-to-base, mark-to-ligature, mark-to-mark). One call resolves
 * the mark's class + anchor and the base's anchor for that class.
 * `lig_component` is used by type 5 only (ignored otherwise).
 * Returns 0 on success, -1 if either glyph is not covered by this subtable. */
int otl_gpos_mark(const struct otl_table *t, uint32_t sub, int type,
                  uint16_t mark_gid, uint16_t base_gid, int lig_component,
                  struct otl_anchor *mark_anchor, struct otl_anchor *base_anchor,
                  int *mark_class);

/* Component count of `lig_gid` in a type 5 subtable (so a shaper can clamp
 * lig_component). -1 if not covered. */
int otl_gpos_lig_components(const struct otl_table *t, uint32_t sub, uint16_t lig_gid);

/* ------------------------------------------------ contextual lookups (5/6, 7/8) --
 * GSUB 5/6 and GPOS 7/8 share one wire format, so they share one reader. The
 * three subtable formats are exposed as-is rather than flattened, because a
 * shaper matches them differently (glyph ids / classes / coverage sets), but the
 * plumbing -- rule-set indexing, bounds, the lookup records -- is done here.
 *
 * "chain" is 0 for the plain contextual types (GSUB 5 / GPOS 7) and 1 for the
 * chaining ones (GSUB 6 / GPOS 8). In the plain case nback and nahead are
 * always 0 and the backtrack/lookahead arrays are unused. */

struct otl_ctx_info {
    int fmt;                    /* 1 = glyph, 2 = class, 3 = coverage */
    int chain;
    uint32_t coverage;          /* fmt 1/2: coverage of the first input glyph */
    uint32_t cd_back, cd_input, cd_ahead;   /* fmt 2 ClassDefs (cd_input only,
                                             * for the non-chaining case) */
    int nsets;                  /* fmt 1/2: RuleSet count. fmt 3: always 1 */
};

struct otl_lookup_rec { uint16_t seq, lookup; };

struct otl_ctx_rule {
    int fmt, chain;
    int nback, ninput, nahead;  /* ninput COUNTS the first glyph */
    /* fmt 1: glyph ids. fmt 2: class values. fmt 3: unused (see cov*). */
    uint16_t back[OTL_CTX_MAX], input[OTL_CTX_MAX], ahead[OTL_CTX_MAX];
    /* fmt 3 only: absolute coverage offsets, one per position. */
    uint32_t covback[OTL_CTX_MAX], covinput[OTL_CTX_MAX], covahead[OTL_CTX_MAX];
    int nrec;
    struct otl_lookup_rec rec[OTL_CTX_MAX];
};

/* Read the subtable header. Returns 0 or -1. */
int otl_ctx_open(const struct otl_table *t, uint32_t sub, int chain,
                 struct otl_ctx_info *ci);
/* Rules in set `set` (fmt 1/2). For fmt 3 there is one set with one rule.
 * For fmt 1 the set index is the coverage index of the first glyph; for fmt 2
 * it is that glyph's input class. -1 on error. */
int otl_ctx_rule_count(const struct otl_table *t, const struct otl_ctx_info *ci,
                       uint32_t sub, int set);
/* Decode one rule. Returns 0, or -1 if it is malformed or longer than
 * OTL_CTX_MAX (deliberately refused rather than truncated). */
int otl_ctx_rule(const struct otl_table *t, const struct otl_ctx_info *ci,
                 uint32_t sub, int set, int rule, struct otl_ctx_rule *out);

/* ------------------------------------------------------------------ GDEF -- */

struct otl_gdef {
    const uint8_t *data; uint32_t len; uint32_t off;
    uint32_t glyph_class, attach_list, lig_caret, mark_attach_class, mark_glyph_sets;
    uint16_t major, minor;
};
int otl_gdef_open(const uint8_t *data, uint32_t len, uint32_t off, struct otl_gdef *g);
/* OTL_GC_*; 0 when the font has no GlyphClassDef. */
int otl_gdef_class(const struct otl_gdef *g, uint16_t gid);
/* Mark attachment class (0 = none). */
int otl_gdef_mark_attach(const struct otl_gdef *g, uint16_t gid);
/* Is `gid` in mark filtering set `set`?  0/1, or -1 if there is no such set. */
int otl_gdef_mark_set_covers(const struct otl_gdef *g, int set, uint16_t gid);
/* Ligature caret positions of `gid`, in font units along the x axis. Writes up
 * to `cap`, returns the true count (0 if none). Format 2 (contour point) carets
 * report the point index negated minus one, so a caller can tell them apart:
 * value >= 0 is a coordinate, value < 0 means point index (-value - 1). */
int otl_gdef_lig_carets(const struct otl_gdef *g, uint16_t gid, int *out, int cap);

/* ------------------------------------------------------- the `kern` table --
 * Plenty of fonts ship kerning ONLY here (DejaVu, most older Type-1 ports), so
 * a shaper that only reads GPOS silently loses it. Both dialects are handled:
 * the original TrueType version 0 (uint16 version, uint16 nTables, per-subtable
 * coverage byte) and Apple's version 1.0 (uint32 version, uint32 nTables,
 * 32-bit subtable lengths). Only format 0 (sorted pair list) is read -- format 2
 * (class array) appears essentially only in Apple system fonts, and format 1/3
 * are state machines that belong to AAT, not to us. */

struct otl_kern {
    const uint8_t *data; uint32_t len, off;
    int ntables, apple;
};
int otl_kern_open(const uint8_t *data, uint32_t len, uint32_t off, struct otl_kern *k);
/* Summed kerning value for the pair, in font units (0 when unkerned). Only
 * horizontal, non-cross-stream, non-minimum subtables contribute -- the others
 * are not additive and would corrupt the advance. */
int otl_kern_pair(const struct otl_kern *k, uint16_t left, uint16_t right);

#endif /* LOGIT_OTLAYOUT_H */
