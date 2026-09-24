/* The Terminal's line builder (c/apps/gui/terminal/term_line.h), driven the way
 * terminal.c's put_char drives it: one byte at a time, wrapping a unit that
 * would not fit onto a new line.
 *
 * The property under test is the one the old code broke: a line is full when
 * its CELLS reach the window width, never earlier, and never in the middle of a
 * character. Every assertion below is a count a person could check by hand --
 * how many characters sit on a row, and whether each row decodes cleanly --
 * rather than a comparison against this file's own arithmetic.
 *
 * make test-term-wrap-negctl builds this with -DTERM_WRAP_NEGCTL, which puts
 * the old byte rule back inside tl_fits, and requires the suite to FAIL. */
#include <stdio.h>
#include <string.h>
#include "term_line.h"

#define MAXL 64
#define LBYTES 400

static char lines[MAXL][LBYTES + 1];
static int  lens[MAXL], nlines;
static struct tl_asm A;
static int fails, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static void reset(void)
{
    memset(lines, 0, sizeof lines); memset(lens, 0, sizeof lens);
    memset(&A, 0, sizeof A); nlines = 1;
}

/* terminal.c place_unit(), with the ring and streams stripped away. */
static void place(const struct tl_unit *u, int cols, int maxbytes)
{
    int i = nlines - 1;
    if (!tl_fits(lines[i], lens[i], cols, maxbytes, u)) { nlines++; i++; }
    tl_put(lines[i], &lens[i], u);
}

static void feed(const char *s, int cols, int maxbytes)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        struct tl_unit u[2];
        int k = tl_asm_feed(&A, *p, u);
        for (int j = 0; j < k; j++) place(&u[j], cols, maxbytes);
    }
}

static void flush(int cols, int maxbytes)
{
    struct tl_unit u;
    if (tl_asm_flush(&A, &u)) place(&u, cols, maxbytes);
}

/* A row decodes with no U+FFFD, i.e. no character was split by a wrap. */
static int clean(int i)
{
    for (int b = 0; b < lens[i]; ) {
        unsigned cp; b += lc_decode(lines[i], lens[i], b, &cp);
        if (cp == 0xFFFD) return 0;
    }
    return 1;
}

static int chars(int i)
{
    int n = 0;
    for (int b = 0; b < lens[i]; ) { unsigned cp; b += lc_decode(lines[i], lens[i], b, &cp); n++; }
    return n;
}

int main(void)
{
    char buf[1024], what[128];

    /* 1. A row of Chinese fills the WIDTH. 25 x U+4F60 at cols=20: rows of
     *    10, 10, 5 characters (20, 20, 10 cells). The byte rule gave rows of
     *    6 characters -- 18 bytes -- and split the 7th. */
    reset(); buf[0] = 0;
    for (int i = 0; i < 25; i++) strcat(buf, "\xE4\xBD\xA0");
    feed(buf, 20, LBYTES); flush(20, LBYTES);
    check(nlines == 3, "CJK: 25 wide chars at 20 cols make 3 rows");
    for (int i = 0; i < nlines; i++) {
        snprintf(what, sizeof what, "CJK: row %d decodes whole", i);
        check(clean(i), what);
    }
    check(chars(0) == 10 && lc_cells(lines[0], lens[0]) == 20, "CJK: row 0 holds 10 chars / 20 cells");
    check(chars(2) == 5, "CJK: last row holds the remaining 5");

    /* 2. Mixed: "a" + wide, repeated. 3 cells per pair; at cols=10 a row
     *    holds 3 pairs (9 cells) and the next "a" (10th cell), then the wide
     *    one would need cells 11-12, so it wraps -- never half-drawn. */
    reset(); buf[0] = 0;
    for (int i = 0; i < 8; i++) strcat(buf, "a\xE5\xA5\xBD");
    feed(buf, 10, LBYTES); flush(10, LBYTES);
    int over = 0, split = 0;
    for (int i = 0; i < nlines; i++) {
        if (lc_cells(lines[i], lens[i]) > 10) over++;
        if (!clean(i)) split++;
    }
    check(over == 0, "mixed: no row exceeds 10 cells");
    check(split == 0, "mixed: no character split across rows");
    check(lc_cells(lines[0], lens[0]) == 10, "mixed: row 0 is exactly full (10 cells)");

    /* 3. A sequence split across two pipe reads is assembled, not judged
     *    half-arrived: feed the three bytes of one character separately. */
    reset();
    feed("x\xE4", 20, LBYTES); feed("\xBD", 20, LBYTES); feed("\xA0y", 20, LBYTES); flush(20, LBYTES);
    check(nlines == 1 && lens[0] == 5 && clean(0), "split read: x + one char + y, whole");

    /* 4. An interrupted sequence is kept as raw bytes -- nothing dropped. */
    reset();
    feed("\xE4" "a", 20, LBYTES); flush(20, LBYTES);
    check(lens[0] == 2 && (unsigned char)lines[0][0] == 0xE4 && lines[0][1] == 'a',
          "interrupted: raw lead byte kept, then 'a'");

    /* 5. Backspace removes a CODE POINT. */
    reset();
    feed("a\xE4\xBD\xA0", 20, LBYTES); flush(20, LBYTES);
    tl_backspace(lines[0], &lens[0]);
    check(lens[0] == 1 && lines[0][0] == 'a', "backspace: removes the whole wide char");
    tl_backspace(lines[0], &lens[0]);
    check(lens[0] == 0, "backspace: then the ASCII one");

    /* 6. Tabs stop at multiples of 8 CELLS: a wide char (2 cells) then a tab
     *    pads 6 spaces, not 5 (which is what 8-minus-3-bytes gives). */
    reset();
    feed("\xE4\xBD\xA0", 20, LBYTES); flush(20, LBYTES);
    tl_tab(lines[0], &lens[0], 20, LBYTES);
    check(lc_cells(lines[0], lens[0]) == 8, "tab: lands on cell 8");

    /* 7. The byte ceiling binds too: 4-byte characters (2 cells each) with a
     *    12-byte buffer and a generous width fit three per row. */
    reset(); buf[0] = 0;
    for (int i = 0; i < 5; i++) strcat(buf, "\xF0\x9F\x98\x80");
    feed(buf, 100, 12); flush(100, 12);
    check(nlines == 2 && lens[0] == 12 && clean(0) && clean(1), "byte ceiling: wraps at 12 bytes, whole");

    printf("term-wrap: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
