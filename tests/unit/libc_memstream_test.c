/* fmemopen()/open_memstream() case list, compiled TWICE by the harness (see
 * tests/libc.mk's LIBC_HOST_INC pattern, the same one libc_fnmatch_test.c
 * uses): once against this library's own headers/implementation, once as an
 * ordinary host program against glibc's <stdio.h>. Byte-identical stdout
 * from both runs is the proof.
 *
 * Every assertion prints ONE line: an incrementing sequence number, a tab,
 * then either an integer (a return value, ftell(), feof()/ferror() as 0/1,
 * ...) or a bracketed, hex-escaped dump of buffer bytes (chk_buf). Named
 * macros (SEEK_SET/EOF/...) are used throughout, never raw integers -- each
 * build resolves them against its OWN header, so the comparison is of
 * BEHAVIOUR, not of two implementations happening to agree on a bit pattern.
 * `errno` comparisons use `errno == E*` (an int compare, never printf'd
 * through strerror()) for the same reason: the two libraries' error message
 * TEXT is not something either one promises to match, only the numeric
 * value POSIX assigns it.
 *
 * SCOPE, per this unit's brief: fmemopen() and open_memstream() only. popen()
 * cannot be host-diffed meaningfully -- its child is /bin/sh, and diffing it
 * here would run the HOST's /bin/sh, not LogitOS's, which proves nothing
 * about this library's popen.c. See the unit's `deviations` note instead of
 * a test here.
 *
 * WHAT IS DELIBERATELY NOT ASSERTED HERE, AND WHY (see memstream.c's much
 * longer file-top comment for the full reverse-engineering story):
 *
 *   - The exact BUFFER CONTENT of an fmemopen() write that fills the buffer
 *     to EXACTLY its capacity in one call. Real glibc silently sacrifices
 *     the last byte written (replacing it with the terminating NUL) while
 *     still reporting the write as fully successful; this library does the
 *     simpler, safe thing instead (keeps the caller's last byte, skips the
 *     terminator when there is no room for it) rather than resurface a
 *     genuine short-write count through fwrite()'s existing all-or-nothing
 *     contract, which every OTHER caller in the tree also relies on.
 *     SCENARIO 8 below deliberately stops one byte short of that boundary
 *     for its content checks, and checks ONLY the return value / ftell() /
 *     errno (which DO still match glibc) for the exact-fill call itself and
 *     the true-overflow call after it.
 *   - open_memstream()'s reported *sizep and buffer bytes strictly BETWEEN a
 *     "seek backward, write something shorter, fflush()" and the following
 *     fclose(). Real glibc updates *sizep at that fflush() but defers
 *     actually re-terminating the buffer until fclose() -- an internal
 *     timing artifact, not documented behaviour. SCENARIO 15 checks the
 *     shrink case only at its final, post-fclose() state, where both
 *     implementations agree.
 *   - Reading from an open_memstream() stream. The man page and this unit's
 *     brief both call it a write-only stream; glibc's cookie-based
 *     implementation happens not to enforce that (a read after a write
 *     succeeds), which is unspecified behaviour neither side has to match.
 *     This library refuses it (EBADF), which is the documented contract. */
#define _POSIX_C_SOURCE 200809L   /* fmemopen/open_memstream/popen are POSIX.1-2008;
                                    * plain -std=c11 hides them from glibc's own
                                    * <stdio.h> without this (verified: omitting it
                                    * makes the glibc build fail to even compile). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int SEQ = 0;

static void chk_int(long v) { printf("%d\t%ld\n", SEQ++, v); }

static void chk_buf(const void *buf, size_t n)
{
    printf("%d\t[", SEQ++);
    const unsigned char *p = (const unsigned char *)buf;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c >= 32 && c < 127 && c != '\\') putchar(c);
        else printf("\\x%02x", c);
    }
    printf("]\n");
}

