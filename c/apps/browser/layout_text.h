#ifndef LOGIT_LAYOUT_TEXT_H
#define LOGIT_LAYOUT_TEXT_H

#include <stdint.h>

/* CSS Text: white-space processing, UAX #14 line breaking, and inline line
 * building.
 *
 * WHY THIS IS ITS OWN MODULE.  layout.c breaks lines at U+0020 and, for a word
 * too wide for the measure, at whatever UTF-8 boundary happens to fit.  That is
 * the correct answer for English prose and the wrong answer for most of the
 * rest of the web: Chinese and Japanese break between ideographs with no space
 * anywhere in the paragraph, so a space-only breaker produces one enormous
 * "word" per sentence and then chops it at an arbitrary column -- including
 * immediately before a closing bracket or a full stop, which no typesetter
 * would do.  This module implements the real algorithm over the real Unicode
 * property table, and it is a separate translation unit so it can be tested
 * against the Unicode Consortium's own conformance corpus without a browser,
 * a font, or a frame buffer.
 *
 * THE LAYERS, and why they are separately callable.  Each one is exactly
 * checkable on its own -- a question about positions in a string, not about
 * pixels -- so each one gets its own entry point and its own assertions:
 *
 *   1. ltx_break_cp / ltx_break_utf8   UAX #14.  Where MAY a line break?
 *   2. ltx_collapse                    CSS Text 3 §4.1.  What does the text
 *                                      become after white-space processing?
 *   3. ltx_text_transform              CSS Text 3 §2.1.
 *   4. ltx_measure_run                 advance arithmetic: letter-spacing,
 *                                      word-spacing, tab stops.
 *   5. ltx_layout_runs                 all of the above plus the measure, into
 *                                      line boxes and fragment positions.
 *
 * Only layer 5 needs a font.  Layers 1-3 are pure string -> string or
 * string -> positions, which is why this line had a scoreboard before the
 * reftest harness existed.
 */

/* ---------------------------------------------------------------- values -- */

/* `white-space-collapse` (CSS Text 4), which is the longhand the algorithm
 * actually branches on. The `white-space` shorthand folds onto this plus
 * `text-wrap`; use ltx_white_space() to expand it. */
enum { LTX_WSC_COLLAPSE,          /* normal, nowrap                */
       LTX_WSC_PRESERVE,          /* pre, pre-wrap, break-spaces   */
       LTX_WSC_PRESERVE_BREAKS,   /* pre-line                      */
       LTX_WSC_PRESERVE_SPACES,   /* preserve spaces, collapse breaks */
       LTX_WSC_BREAK_SPACES };    /* pre + every space is a break opportunity
                                   * and none of them hang */

/* `text-wrap` (only WRAP/NOWRAP change where the breaks land; BALANCE and
 * PRETTY are recognised and treated as WRAP -- see the note on
 * ltx_layout_runs). */
enum { LTX_WRAP_WRAP, LTX_WRAP_NOWRAP, LTX_WRAP_BALANCE, LTX_WRAP_PRETTY,
       LTX_WRAP_STABLE };

/* The `white-space` shorthand's five legacy values. */
enum { LTX_WS_NORMAL, LTX_WS_PRE, LTX_WS_NOWRAP, LTX_WS_PRE_WRAP,
       LTX_WS_PRE_LINE, LTX_WS_BREAK_SPACES };

enum { LTX_WB_NORMAL, LTX_WB_KEEP_ALL, LTX_WB_BREAK_ALL,
       LTX_WB_BREAK_WORD };                       /* word-break */
enum { LTX_OW_NORMAL, LTX_OW_BREAK_WORD, LTX_OW_ANYWHERE };  /* overflow-wrap */
enum { LTX_LB_AUTO, LTX_LB_LOOSE, LTX_LB_NORMAL, LTX_LB_STRICT,
       LTX_LB_ANYWHERE };                         /* line-break */
enum { LTX_HY_NONE, LTX_HY_MANUAL, LTX_HY_AUTO }; /* hyphens */
enum { LTX_TT_NONE, LTX_TT_CAPITALIZE, LTX_TT_UPPERCASE, LTX_TT_LOWERCASE,
       LTX_TT_FULL_WIDTH, LTX_TT_FULL_SIZE_KANA };/* text-transform */
