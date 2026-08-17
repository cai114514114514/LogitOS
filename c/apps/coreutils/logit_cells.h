#ifndef LOGIT_CELLS_H
#define LOGIT_CELLS_H

/* THE CELL/BYTE RULE, in one place, because two binaries have to agree on it.
 *
 * A character grid has two coordinate systems and they are not the same
 * number the moment anything but ASCII is typed:
 *
 *   BYTES  what a C string is indexed by, what the shell's edit cursor `lcur`
 *          counts, what `strlen` returns.
 *   CELLS  what the terminal draws in: gui_text_mono advances by one `cell`
 *          per code point (two for a wide one) -- c/lib/text/shape.c:1208.
 *
 * They disagreed across a pipe. c/apps/coreutils/sh.c published `lcur`, a BYTE
 * index, in its RT_T_INPUT frame; c/apps/gui/terminal.c drew that number as a
 * CELL column. For ASCII the two are identical, which is why it went unnoticed
 * for as long as nothing but ASCII could reach either side -- and both sides
 * were fixed in the same milestone that let a non-ASCII byte through, so the
 * disagreement became reachable and this header became necessary in the same
 * change. The error is exactly the number of UTF-8 continuation bytes left of
 * the cursor, plus one per wide character: type three Chinese characters and
 * the caret sits nine columns right of the text (six continuation bytes, three
 * wide glyphs, nine cells of drift).
 *
 * So the conversion is written once, here, and both binaries include it.
 *
 * --------------------------------------------------------------------------
 * THE RULE
 *
 *   cells(codepoint) = 2 if East Asian Wide (W) or Fullwidth (F), else 1.
 *
 * and NOTHING is zero-width. Two deliberate deviations from POSIX wcwidth(),
 * each because this grid is not a POSIX terminal and the renderer is readable:
 *
 *   - A COMBINING MARK TAKES A CELL. wcwidth() gives U+0300..U+036F width 0.
 *     c/lib/text/shape.c's cell grid does not: it walks code points and
 *     advances one cell for each, with no zero-advance case (shape.c:1208-1219,
 *     `for (i..n) { ...; x += w; }`). Measured, not assumed -- shaping U+0300
 *     through the shipped mono font at 16px returns exactly one cell. A rule
 *     that said 0 here would put the caret one column left of where the
 *     terminal draws the mark, which is the bug this header exists to stop.
 *   - AMBIGUOUS (EAW `A`) IS NARROW, the wcwidth default outside a CJK locale.
 *
 * --------------------------------------------------------------------------
 * WHAT THIS RULE IS NOT, and this is the thing to know before trusting it
 * anywhere beyond the two call sites it was written for.
 *
 * It is a function of the CODE POINT. The renderer's is not. shape.c:1215 says
 *
 *     int w = (adv > cell * 3 / 2) ? cell * 2 : cell;
 *
 * -- two cells if THIS FONT's advance for THIS glyph exceeds one and a half
 * cells. That is a different question, and it gives a different answer twice:
 *
 *   1. A code point the shipped font does not contain falls back to .notdef,
 *      whose advance is narrow, so the renderer calls it ONE cell. The fonts on
 *      the disk are subsets (tools/mkfont.py), so most of the CJK block is in
 *      that state: U+4E02 measures 1 cell and U+4E03 measures 2, from the same
 *      block, on the same line, because one glyph survived subsetting and the
 *      other did not.
 *   2. A glyph that comes from the PROPORTIONAL fallback (ui.ttf, then DejaVu)
 *      can be wider than 1.5 cells while being an ordinary narrow letter:
 *      U+0416 CYRILLIC ZHE, U+00A9, U+00BC and hundreds more measure 2 cells
 *      at 16px in the shipped set.
 *
 * Neither is reproducible by a second binary that has no font, so neither can
 * be the shared rule -- and both are the RENDERER being wrong about what a
 * fixed grid is, not this header. It is written up as a finding rather than
 * chased here, because the fix is in c/lib/text/shape.c and c/lib/text is not
 * this file's to change. tests/unit/cells_test.c measures the size of the
 * disagreement against the real fonts and PRINTS it, so it is a number that
 * moves rather than a paragraph that rots.
 *
 * --------------------------------------------------------------------------
 * THE GATE (make test-cells) checks two things against references that are not
 * this header:
 *
 *   - the decoder against c/lib/text/utf8.c, the decoder the renderer itself
 *     uses, over every 1..4 byte sequence class including the malformed ones;
 *   - the width table against /usr/share/unicode/EastAsianWidth.txt, every one
 *     of the 1,114,112 code points, so the table is re-derived from the
 *     Unicode Character Database on every run rather than trusted because
 *     somebody typed it once.
 *
 * `make test-cells-negctl` compiles the same suite with -DCELLS_NEGATIVE_CONTROL,
 * which restores one-cell-per-BYTE (the bug), and REQUIRES it to fail.
 *
 * Header-only and freestanding on purpose: /bin/sh links crt0_cli and nothing
 * else, so there is no library for this to live in.
 */