int main(void)
{
    /* ==== SCENARIO 1: "w" mode, single fwrite with room to spare ==== */
    {
        char buf[8]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        chk_int(f != NULL);
        size_t n = fwrite("ab", 1, 2, f);
        chk_int((long)n);
        chk_int(ftell(f));
        fflush(f);
        chk_buf(buf, sizeof buf);           /* "ab\0ZZZZZ": NUL after, rest untouched */
        chk_int(ferror(f));
        fclose(f);
        chk_buf(buf, sizeof buf);           /* unchanged by close (already flushed) */
    }

    /* ==== SCENARIO 2: "w" mode, two fwrite calls accumulate ==== */
    {
        char buf[6]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        size_t n1 = fwrite("ab", 1, 2, f);
        size_t n2 = fwrite("cd", 1, 2, f);
        chk_int((long)n1); chk_int((long)n2);
        chk_int(ftell(f));
        fflush(f);
        chk_buf(buf, sizeof buf);           /* "abcd\0Z" */
        fclose(f);
    }

    /* ==== SCENARIO 3: "w+" -- write, rewind, read back (bounded by what was
     * actually written, not the full capacity) ==== */
    {
        char buf[8]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w+");
        fwrite("abc", 1, 3, f);
        rewind(f);
        char rb[8]; memset(rb, 'Q', sizeof rb);
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 3, not 8 */
        chk_int(feof(f));
        chk_buf(rb, sizeof rb);
        fclose(f);
    }

    /* ==== SCENARIO 4: "w+" -- fseek past what's been written (within
     * capacity), then write; the gap is NOT zero-filled, it retains
     * whatever the CALLER's buffer already held there (fmemopen has no way
     * to "erase" the caller's own memory) ==== */
    {
        char buf[8]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w+");
        fwrite("abc", 1, 3, f);
        int sr = fseek(f, 5, SEEK_SET);
        chk_int(sr);
        fwrite("XY", 1, 2, f);
        fflush(f);
        chk_buf(buf, sizeof buf);           /* "abc\0ZXY\0" -- index 4 stays 'Z' */
        rewind(f);
        char rb[8]; memset(rb, 'Q', sizeof rb);
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 7: bounded by the new high-water mark */
        chk_buf(rb, sizeof rb);
        fclose(f);
    }

    /* ==== SCENARIO 5: "a"/"a+" position at the first NUL byte ==== */
    {
        char buf[8] = "ab\0zzzz";           /* NUL at index 2 */
        FILE *f = fmemopen(buf, sizeof buf, "a");
        chk_int(ftell(f));                  /* 2 */
        fclose(f);
    }
    {
        char buf[8] = "ab\0zzzz";
        FILE *f = fmemopen(buf, sizeof buf, "a+");
        fwrite("XY", 1, 2, f);
        chk_int(ftell(f));                  /* 4: wrote at the append point, not at 0 */
        fflush(f);
        chk_buf(buf, sizeof buf);           /* "abXY\0zz\0" */
        rewind(f);
        char rb[8]; memset(rb, 'Q', sizeof rb);
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 4: bounded by the append high-water mark */
        chk_buf(rb, sizeof rb);
        fclose(f);
    }

    /* ==== SCENARIO 6: "a+" with no NUL anywhere -> initial position = size ==== */
    {
        char buf[4] = { 'a', 'b', 'c', 'd' };
        FILE *f = fmemopen(buf, sizeof buf, "a+");
        chk_int(ftell(f));                  /* 4 */
        fclose(f);
    }

    /* ==== SCENARIO 7: "a+" whose buffer starts with NUL -> position 0 ==== */
    {
        char buf[4] = { 0, 0, 0, 0 };
        FILE *f = fmemopen(buf, sizeof buf, "a+");
        chk_int(ftell(f));                  /* 0 */
        fclose(f);
    }

    /* ==== SCENARIO 8: "w" mode, single-byte UNBUFFERED writes (setbuf(NULL)
     * forces each one to commit immediately instead of staging in wbuf, so
     * ftell()/content are observed at the real capacity boundary, not after
     * a later flush) up to, then past, an exact-capacity fill. Content is
     * checked only where there is still room for the terminator -- see the
     * file-top comment for why the exact-fill call itself (pos 3 -> 4 ==
     * capacity) is checked by return value and ftell() only, not content. */
    {
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        setbuf(f, NULL);
        int r0 = fputc('0', f); chk_int(r0); chk_int(ftell(f));
        int r1 = fputc('1', f); chk_int(r1); chk_int(ftell(f));
        chk_buf(buf, sizeof buf);           /* "01\0Z": two bytes of capacity still free */
        int r2 = fputc('2', f); chk_int(r2); chk_int(ftell(f));
        chk_buf(buf, sizeof buf);           /* "012\0": one byte of capacity still free */
        int r3 = fputc('3', f);             /* exact-fill: pos goes 3 -> 4 == capacity */
        chk_int(r3 == '3');                 /* still reports success, like glibc */
        chk_int(ftell(f));                  /* 4, like glibc */
        errno = 0;
        int r4 = fputc('4', f);             /* true overflow: no capacity left at all */
        chk_int(r4);                        /* EOF */
        chk_int(errno == ENOSPC);
        chk_int(ferror(f));
        chk_int(ftell(f));                  /* still 4: an overflow does not move the position */
        fclose(f);
    }

    /* ==== SCENARIO 9: "r" mode -- read stops at `size`, not at an embedded
     * NUL, and the buffer is NOT NUL-terminated for the caller ==== */
    {
        char buf[6] = { 'a', 'b', 0, 'd', 'e', 'f' };
        FILE *f = fmemopen(buf, sizeof buf, "r");
        char rb[10]; memset(rb, 'Q', sizeof rb);
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 6, straight through the embedded NUL */
        chk_int(feof(f));
        chk_buf(rb, sizeof rb);             /* last 4 bytes still 'Q': not padded/terminated */
        chk_buf(buf, sizeof buf);           /* untouched: fmemopen("r") never writes to it */
        fclose(f);
    }

    /* ==== SCENARIO 10: "r+" -- no truncation; the WHOLE buffer is valid
     * content from the start (unlike "w+") ==== */
    {
        char buf[6] = { 'a', 'b', 'c', 'd', 'e', 'f' };
        FILE *f = fmemopen(buf, sizeof buf, "r+");
        char rb[8]; memset(rb, 'Q', sizeof rb);
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 6 */
        chk_buf(rb, sizeof rb);
        fclose(f);
    }

    /* ==== SCENARIO 11: writing to a read-only ("r") stream fails; reading
     * from a write-only ("w") stream fails. Both set ferror, not feof. ==== */
    {
        char buf[4] = { 'a', 'b', 'c', 'd' };
        FILE *f = fmemopen(buf, sizeof buf, "r");
        errno = 0;
        size_t n = fwrite("X", 1, 1, f);
        chk_int((long)n);                   /* 0 */
        chk_int(errno == EBADF);
        chk_int(ferror(f));
        fclose(f);
    }
    {
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        errno = 0;
        char rb[4];
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 0 */
        chk_int(errno == EBADF);
        chk_int(feof(f));                   /* 0: this is an error, not EOF */
        chk_int(ferror(f));
        fclose(f);
    }

    /* ==== SCENARIO 12: buf == NULL -- fmemopen allocates and OWNS it, and
     * the allocation is zero-initialised (matters for "a"/"a+"'s NUL scan) ==== */
    {
        FILE *f = fmemopen(NULL, 8, "w+");
        chk_int(f != NULL);
        fwrite("hi", 1, 2, f);
        rewind(f);
        char rb[8]; memset(rb, 'Q', sizeof rb);
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 2 */
        chk_buf(rb, sizeof rb);
        fclose(f);                          /* must not crash: this frees its own buffer */
    }
    {
        FILE *f = fmemopen(NULL, 8, "a+");   /* zero-filled -> first byte is already NUL */
        chk_int(ftell(f));                  /* 0 */
        fclose(f);
    }

    /* ==== SCENARIO 13: size == 0 (legal since glibc 2.22: an already-EOF
     * stream, not an error) ==== */
    {
        char buf[1];
        FILE *f = fmemopen(buf, 0, "r");
        chk_int(f != NULL);
        char rb[4];
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 0 */
        chk_int(feof(f));
        fclose(f);
    }
    {
        char buf[1];
        FILE *f = fmemopen(buf, 0, "w");
        chk_int(f != NULL);
        setbuf(f, NULL);                    /* force the write to commit immediately,
                                              * instead of merely staging successfully
                                              * in an internal buffer bigger than the
                                              * zero-byte target it will never reach */
        errno = 0;
        int r = fputc('X', f);
        chk_int(r);                         /* EOF: zero capacity */
        chk_int(errno == ENOSPC);
        fclose(f);
    }

    /* ==== SCENARIO 14: fseek/ftell edge cases ==== */
    {
        /* SEEK_END is relative to the high-water mark, not the raw capacity */
        char buf[8]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w+");
        fwrite("abc", 1, 3, f);
        int r = fseek(f, 0, SEEK_END);
        chk_int(r);
        chk_int(ftell(f));                  /* 3, not 8 */
        fclose(f);
    }
    {
        /* SEEK_SET beyond capacity fails; position is unchanged */
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        errno = 0;
        int r = fseek(f, 10, SEEK_SET);
        chk_int(r);                         /* -1 */
        chk_int(errno == EINVAL);
        chk_int(ftell(f));                  /* 0: unchanged */
        fclose(f);
    }
    {
        /* SEEK_SET within capacity but beyond the high-water mark is fine */
        char buf[8]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w+");
        int r = fseek(f, 6, SEEK_SET);
        chk_int(r);                         /* 0 */
        chk_int(ftell(f));                  /* 6 */
        fclose(f);
    }
    {
        /* ftell() reflects a pending, not-yet-flushed write immediately */
        char buf[8]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        fwrite("abcd", 1, 4, f);
        chk_int(ftell(f));                  /* 4, before any fflush() */
        fclose(f);
    }

    /* ==== SCENARIO 15: open_memstream -- basic grow + fflush publishes,
     * NUL not counted in *sizep ==== */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        chk_int(f != NULL);
        size_t n = fwrite("hello", 1, 5, f);
        chk_int((long)n);
        int fr = fflush(f);
        chk_int(fr);
        chk_int((long)sz);                  /* 5 */
        chk_buf(bp, sz + 1);                /* "hello\0" */
        size_t n2 = fwrite(" world", 1, 6, f);
        chk_int((long)n2);
        fclose(f);
        chk_int((long)sz);                  /* 11 */
        chk_buf(bp, sz + 1);                /* "hello world\0" */
        free(bp);
    }

    /* ==== SCENARIO 16: open_memstream -- fflush() with nothing written yet
     * still allocates and publishes an empty, NUL-terminated buffer ==== */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        int fr = fflush(f);
        chk_int(fr);
        chk_int(bp != NULL);
        chk_int((long)sz);                  /* 0 */
        chk_buf(bp, 1);                     /* just the NUL */
        fclose(f);
        free(bp);
    }

    /* ==== SCENARIO 17: open_memstream -- seeking past the end zero-fills
     * the gap (the opposite of fmemopen's behaviour: this buffer is OWNED
     * by the stream, not the caller's, so there is something safe to zero) ==== */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        fwrite("ab", 1, 2, f);
        fseek(f, 5, SEEK_SET);
        fwrite("XY", 1, 2, f);
        fflush(f);
        chk_int((long)sz);                  /* 7 */
        chk_buf(bp, sz);                    /* "ab\0\0\0XY" */
        fclose(f);
        free(bp);
    }

    /* ==== SCENARIO 18: open_memstream -- shrink via seek-back-and-write,
     * checked only at its final (post-fclose) state (see the file-top
     * comment for why not sooner) ==== */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        fwrite("hello world", 1, 11, f);
        fflush(f);
        chk_int((long)sz);                  /* 11 */
        fseek(f, 0, SEEK_SET);
        fwrite("HI", 1, 2, f);
        fclose(f);
        chk_int((long)sz);                  /* 2 */
        chk_buf(bp, sz + 1);                /* "HI\0" */
        free(bp);
    }

    /* ==== SCENARIO 19: open_memstream -- multiple small writes accumulate
     * across an internal buffer boundary-sized amount of data ==== */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        for (int i = 0; i < 50; i++) fputc('a' + (i % 26), f);
        fclose(f);
        chk_int((long)sz);                  /* 50 */
        chk_buf(bp, sz);
        free(bp);
    }

    /* ==== SCENARIO 20: "a" mode (write-only append, no '+') refuses reads
     * the same way plain "w" does -- F_READ was never set ==== */
    {
        char buf[8] = "ab\0zzzz";
        FILE *f = fmemopen(buf, sizeof buf, "a");
        errno = 0;
        char rb[4];
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 0 */
        chk_int(errno == EBADF);
        chk_int(ferror(f));
        fclose(f);
    }

    /* ==== SCENARIO 21: fgetc()/ungetc() over an fmemopen("r") stream --
     * ungetc() must be visible to the very next read, and ftell() must
     * account for it (mirrors the same accounting this library already does
     * for a real fd, now exercised over the mem backend) ==== */
    {
        char buf[4] = { 'a', 'b', 'c', 'd' };
        FILE *f = fmemopen(buf, sizeof buf, "r");
        int c0 = fgetc(f); chk_int(c0);     /* 'a' */
        int c1 = fgetc(f); chk_int(c1);     /* 'b' */
        chk_int(ftell(f));                  /* 2 */
        int ur = ungetc(c1, f); chk_int(ur);
        chk_int(ftell(f));                  /* 1: pushed back */
        int c2 = fgetc(f); chk_int(c2);     /* 'b' again */
        int c3 = fgetc(f); chk_int(c3);     /* 'c' */
        int c4 = fgetc(f); chk_int(c4);     /* 'd' */
        int c5 = fgetc(f); chk_int(c5);     /* EOF: exactly `size` bytes served */
        chk_int(feof(f));
        fclose(f);
    }

    /* ==== ADVERSARIAL ADDITIONS below (verification pass on top of the
     * implementer's 21 scenarios). Aimed at: mode-string parsing edge cases,
     * the BUFFERED write path (scenario 8 above only ever exercised the
     * unbuffered/setbuf(NULL) path -- fput_raw's staging loop is a genuinely
     * different code path and untested until now), errno-on-success, and
     * whether a TRUE overflow (not an exact-capacity fill) reports the same
     * return value as glibc when it happens inside one buffered flush. ==== */

    /* ==== SCENARIO 22: empty / garbage-first-char mode strings reject the
     * same way fopen()'s mode grammar would.
     *
     * fmemopen(buf, size, NULL) is DELIBERATELY NOT a case here: real glibc
     * (2.43, this host) SEGFAULTS on a NULL mode -- confirmed in isolation
     * (a 6-line standalone program calling only that one line crashes the
     * same way). Passing NULL as a "shall be a valid mode string" argument
     * is a caller contract violation with no defined behaviour, and glibc
     * does not guard against it here the way it guards, say, fopen()'s mode
     * elsewhere. This library's fmem_parse_mode() DOES check `!m` and
     * returns EINVAL instead of dereferencing -- strictly safer than glibc --
     * but that graceful handling has no glibc behaviour to diff against, so
     * it is asserted only by code inspection (memstream.c's
     * `if (!m || !*m) return -1;`), not by this harness. */
    {
        char buf[4];
        errno = 0;
        FILE *f = fmemopen(buf, sizeof buf, "");
        chk_int(f == NULL);
        chk_int(errno == EINVAL);
    }
    {
        char buf[4];
        errno = 0;
        FILE *f = fmemopen(buf, sizeof buf, "x");
        chk_int(f == NULL);
        chk_int(errno == EINVAL);
    }

    /* ==== SCENARIO 23: 'b' is accepted (and ignored) in either position
     * relative to '+' -- "r+b" and "rb+" must both grant read+write ==== */
    {
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "r+b");
        errno = 0;
        size_t n = fwrite("Q", 1, 1, f);
        chk_int((long)n);                   /* 1: '+' still recognised after 'b' consumed */
        chk_int(errno);                     /* untouched (0) by a successful write */
        fclose(f);
    }
    {
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "rb+");
        size_t n = fwrite("Q", 1, 1, f);
        chk_int((long)n);                   /* 1: order of 'b' and '+' doesn't matter */
        fclose(f);
    }

    /* ==== SCENARIO 24: plain "rb" (no '+') still write-protected -- 'b'
     * must not be mistaken for anything that grants F_WRITE ==== */
    {
        char buf[4] = { 'a', 'b', 'c', 'd' };
        FILE *f = fmemopen(buf, sizeof buf, "rb");
        errno = 0;
        size_t n = fwrite("Q", 1, 1, f);
        chk_int((long)n);                   /* 0 */
        chk_int(errno == EBADF);
        fclose(f);
    }

    /* ==== SCENARIO 25: buf==NULL AND size==0 together (the two "relax the
     * rules" cases stacked) -- capacity is still 0 even though a 1-byte
     * scratch allocation backs it internally ==== */
    {
        FILE *f = fmemopen(NULL, 0, "r");
        chk_int(f != NULL);
        char rb[2];
        size_t n = fread(rb, 1, sizeof rb, f);
        chk_int((long)n);                   /* 0 */
        chk_int(feof(f));
        fclose(f);
    }
    {
        FILE *f = fmemopen(NULL, 0, "a+");
        chk_int(ftell(f));                  /* 0: nothing to scan for a NUL in */
        setbuf(f, NULL);
        errno = 0;
        int r = fputc('X', f);
        chk_int(r);                         /* EOF: zero capacity, even though buf!=NULL now */
        chk_int(errno == ENOSPC);
        fclose(f);
    }

    /* ==== SCENARIO 26: "r+" mid-buffer overwrite does not shrink mmax --
     * unlike "w+", mmax for r/r+ starts at the FULL size, not 0, so writing
     * over only the first two bytes must not truncate what a subsequent read
     * sees ==== */
    {
        char buf[6] = { 'a', 'b', 'c', 'd', 'e', 'f' };
        FILE *f = fmemopen(buf, sizeof buf, "r+");
        size_t n = fwrite("XY", 1, 2, f);
        chk_int((long)n);
        rewind(f);
        char rb[8]; memset(rb, 'Q', sizeof rb);
        size_t n2 = fread(rb, 1, sizeof rb, f);
        chk_int((long)n2);                  /* 6: still the whole buffer */
        chk_buf(rb, sizeof rb);             /* "XYcdef" + 2x 'Q' padding */
        fclose(f);
    }

    /* ==== SCENARIO 27: BUFFERED (default _IOFBF, no setbuf) write that
     * exactly fills capacity in ONE eventual flush -- scenario 8 above only
     * tested this through setbuf(NULL) single-byte commits; fput_raw's
     * staging loop is a different code path (bytes sit in wbuf and only
     * reach the mem backend at fflush/fclose) and was untested until now.
     * Return value / ftell() / errno must still agree with glibc (only
     * buffer CONTENT is the documented, excluded divergence -- see the
     * file-top comment) ==== */
    {
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");     /* fully buffered by default */
        errno = 0;
        size_t n = fwrite("wxyz", 1, 4, f);           /* exactly capacity, staged in wbuf */
        chk_int((long)n);                             /* 4: fwrite itself just staged it */
        int fr = fflush(f);                           /* THIS is where the exact-fill commit happens */
        chk_int(fr);                                  /* 0: glibc reports this flush as success too */
        chk_int(errno);                                /* untouched by a successful flush */
        chk_int(ftell(f));                             /* 4 */
        chk_int(ferror(f));                            /* 0 */
        fclose(f);
    }

    /* ==== SCENARIO 28: BUFFERED write that overflows capacity WITHIN ONE
     * flush (5 bytes into a 4-byte buffer, staged together in wbuf so the
     * mem backend sees a single n=5 commit request) -- the return-value
     * question a diff can answer that code review cannot: does glibc's
     * fwrite() report a SHORT item count (reflecting the 4 bytes that did
     * land) or 0 (all-or-nothing, this library's choice per its own
     * documented all-or-nothing fput_raw()/fwrite() contract)? ==== */
    {
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        errno = 0;
        size_t n = fwrite("wxyzQ", 1, 5, f);          /* stays buffered: 5 < BUFSIZ */
        int fr = fflush(f);                           /* overflow surfaces here */
        chk_int((long)n);                             /* fwrite()'s own return: glibc buffers
                                                        * first too, so this should be 5
                                                        * regardless of what the eventual
                                                        * flush reports */
        chk_int(fr);                                  /* EOF from the flush */
        chk_int(errno == ENOSPC);
        chk_int(ferror(f));
        fclose(f);
    }

    /* ==== SCENARIO 29: fseek() boundary arithmetic -- negative SEEK_CUR
     * past the start fails and leaves the position untouched; SEEK_END with
     * a negative offset lands correctly relative to the high-water mark ==== */
    {
        char buf[4]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w");
        errno = 0;
        int r = fseek(f, -1, SEEK_CUR);               /* position is 0; -1 would go negative */
        chk_int(r);                                    /* -1 */
        chk_int(errno == EINVAL);
        chk_int(ftell(f));                              /* 0: unchanged */
        fclose(f);
    }
    {
        char buf[8]; memset(buf, 'Z', sizeof buf);
        FILE *f = fmemopen(buf, sizeof buf, "w+");
        fwrite("abc", 1, 3, f);                          /* mmax = 3 */
        int r = fseek(f, -1, SEEK_END);
        chk_int(r);                                       /* 0 */
        chk_int(ftell(f));                                 /* 2 */
        fclose(f);
    }

    /* ==== SCENARIO 30: open_memstream -- NULL bufp/sizep.
     *
     * NOT diffed as a reject-with-EINVAL case, on direct evidence rather than
     * assumption: an isolated probe against real glibc (three lines, one per
     * combination of NULL bufp/sizep/both) shows glibc does NOT validate
     * either pointer at open time at all -- all three calls return a live,
     * non-NULL FILE* with errno left at 0. glibc defers any use of *bufp/
     * *sizep to the first fflush()/fclose(), so nothing crashes here only
     * because this test never flushes these particular streams; POSIX
     * documents no required behaviour for this misuse either way. This
     * library's memstream.c chose to validate up front instead
     * (`if (!bufp || !sizep) { errno = EINVAL; return NULL; }` in
     * open_memstream()) -- a strictly safer, but NOT glibc-matching, choice
     * (parallel to fmemopen()'s NULL-mode note above), so it is verified by
     * reading that line, not by a diff. What IS diffed here is that the
     * library is still in a normal, working state for the NEXT,
     * legitimately-argumented caller afterwards -- i.e. those rejections (or,
     * on any future change, non-rejections) leave no bad global state
     * behind. */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        chk_int(f != NULL);
        fwrite("ok", 1, 2, f);
        fclose(f);
        chk_int((long)sz);
        chk_buf(bp, sz + 1);
        free(bp);
    }

    /* ==== SCENARIO 31: open_memstream -- a big single write (forces several
     * doublings of the internal capacity in one call: 16 -> 32 -> ... ->
     * >=1001) and a combined seek-forward-gap + big write (grow and
     * zero-fill interacting in the same commit) ==== */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        char pattern[1000];
        for (int i = 0; i < 1000; i++) pattern[i] = (char)('A' + (i % 26));
        size_t n = fwrite(pattern, 1, sizeof pattern, f);
        chk_int((long)n);                                  /* 1000 */
        fclose(f);
        chk_int((long)sz);                                  /* 1000 */
        chk_int(memcmp(bp, pattern, sizeof pattern) == 0);
        chk_int((unsigned char)bp[1000]);                    /* NUL terminator */
        free(bp);
    }
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        fwrite("ab", 1, 2, f);
        fseek(f, 1000, SEEK_SET);
        fwrite("Z", 1, 1, f);
        fclose(f);
        chk_int((long)sz);                                    /* 1001 */
        chk_int(bp[0]); chk_int(bp[1]);                        /* 'a','b' */
        chk_int(bp[2]);                                         /* 0: start of the zero-filled gap */
        chk_int(bp[999]);                                       /* 0: end of the gap */
        chk_int(bp[1000]);                                      /* 'Z' */
        free(bp);
    }

    /* ==== SCENARIO 32: open_memstream -- fclose() with NOTHING ever written
     * and no explicit fflush() still publishes an allocated, NUL-terminated
     * empty buffer (fclose() must reach the same do_fflush_one() path as an
     * explicit fflush(), not just drain wbuf) ==== */
    {
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        fclose(f);
        chk_int(bp != NULL);
        chk_int((long)sz);                  /* 0 */
        chk_buf(bp, 1);                     /* just the NUL */
        free(bp);
    }

    /* ==== SCENARIO 33: errno is left completely untouched by successful
     * operations across the whole constructor/read/write/seek/close set --
     * catches a stray `errno = ...` on a success path (this library's own
     * stated rule: only a genuine failure sets errno) ==== */
    {
        char buf[8]; memset(buf, 'Z', sizeof buf);
        errno = 0;
        FILE *f = fmemopen(buf, sizeof buf, "w+");
        chk_int(errno);                     /* 0: opening must not touch errno */
        errno = 0;
        fwrite("abc", 1, 3, f);
        chk_int(errno);                     /* 0 */
        errno = 0;
        fseek(f, 0, SEEK_SET);
        chk_int(errno);                     /* 0 */
        errno = 0;
        char rb[3];
        fread(rb, 1, 3, f);
        chk_int(errno);                     /* 0 */
        errno = 0;
        int cr = fclose(f);
        chk_int(cr);
        chk_int(errno);                     /* 0 */
    }
    {
        errno = 0;
        char *bp; size_t sz;
        FILE *f = open_memstream(&bp, &sz);
        chk_int(errno);                     /* 0 */
        errno = 0;
        fwrite("hi", 1, 2, f);
        chk_int(errno);                     /* 0 */
        errno = 0;
        fclose(f);
        chk_int(errno);                     /* 0 */
        free(bp);
    }

    /* ==== SCENARIO 34: fread() with size==0 or nmemb==0 on a fmemopen
     * stream is a no-op that must not disturb position or hit EOF -- glibc
     * short-circuits this before ever touching the backend ==== */
    {
        char buf[4] = { 'a', 'b', 'c', 'd' };
        FILE *f = fmemopen(buf, sizeof buf, "r");
        char rb[4];
        size_t n1 = fread(rb, 0, 4, f);
        chk_int((long)n1);                  /* 0 */
        size_t n2 = fread(rb, 4, 0, f);
        chk_int((long)n2);                  /* 0 */
        chk_int(ftell(f));                  /* 0: neither call moved the position */
        chk_int(feof(f));                   /* 0 */
        size_t n3 = fread(rb, 1, 4, f);      /* a real read still works afterwards */
        chk_int((long)n3);                  /* 4 */
        fclose(f);
    }

    return 0;
}