enum { LTX_ALIGN_START, LTX_ALIGN_END, LTX_ALIGN_LEFT, LTX_ALIGN_RIGHT,
       LTX_ALIGN_CENTER, LTX_ALIGN_JUSTIFY };     /* text-align */
enum { LTX_ALAST_AUTO, LTX_ALAST_START, LTX_ALAST_END, LTX_ALAST_LEFT,
       LTX_ALAST_RIGHT, LTX_ALAST_CENTER, LTX_ALAST_JUSTIFY }; /* text-align-last */
enum { LTX_TJ_AUTO, LTX_TJ_NONE, LTX_TJ_INTER_WORD,
       LTX_TJ_INTER_CHARACTER };                  /* text-justify */

/* Break opportunity strength, one per position. */
enum { LTX_BRK_PROHIBITED = 0, LTX_BRK_ALLOWED = 1, LTX_BRK_MANDATORY = 2 };

/* ------------------------------------------------- layer 1: UAX #14 ------- */

/* The tailorings CSS is allowed to apply to the algorithm.  All zero is
 * "line-break: auto, word-break: normal, overflow-wrap: normal", i.e. plain
 * UAX #14 with its default class resolutions -- which is exactly what the
 * Unicode conformance corpus expects, so the corpus runs against this struct
 * zeroed and nothing else. */
struct ltx_lbopt {
    unsigned char line_break;    /* LTX_LB_*    */
    unsigned char word_break;    /* LTX_WB_*    */
    unsigned char overflow_wrap; /* LTX_OW_*    */
    unsigned char ai_is_id;      /* resolve the ambiguous class AI as ID
                                  * (an East Asian locale) instead of AL */
    unsigned char hyphens_none;  /* `hyphens: none`: U+00AD SOFT HYPHEN stops
                                  * being a break opportunity.  Spelled as the
                                  * negative so that a zeroed struct is plain
                                  * UAX #14 -- which is what the Unicode
                                  * conformance corpus expects, and the corpus
                                  * is this module's scoreboard. */
};

/* Raw Line_Break class of `cp` (LB_* from linebreak_data.inc; SA and SG are
 * already folded, everything else is as the UCD states it). */
int         ltx_class(uint32_t cp);
/* Name of a class ("AL", "ID", ...), for test output. */
const char *ltx_class_name(int cls);

/* Break opportunities over a code point array.  `out` must hold n+1 entries:
 * out[i] is the opportunity BEFORE cp[i], and out[n] is the one at the end of
 * the text (always LTX_BRK_MANDATORY, UAX #14 LB3).  out[0] is always
 * LTX_BRK_PROHIBITED (LB2).  Returns n+1. */
int ltx_break_cp(const uint32_t *cp, int n, const struct ltx_lbopt *opt,
                 unsigned char *out);

/* Same over UTF-8.  `out` must hold len+1 entries and is indexed by BYTE
 * offset; positions inside a multi-byte sequence are LTX_BRK_PROHIBITED. */
int ltx_break_utf8(const char *s, int len, const struct ltx_lbopt *opt,
                   unsigned char *out);

/* ------------------------------------- layer 2: white-space processing ---- */

/* Expand the `white-space` shorthand into its two longhands. */
void ltx_white_space(int ws, int *collapse, int *wrap);

/* CSS Text 3 §4.1: white-space processing of one text run.
 *
 * Runs are processed one at a time but a collapse crosses run boundaries (the
 * space at the end of "<b>a </b>b" collapses with nothing after it and is still
 * one space), so `state` carries the phase between calls.  Zero it at the start
 * of an inline formatting context.
 *
 * Returns the number of bytes written to `out`, or -1 if `outmax` is too small.
 * Worst case output length is `len` -- collapsing never grows the text.
 *
 * A collapsible space at the END of the input is NOT written; it is left owed
 * in `state->pending_space` and appears only if a later run supplies something
 * for it to sit before.  That is not a shortcut, it is the rule: a collapsible
 * space at the end of a line is removed, and the end of the text is the end of
 * a line.  It is also what makes "<b>a </b><i> b</i>" come out as "a b" with
 * exactly one space rather than two or three.
 *
 * The segment-break transformation (§4.1.2) is applied here, including the
 * East-Asian rule: a segment break between two East Asian wide characters is
 * REMOVED, not turned into a space, because CJK source text is line-wrapped in
 * the HTML and a space per source line would be visible in the output. */