/* ---------------------------------------------------------------- decode -- */

/* Decode the code point at `s[i]`, reading no further than `n` bytes total.
 * Returns the number of bytes consumed, always >= 1 so a caller cannot loop.
 *
 * The classification rules are c/lib/text/utf8.c's `utf8_next`, byte for byte:
 * the well-formed 1..4 byte forms are accepted, and a bad lead byte, a bad
 * continuation byte, an overlong form, a surrogate or a value past U+10FFFF
 * decodes to U+FFFD consuming ONE byte. The one difference is the bound: this
 * takes a length, because the shell's edit buffer is measured to `lcur` and a
 * cursor can legitimately sit in the middle of a sequence that has not finished
 * arriving. utf8_next relies on the terminating NUL to stop instead -- which is
 * the same behaviour on a NUL-terminated string, and that equivalence is what
 * the gate asserts rather than assumes. */
static inline int lc_decode(const char *s, int n, int i, unsigned *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (i < 0 || i >= n) { *cp = 0; return 1; }

    unsigned char c = p[i];
    if (c < 0x80) { *cp = c; return 1; }

    int k;
    unsigned v;
    if      ((c & 0xE0) == 0xC0) { k = 1; v = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { k = 2; v = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { k = 3; v = c & 0x07u; }
    else { *cp = 0xFFFD; return 1; }

    if (i + k >= n) { *cp = 0xFFFD; return 1; }        /* truncated: not a form */
    for (int j = 1; j <= k; j++) {
        if ((p[i + j] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (p[i + j] & 0x3Fu);
    }
    if (v > 0x10FFFFu || (v >= 0xD800u && v <= 0xDFFFu) ||
        (k == 1 && v < 0x80u) || (k == 2 && v < 0x800u) || (k == 3 && v < 0x10000u)) {
        *cp = 0xFFFD; return 1;
    }
    *cp = v;
    return k + 1;
}

/* ----------------------------------------------------------------- width -- */

/* East Asian Wide + Fullwidth, merged into 122 ranges. Derived mechanically
 * from EastAsianWidth.txt and re-checked against it by the gate on every run --
 * see the header comment. Sorted, disjoint, and never adjacent (adjacent
 * ranges were merged), which is what makes the binary search below exact. */
struct lc_range { unsigned lo, hi; };

static const struct lc_range lc_wide_ranges[] = {
    { 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A }, { 0x23E9, 0x23EC },
    { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 }, { 0x25FD, 0x25FE }, { 0x2614, 0x2615 },
    { 0x2630, 0x2637 }, { 0x2648, 0x2653 }, { 0x267F, 0x267F }, { 0x268A, 0x268F },
    { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 }, { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE },
    { 0x26C4, 0x26C5 }, { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA },
    { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA }, { 0x26FD, 0x26FD },
    { 0x2705, 0x2705 }, { 0x270A, 0x270B }, { 0x2728, 0x2728 }, { 0x274C, 0x274C },
    { 0x274E, 0x274E }, { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
    { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C }, { 0x2B50, 0x2B50 },
    { 0x2B55, 0x2B55 }, { 0x2E80, 0x2E99 }, { 0x2E9B, 0x2EF3 }, { 0x2F00, 0x2FD5 },
    { 0x2FF0, 0x303E }, { 0x3041, 0x3096 }, { 0x3099, 0x30FF }, { 0x3105, 0x312F },
    { 0x3131, 0x318E }, { 0x3190, 0x31E5 }, { 0x31EF, 0x321E }, { 0x3220, 0x3247 },
    { 0x3250, 0xA48C }, { 0xA490, 0xA4C6 }, { 0xA960, 0xA97C }, { 0xAC00, 0xD7A3 },
    { 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE52 }, { 0xFE54, 0xFE66 },
    { 0xFE68, 0xFE6B }, { 0xFF01, 0xFF60 }, { 0xFFE0, 0xFFE6 },
    { 0x16FE0, 0x16FE4 }, { 0x16FF0, 0x16FF1 }, { 0x17000, 0x187F7 },
    { 0x18800, 0x18CD5 }, { 0x18CFF, 0x18D08 }, { 0x1AFF0, 0x1AFF3 },
    { 0x1AFF5, 0x1AFFB }, { 0x1AFFD, 0x1AFFE }, { 0x1B000, 0x1B122 },
    { 0x1B132, 0x1B132 }, { 0x1B150, 0x1B152 }, { 0x1B155, 0x1B155 },
    { 0x1B164, 0x1B167 }, { 0x1B170, 0x1B2FB }, { 0x1D300, 0x1D356 },
    { 0x1D360, 0x1D376 }, { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF },
    { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A }, { 0x1F200, 0x1F202 },
    { 0x1F210, 0x1F23B }, { 0x1F240, 0x1F248 }, { 0x1F250, 0x1F251 },
    { 0x1F260, 0x1F265 }, { 0x1F300, 0x1F320 }, { 0x1F32D, 0x1F335 },
    { 0x1F337, 0x1F37C }, { 0x1F37E, 0x1F393 }, { 0x1F3A0, 0x1F3CA },
    { 0x1F3CF, 0x1F3D3 }, { 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 },
    { 0x1F3F8, 0x1F43E }, { 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC },
    { 0x1F4FF, 0x1F53D }, { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 },
    { 0x1F57A, 0x1F57A }, { 0x1F595, 0x1F596 }, { 0x1F5A4, 0x1F5A4 },
    { 0x1F5FB, 0x1F64F }, { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC },
    { 0x1F6D0, 0x1F6D2 }, { 0x1F6D5, 0x1F6D7 }, { 0x1F6DC, 0x1F6DF },
    { 0x1F6EB, 0x1F6EC }, { 0x1F6F4, 0x1F6FC }, { 0x1F7E0, 0x1F7EB },
    { 0x1F7F0, 0x1F7F0 }, { 0x1F90C, 0x1F93A }, { 0x1F93C, 0x1F945 },
    { 0x1F947, 0x1F9FF }, { 0x1FA70, 0x1FA7C }, { 0x1FA80, 0x1FA89 },
    { 0x1FA8F, 0x1FAC6 }, { 0x1FACE, 0x1FADC }, { 0x1FADF, 0x1FAE9 },
    { 0x1FAF0, 0x1FAF8 }, { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
};
#define LC_NWIDE ((int)(sizeof lc_wide_ranges / sizeof lc_wide_ranges[0]))

/* 1 if this code point occupies two cells. */
static inline int lc_wide(unsigned cp)
{
    if (cp < 0x1100u) return 0;                        /* the whole Latin fast path */
    int lo = 0, hi = LC_NWIDE - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (cp < lc_wide_ranges[mid].lo) hi = mid - 1;
        else if (cp > lc_wide_ranges[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

/* Cells occupied by one code point. Never 0 -- see the header comment. */
static inline int lc_cp_cells(unsigned cp) { return lc_wide(cp) ? 2 : 1; }

/* ------------------------------------------------------------ conversion -- */

/* Cells occupied by the first `nbytes` bytes of `s`. This is the whole point of
 * the header: a byte index in, the column the terminal will draw at out. */
static inline int lc_cells(const char *s, int nbytes)
{
#ifdef CELLS_NEGATIVE_CONTROL
    /* THE NEGATIVE CONTROL, and it reinstates the whole of the design this
     * header replaced rather than one function of it: one cell per BYTE, its
     * inverse the identity, and a cursor that steps one BYTE at a time. That is
     * exactly what /bin/sh and the Terminal did before this file existed, so a
     * suite that still passes with it compiled in is not testing the
     * conversion. See `make test-cells-negctl`. */
    (void)s;
    return nbytes < 0 ? 0 : nbytes;
#else
    if (nbytes <= 0) return 0;
    int cells = 0;
    for (int i = 0; i < nbytes; ) {
        unsigned cp;
        i += lc_decode(s, nbytes, i, &cp);
        cells += lc_cp_cells(cp);
    }
    return cells;
#endif
}

/* The inverse, and it is not a bijection: a wide character covers two columns
 * and only its first is a byte boundary. Returns the byte offset of the last
 * boundary at or before column `cell`, so landing in the right half of a wide
 * character yields that character's own start -- which is what a horizontal
 * scroll window wants (start drawing at a character, never at half of one). */
static inline int lc_bytes(const char *s, int nbytes, int cell)
{
#ifdef CELLS_NEGATIVE_CONTROL
    (void)s;
    return cell <= 0 ? 0 : (cell > nbytes ? nbytes : cell);
#else
    if (cell <= 0 || nbytes <= 0) return 0;
    int col = 0;
    for (int i = 0; i < nbytes; ) {
        unsigned cp;
        int adv = lc_decode(s, nbytes, i, &cp);
        int w = lc_cp_cells(cp);
        if (col + w > cell) return i;
        col += w;
        i += adv;
    }
    return nbytes;
#endif
}

/* ------------------------------------------------------------ boundaries -- */

/* Where a cursor at byte `i` moves back to: the start of the character that
 * CONTAINS `i` if `i` is not on a boundary, and the start of the character
 * BEFORE it if it is. One backspace, one left-arrow. A cursor that is not on a
 * boundary has no cell answer at all, so every edit that moves the cursor has
 * to move it by characters -- which is why these two live here beside the
 * conversion rather than in the shell. */
static inline int lc_prev(const char *s, int nbytes, int i)
{
#ifdef CELLS_NEGATIVE_CONTROL
    (void)s; (void)nbytes;
    return i <= 0 ? 0 : i - 1;
#else
    if (i <= 0) return 0;
    if (i > nbytes) i = nbytes;
    /* Walk forward from the start: the only way to be exact about a malformed
     * sequence, since "back up over continuation bytes" cannot tell a real
     * continuation byte from a stray one and would swallow the character before
     * it. The buffer is one edit line, so the cost does not matter. */
    for (int k = 0; k < i; ) {
        unsigned cp;
        int adv = lc_decode(s, nbytes, k, &cp);
        if (k + adv >= i) return k;
        k += adv;
    }
    return 0;
#endif
}

/* The byte index of the boundary after `i`. */
static inline int lc_next(const char *s, int nbytes, int i)
{
#ifdef CELLS_NEGATIVE_CONTROL
    (void)s;
    return i < 0 ? 0 : (i >= nbytes ? nbytes : i + 1);
#else
    if (i < 0) i = 0;
    if (i >= nbytes) return nbytes;
    unsigned cp;
    int adv = lc_decode(s, nbytes, i, &cp);
    return i + adv > nbytes ? nbytes : i + adv;
#endif
}

#endif /* LOGIT_CELLS_H */
