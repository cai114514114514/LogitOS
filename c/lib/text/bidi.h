#ifndef LOGIT_BIDI_H
#define LOGIT_BIDI_H

#include <stdint.h>

/* The Unicode Bidirectional Algorithm (UAX #9).
 *
 * Complete: the explicit phase X1-X10 (embeddings, overrides AND isolates),
 * isolating run sequences (BD13), the weak/neutral/implicit resolutions
 * W1-W7, N0 (paired brackets), N1-N2, I1-I2, and the reordering L1-L2.
 * Not here: L3 (combining-mark reordering, a rendering matter the shaper
 * handles) and L4 (mirroring is offered separately as bidi_mirror_cp, because
 * whether to apply it depends on whether the font has the mirrored glyph).
 *
 * Conformance is measured, not asserted: tests/unit/bidi_test.c runs the
 * Unicode BidiTest.txt and BidiCharacterTest.txt corpora.
 *
 * Zero allocation and zero global state. Every entry point writes into
 * caller-supplied arrays, so this is callable from the kernel and from ring 3.
 */

/* Bidi_Class, in the order tests/unit/bidi_gen.py emits. */
enum {
    BIDI_L = 0, BIDI_R, BIDI_AL, BIDI_EN, BIDI_ES, BIDI_ET, BIDI_AN,
    BIDI_CS, BIDI_NSM, BIDI_BN, BIDI_B, BIDI_S, BIDI_WS, BIDI_ON,
    BIDI_LRE, BIDI_LRO, BIDI_RLE, BIDI_RLO, BIDI_PDF,
    BIDI_LRI, BIDI_RLI, BIDI_FSI, BIDI_PDI,
    BIDI_NCLASS
};

/* Paragraph direction requested by the caller (HTML `dir`, CSS `direction`). */
enum { BIDI_DIR_AUTO = 0, BIDI_DIR_LTR = 1, BIDI_DIR_RTL = 2 };

/* UAX #9 caps embedding depth at 125; the stack needs one more slot. */
#define BIDI_MAX_DEPTH 125

/* Bidi_Class of a code point. */
int bidi_class(uint32_t cp);

/* Mirrored code point (L4 / BD16), or `cp` itself when it has no mirror. */
uint32_t bidi_mirror_cp(uint32_t cp);

/* P2/P3: the paragraph embedding level implied by the text (0 or 1). Honours
 * isolate initiators by skipping their contents, as P2 requires. */
int bidi_paragraph_level(const uint32_t *cps, int n);

/* Bytes of scratch bidi_resolve needs for an n-character paragraph. Linear in
 * n (26n + slack). Exposed so a caller can size a static buffer and decide what
 * to do about a paragraph that does not fit, rather than discovering it does
 * not fit halfway through. */
int bidi_scratch_size(int n);

/* Resolve embedding levels for one paragraph.
 *
 *   cps/n     the paragraph's code points
 *   dir       BIDI_DIR_* ; AUTO runs P2/P3
 *   levels    out, n bytes: the resolved level of each code point
 *   scratch   at least bidi_scratch_size(n) bytes, any alignment >= 4
 *   ret       the paragraph embedding level actually used, or -1 if n < 0 or
 *             the scratch is too small (levels is then untouched)
 *
 * Characters removed by X9 (the embedding/override formatting characters and
 * BN) take the level of the preceding character, which is what UAX #9 section
 * 5.2 "retaining explicit formatting characters" prescribes and what
 * BidiTest.txt's `x` entries allow. */
int bidi_resolve(const uint32_t *cps, int n, int dir, uint8_t *levels,
                 void *scratch, int scratchlen);

/* L1: reset separators and trailing whitespace to the paragraph level. Called
 * by bidi_resolve already; exposed because a line breaker must re-apply it per
 * line, on the sub-range it actually put on the line. */
void bidi_l1_line(const uint32_t *cps, int n, int para_level, uint8_t *levels);

/* L2: reorder one line's levels into visual order. `order` receives n logical
 * indices in left-to-right visual order. Levels of "removed by X9" characters
 * must already have been filled in (bidi_resolve does that). */
void bidi_reorder(const uint8_t *levels, int n, int *order);

/* Convenience: does this paragraph need any bidi work at all? Returns 0 when
 * every character is L/EN/ES/ET/CS/WS/ON/B/S and the paragraph level is 0, i.e.
 * when logical order is already visual order. The text layer uses this to skip
 * the whole machinery on the overwhelmingly common pure-LTR string. */
int bidi_is_trivial(const uint32_t *cps, int n);

#endif /* LOGIT_BIDI_H */