struct ltx_wsstate {
    unsigned char pending_space;  /* a collapsible space is owed to the output */
    unsigned char started;        /* something has been emitted -- while this is
                                   * 0 a leading collapsible space is dropped */
    unsigned char pending_break;  /* a segment break is owed, and its fate is
                                   * not decided until the next character is
                                   * known (see ea_segbreak) */
    unsigned char ea_segbreak;    /* CONFIGURATION, not state, and it must be
                                   * set before the first call.
                                   *
                                   * 0 (the default, and what Chrome, Firefox
                                   * and Safari all do): a collapsible segment
                                   * break becomes a space unless a U+200B sits
                                   * on either side of it.
                                   *
                                   * 1: additionally REMOVE the break when the
                                   * characters on both sides are East Asian
                                   * (width F/W/H) and neither is Hangul --
                                   * the rule CSS Text 3 §4.1.2 originally
                                   * specified.  It matters because CJK source
                                   * HTML is hard-wrapped and every source line
                                   * ending would otherwise show up as a real
                                   * space between two ideographs.  It is off
                                   * by default because no shipping browser
                                   * does it and a reftest would disagree. */
    uint32_t last_cp;             /* last emitted code point (segment-break
                                   * transformation looks at it) */
};
int ltx_collapse(const char *in, int len, int wsc, struct ltx_wsstate *state,
                 char *out, int outmax);

/* Trailing white-space at the end of a line "hangs": it is not measured and
 * does not participate in alignment.  Returns the byte length of `s` with the
 * hangable trailing spaces removed, given the collapse mode. */
int ltx_trim_end(const char *s, int len, int wsc);

/* ---------------------------------------- layer 3: text-transform -------- */

/* `at_word_start` is in/out for LTX_TT_CAPITALIZE, which must survive a run
 * boundary ("<b>hello</b> world" capitalises the w).  Returns bytes written or
 * -1 if `outmax` is too small; uppercasing can GROW the text (ß -> SS is not
 * implemented; ﬁ -> FI is not either -- see the note in the .c), so `outmax`
 * should be at least 3*len+1 to be safe.
 *
 * SCOPED OUT, LOUDLY: this is the language-independent mapping only.  Turkish
 * dotless i (i -> İ under lang="tr") and Lithuanian's retained dot need
 * SpecialCasing.txt conditioned on the element's language, and neither the DOM
 * nor `struct cstyle` carries a language today.  Greek final sigma IS handled
 * because it is conditioned on position, not on locale. */
int ltx_text_transform(const char *in, int len, int tt, int *at_word_start,
                       char *out, int outmax);

/* --------------------------------------------- layer 4/5: line building -- */

/* Everything about one styled run that affects where its text goes. */
struct ltx_style {
    int font_px;
    unsigned char bold, italic, mono;
    unsigned char wsc;             /* LTX_WSC_*   */
    unsigned char wrap;            /* LTX_WRAP_*  */
    unsigned char word_break;      /* LTX_WB_*    */
    unsigned char overflow_wrap;   /* LTX_OW_*    */
    unsigned char line_break;      /* LTX_LB_*    */
    unsigned char hyphens;         /* LTX_HY_*    */
    unsigned char text_transform;  /* LTX_TT_*    */
    int letter_spacing;            /* px, may be negative */
    int word_spacing;              /* px, may be negative */
    int tab_size;                  /* in `space` advances if tab_px == 0,
                                    * otherwise in px */
    unsigned char tab_px;
    int line_px;                   /* line-height in px; 0 = derive 1.25*font */
};

