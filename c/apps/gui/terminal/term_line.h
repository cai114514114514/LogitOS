#ifndef TERM_LINE_H
#define TERM_LINE_H

/* How the Terminal builds a scrollback line out of a byte stream, pulled out of
 * terminal.c so a host gate can drive it (make test-term-wrap).
 *
 * THE BUG THIS REPLACES wrapped on BYTES against a width counted in CELLS:
 *
 *     if (l->len >= cols || l->len >= MAXCOLS) wrap;
 *
 * `cols` is how many cells fit across the window; `l->len` is how many bytes
 * the line holds. For ASCII the two are the same number. A Chinese character
 * is three bytes and two cells, so a line of them wrapped after cols/3
 * characters -- a third of every row left blank -- and because the test ran
 * per BYTE, the wrap could land between the bytes of one character and split
 * it across two lines, each half drawn as U+FFFD. Backspace removed one byte
 * and left the same broken tail. Tabs advanced to a multiple of 8 BYTES.
 *
 * So the unit of every decision here is a CODE POINT and the measure is the
 * one logit_cells.h defines (the rule /bin/sh and the input line already
 * share). Three consequences:
 *
 *   1. Bytes are assembled into a whole UTF-8 sequence BEFORE anything is
 *      decided. A pipe read may end in the middle of one; the partial sequence
 *      waits in `struct tl_asm` for the next read rather than being judged
 *      half-arrived.
 *   2. A sequence that is interrupted (a new lead byte, ASCII, or a newline
 *      before its continuation bytes arrived) is emitted as the raw bytes it
 *      has, which lc_decode will read as one U+FFFD per byte -- one cell each.
 *      Nothing is dropped: the renderer shows exactly what was sent.
 *   3. A line has two ceilings, cells (the window) and bytes (the buffer), and
 *      a unit that would exceed EITHER starts a new line. The byte buffer is
 *      sized so the cell ceiling is always the one that binds for text a
 *      person would write (MAXCOLS cells of 3-byte CJK is 1.5x MAXCOLS bytes;
 *      the Terminal gives the buffer 2x).
 *
 * Header-only and freestanding, like logit_cells.h: no libc, no allocation. */

#include "logit_cells.h"

/* A UTF-8 sequence waiting for its continuation bytes. */
struct tl_asm {
    char b[4];
    int  n;        /* bytes held                        */
    int  need;     /* bytes the lead byte promised      */
};

/* One complete unit ready to place: either a well-formed sequence or the raw
 * bytes of an interrupted one. */
struct tl_unit {
    char b[4];
    int  n;
};

static inline int tl_lead_len(unsigned char c)
{
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;          /* ASCII, a stray continuation byte, or an invalid lead */
}

/* Hand back whatever is held, as one unit. Returns 1 if there was anything. */
static inline int tl_asm_flush(struct tl_asm *a, struct tl_unit *u)
{
    if (a->n == 0) return 0;
    for (int i = 0; i < a->n; i++) u->b[i] = a->b[i];
    u->n = a->n;
    a->n = a->need = 0;
    return 1;
}

/* Feed one byte (never a C0 control: the caller handles those). Writes up to
 * two units to `out` and returns how many: an interrupted sequence flushes
 * first, then the byte that interrupted it is considered on its own. */
static inline int tl_asm_feed(struct tl_asm *a, unsigned char c, struct tl_unit out[2])
{
    int k = 0;
    if (a->n > 0) {
        if ((c & 0xC0) == 0x80) {
            a->b[a->n++] = (char)c;
            if (a->n == a->need) k += tl_asm_flush(a, &out[k]);
            return k;
        }
        k += tl_asm_flush(a, &out[k]);          /* interrupted: emit it raw */
    }
    int len = tl_lead_len(c);
    if (len == 1) { out[k].b[0] = (char)c; out[k].n = 1; return k + 1; }
    a->b[0] = (char)c; a->n = 1; a->need = len;
    return k;
}

/* Cells a unit will occupy when drawn -- the same walk the line itself gets. */
static inline int tl_unit_cells(const struct tl_unit *u) { return lc_cells(u->b, u->n); }

/* Would `u` fit on a line that holds `len` bytes, given the two ceilings? */
static inline int tl_fits(const char *t, int len, int cols, int maxbytes,
                          const struct tl_unit *u)
{
#ifdef TERM_WRAP_NEGCTL
    /* THE OLD RULE, whole: one byte, one column. make test-term-wrap-negctl
     * builds the gate with this and requires it to fail. */
    (void)t;
    return len + u->n <= cols && len + u->n <= maxbytes;
#else
    return lc_cells(t, len) + tl_unit_cells(u) <= cols && len + u->n <= maxbytes;
#endif
}

/* Append `u` at `*len` (the caller has already checked tl_fits). */
static inline void tl_put(char *t, int *len, const struct tl_unit *u)
{
    for (int i = 0; i < u->n; i++) t[(*len)++] = u->b[i];
    t[*len] = 0;
}

/* Backspace: remove the last CODE POINT, not the last byte. */
static inline void tl_backspace(char *t, int *len)
{
    int i = *len;
    if (i <= 0) return;
    int start = i - 1;
    /* Walk back over at most three continuation bytes to a lead byte; if what
     * is there is not a whole sequence (a stray continuation byte), remove
     * just the one byte, which is what lc_decode counted it as. */
    while (start > 0 && start > i - 4 && ((unsigned char)t[start] & 0xC0) == 0x80) start--;
    unsigned cp;
    if (lc_decode(t, i, start, &cp) != i - start) start = i - 1;
    *len = start;
    t[start] = 0;
}

/* Tab: pad with spaces to the next multiple of 8 CELLS, stopping at either
 * ceiling. */
static inline void tl_tab(char *t, int *len, int cols, int maxbytes)
{
    int c = lc_cells(t, *len);
    int target = (c / 8 + 1) * 8;
    if (target > cols) target = cols;
    while (c < target && *len < maxbytes) { t[(*len)++] = ' '; c++; }
    t[*len] = 0;
}

#endif