/* Advance of one styled string, in px.  This is the ONE place font advances and
 * CSS spacing meet, and it is a public entry point because the arithmetic is
 * exactly checkable without a font: with a stub measure of 10px per ASCII
 * character, "ab cd" at letter-spacing 2 and word-spacing 5 is exactly
 * 5*10 + 5*2 + 1*5.
 *
 * letter-spacing is added AFTER every typographic character unit including the
 * last, which is what every shipping browser does (the spec's wording says
 * "between", but a trailing-spacing-less implementation disagrees with Chrome,
 * Firefox and Safari on the width of every letter-spaced element).
 * word-spacing is added at every U+0020 only, per CSS Text 3 §8.2. */
struct ltx_env;                    /* below */
int ltx_measure_run(const struct ltx_env *env, const struct ltx_style *st,
                    const char *s, int len);

/* The host supplies the font.  `len` bytes of UTF-8, returns px. */
typedef int (*ltx_measure_fn)(void *ctx, const char *s, int len,
                              const struct ltx_style *st);

struct ltx_env {
    ltx_measure_fn measure;
    void *ctx;
    int avail;                     /* the measure (content width), px */
    unsigned char align;           /* LTX_ALIGN_*      */
    unsigned char align_last;      /* LTX_ALAST_*      */
    unsigned char justify;         /* LTX_TJ_*         */
    unsigned char rtl;             /* base direction is RTL: start = right.
                                    * Affects START/END alignment only; the
                                    * bidi REORDERING itself is c/lib/text's
                                    * (see the seam note in layout_text.c) */
    int indent;                    /* text-indent px on the first line */
    unsigned char indent_each;     /* text-indent: each-line */
    unsigned char indent_hanging;  /* text-indent: hanging  */
};

/* One input run: a contiguous span of text with one style. */
struct ltx_run {
    const char *text;
    int len;
    const struct ltx_style *style;
    void *user;                    /* opaque back-pointer (the DOM node) */
};

/* One placed piece of one run.  `off`/`len` index ltx_layout::text, the
 * processed (collapsed + transformed) text this module owns -- NOT the input,
 * which no longer corresponds byte for byte once white-space has collapsed. */
struct ltx_frag {
    int run;                       /* index into the input runs */
    int off, len;                  /* byte range in ltx_layout::text */
    int x, y, w, h;
    int line;
    void *user;
};

struct ltx_line {
    int frag0, nfrag;
    int x, y, w, h;
    unsigned char hard;            /* ended at a forced break or at the end */
    unsigned char hyphen;          /* the line ended at a SOFT HYPHEN, so the
                                    * painter must draw a visible '-' after the
                                    * last fragment.  U+00AD has no glyph of its
                                    * own -- if the caller ignores this flag the
                                    * word simply comes apart with no mark, and
                                    * `hyphens: manual` has done nothing
                                    * visible. */
};

struct ltx_layout {
    char *text;                    /* owned; the processed text */
    int text_len;
    struct ltx_frag *frags; int nfrag;
    struct ltx_line *lines; int nline;
    int width, height;             /* the widest line, and the total */
};

/* Lay `nrun` runs into lines of `env->avail` px.
 *
 * Returns 0 on success, -1 on allocation failure.  Free with ltx_layout_free.
 *
 * NOT DONE HERE, deliberately: floats (the available width is one number, not a
 * band -- layout.c owns floats and would pass a per-line width when this is
 * wired in), vertical alignment within the line box, and automatic
 * hyphenation.  `hyphens: auto` needs a per-language pattern dictionary
 * (Liang's algorithm plus ~30 KB per language); `hyphens: manual` IS
 * implemented -- U+00AD becomes a break opportunity and a hyphen is drawn.
 * text-wrap: balance/pretty parse and behave as `wrap`: both are defined as
 * "may consider more than one line at a time", and doing so honestly needs a
 * second layout pass whose cost has to be bounded per paragraph. */
int  ltx_layout_runs(const struct ltx_run *runs, int nrun,
                     const struct ltx_env *env, struct ltx_layout *out);
void ltx_layout_free(struct ltx_layout *l);

/* ======================================================================
 * THE SEAM: how this replaces layout.c's inline flow, when someone routes it
 *
 * Nothing here is wired in.  layout.c belongs to another line and three lines
 * cannot share one file, so this is written down instead of done.  Read it as
 * a description of the work, not as a claim that it is finished.
 *
 * WHAT IT REPLACES.  layout.c's `flow_text()` and the `struct iflow` pen it
 * drives.  Today flow_text does five jobs in one loop: white-space branching,
 * word splitting at ASCII spaces, measuring, emitting display-list items, and
 * advancing the pen.  ltx_layout_runs does the first four; the fifth stays in
 * layout.c because the pen is where floats live.
 *
 * THE ONE THING THAT DOES NOT FIT, and it is not small.  `struct iflow` carries
 * x0/x1 -- the CURRENT LINE's edges after the floats overlapping its band are
 * subtracted -- and re-derives them per line from the float list.  This module
 * takes ONE `avail` for the whole block.  Wiring it in therefore needs
 * ltx_layout_runs to ask for the width of each line as it starts it, not to be
 * told once:
 *
 *     int (*line_avail)(void *ctx, int y, int probe_h, int *x_out);
 *
 * added to struct ltx_env, defaulting to "always env->avail".  layout.c's
 * float_band() is exactly that function already.  Until that callback exists,
 * a block containing a float must keep using flow_text -- which is a real
 * limitation and not a detail, because a float is how half the web puts an
 * image beside a paragraph.
 *
 * THE REST OF THE WIRING, in order of how much of it there is:
 *   1. `struct cstyle` -> `struct ltx_style`.  cstyle currently carries
 *      white_space, text_align, line_px, font_px, bold/italic/mono and NOTHING
 *      else this module reads.  The properties it does not have yet are listed
 *      at the bottom of this comment; they belong to the CSSOM line, not here.
 *   2. Collect the inline children of a block into `struct ltx_run[]` --
 *      layout.c's flow_children walk, but appending runs instead of emitting.
 *      An inline-level replaced box (img, video, a form control) is NOT a text
 *      run; it needs an atomic-inline run kind this module does not have yet
 *      (an entry with a fixed width and no text), which is a small addition and
 *      an honest gap today.
 *   3. `struct ltx_frag` -> `struct item`.  One frag becomes one IT_TEXT with
 *      x/y/w/h copied straight across; `user` carries the DOM node.  The
 *      display list keeps its shape, so the painter and the hit test are
 *      untouched.
 *   4. Delete flow_text, the `sp()` word loop, and the break-anywhere fallback
 *      at the end of it.  Keep newline2's float-band re-derivation.
 *
 * BIDI.  It exists -- c/lib/text/bidi.c is a complete UAX #9 implementation,
 * measured against BidiTest.txt (CLAUDE.md's "no bidi/shaping" note is stale).
 * This module does not call it, and the join is already designed for from the
 * other side: bidi.h exposes `bidi_l1_line` with the comment "exposed because a
 * line breaker must re-apply it per line, on the sub-range it actually put on
 * the line".  That line breaker is this file.  The sequence, once the atomic
 * inline above exists, is:
 *
 *     bidi_resolve(paragraph)                once per inline context
 *     ltx_layout_runs(...)                   breaks in LOGICAL order (correct:
 *                                            UAX #14 is defined on logical
 *                                            order, not visual)
 *     per line: bidi_l1_line(sub-range) then bidi_reorder -> visual order,
 *               then assign x by walking the visual order instead of the
 *               logical one
 *
 * Only the last step touches this file: `emit_line` places fragments in logical
 * order today.  `bidi_is_trivial` skips all of it for pure-LTR text, which is
 * almost every line, so the cost lands only where it is needed.
 *
 * WHAT `struct cstyle` WOULD NEED (for whoever owns css_engine.c -- do not add
 * these from here):
 *     text-indent (px/%, and the each-line/hanging keywords)
 *     letter-spacing, word-spacing            (px, signed)
 *     tab-size                                (number or px)
 *     text-transform                          (5 values)
 *     word-break, overflow-wrap, line-break, hyphens
 *     text-align-last, text-justify
 *     white-space-collapse + text-wrap        (the CSS Text 4 longhands;
 *                                              cstyle has only the shorthand)
 *     direction                               (for START/END alignment)
 * Every one of them has a matching field in `struct ltx_style` or
 * `struct ltx_env` already, so the mapping is assignment, not translation.
 * ====================================================================== */

#endif /* LOGIT_LAYOUT_TEXT_H */
