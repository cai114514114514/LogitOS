/* fmemopen() / open_memstream(): FILE streams with no fd underneath them.
 *
 * Ported code assumes both exist -- fmemopen() to parse a buffer someone
 * already has in memory with the usual fscanf/fgets machinery instead of a
 * hand-rolled parser, open_memstream() to build one up with fprintf instead
 * of a hand-rolled growable-string buffer. Neither talks to the kernel at
 * all: everything below is pure bookkeeping over a malloc'd block, which is
 * exactly why stdio.c's ENTIRE existing buffering machinery (fput_raw,
 * flush_w, refill, fgetc, fread, fseek, ftell, ...) is reused unmodified --
 * see the small set of `f->flags & (F_MEM|F_MEMSTR)` branches stdio.c grew
 * for this. This file supplies only the actual "commit these bytes" /
 * "read up to this many bytes" primitives that those branches call, plus the
 * two public constructors.
 *
 * THIS FILE'S BEHAVIOUR WAS REVERSE-ENGINEERED FROM REAL GLIBC (2.43), NOT
 * FROM THE MAN PAGE ALONE, because the man page undersells how much glibc's
 * fmemopen() write path actually does. Concretely, discovered by probing:
 *
 *   1. Writing exactly to capacity (no byte left for the terminating NUL)
 *      does not fail -- but glibc's real fmemopen_write SACRIFICES the last
 *      byte actually written, silently replacing it with '\0' to keep the
 *      "NUL if there's room" promise, while still reporting the write as
 *      fully successful. fputc('d', f) on a 4-byte buffer already holding
 *      "abc" returns 'd' (success!) and yet buf[3] comes back '\0', not 'd'.
 *      THIS LIBRARY DOES NOT REPLICATE THAT: it is undocumented, it silently
 *      destroys a byte the caller was told was written, and replicating it
 *      exactly would mean changing fput_raw()/fwrite()'s return-value
 *      contract for every OTHER caller in the tree (they are all-or-nothing
 *      today; glibc's fmemopen needs a genuine short-write count). This
 *      implementation instead does the simple, safe, spec-literal thing: a
 *      NUL is written after the last byte ONLY if a byte is actually free,
 *      and a write that would exactly exhaust the buffer keeps the caller's
 *      last byte and just skips the terminator. Exact-capacity writes are
 *      therefore a deliberate, documented divergence from glibc -- see
 *      tests/unit/libc_memstream_test.c's comment at the top of its fmemopen
 *      section, and this unit's `deviations` note.
 *   2. "a"/"a+" writes always land at the high-water mark (mmax), even after
 *      an intervening fseek() elsewhere -- exactly like a real file opened
 *      O_APPEND. ftell() afterwards reports where the write landed, not
 *      wherever the stream was seeked to beforehand.
 *   3. Seeking a "w+"/"r+"/"a+" stream past its high-water mark and writing
 *      there does NOT zero-fill the gap -- fmemopen has no way to "erase"
 *      part of the CALLER's buffer, so whatever was already sitting in that
 *      memory (the caller's original contents, uninitialised heap for
 *      buf==NULL) stays until something explicitly overwrites it.
 *      open_memstream is the opposite -- it owns the buffer outright, and
 *      DOES zero-fill a seek-then-write gap (open_memstream(3) says so, and
 *      the exploration confirms it): see __libc_msmem_commit below.
 *   4. open_memstream()'s *sizep, once published by fflush()/fclose(), is
 *      the CURRENT POSITION at that moment, not the historical maximum --
 *      seek back into an 11-byte stream and overwrite the first 2 bytes, and
 *      *sizep reports 2 (not 11) after the next flush, even though bytes
 *      2..10 are still sitting in the malloc'd block. This is why mpos, not
 *      a separate high-water mark, is what gets published; mmax is kept
 *      SEPARATELY, only for SEEK_END's benefit (a real file's "end" does not
 *      shrink just because you overwrote its middle).
 *
 * WHAT WAS DELIBERATELY NOT CHASED: glibc's fflush() on an open_memstream(),
 * in the exact "seek backward then write something shorter" case, updates
 * *sizep immediately but defers actually writing the new (earlier) NUL
 * terminator until fclose() -- so a caller who inspects *bufp between that
 * fflush() and the eventual fclose() sees stale trailing bytes glibc itself
 * has not cleaned up yet. That is an internal timing artifact (open_memstream(3)
 * itself only promises these values "as of" a flush/close, and glibc's own
 * BUGS section for the sibling fmemopen() lists several multi-release fixes
 * to comparably obscure corners), not documented behaviour, and no two libc
 * implementations are likely to agree on it. This implementation always
 * places the terminator at the moment it publishes *sizep -- simpler, and
 * correct at every point a caller is actually told the values are valid.
 * tests/unit/libc_memstream_test.c inspects the shrink case only AFTER
 * fclose(), where glibc's own deferred write has long since happened and the
 * two implementations agree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "libc_internal.h"

/* ---------------------------------------------------------------------- */
/* fmemopen()                                                              */
/* ---------------------------------------------------------------------- */

/* fmemopen's mode grammar is smaller than fopen's (mode_flags() in stdio.c):
 * no O_CREAT/O_TRUNC-at-the-kernel, no 'e'. 'b' is accepted and ignored
 * everywhere in the string, matching glibc since "binary mode" was removed
 * in 2.22 (see the man page's HISTORY section) -- there is no reason for a
 * ported program written against modern glibc to see it treated specially. */
static int fmem_parse_mode(const char *m, int *out_flags, int *out_append, int *out_trunc)
{
    int lf, append = 0, trunc = 0;
    if (!m || !*m) return -1;
    switch (m[0]) {
    case 'r': lf = F_READ; break;
    case 'w': lf = F_WRITE; trunc = 1; break;
    case 'a': lf = F_WRITE; append = 1; break;
    default: return -1;
    }
    for (const char *p = m + 1; *p; p++)
        if (*p == '+') lf |= (F_READ | F_WRITE);
    *out_flags = lf; *out_append = append; *out_trunc = trunc;
    return 0;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
    int lf, append, trunc;
    if (fmem_parse_mode(mode, &lf, &append, &trunc) < 0) { errno = EINVAL; return NULL; }

    unsigned char *b = (unsigned char *)buf;
    int owns = 0;
    if (!b) {
        /* POSIX: buf==NULL allocates, and the caller has no way to get a
         * pointer to it except by reading *bufp back out later (or, really,
         * by using open_memstream() instead -- see its man page). glibc's
         * allocation for this case is zero-filled (it mmaps anonymous
         * pages); that matters here because "a"/"a+" mode positions at the
         * first NUL byte, and an unzeroed buffer would start at a garbage
         * offset. size==0 is legal (modern glibc: an empty, already-EOF
         * stream, not an error) so this must not treat a zero-byte request
         * as a malloc(0)-may-return-NULL failure. */
        b = size ? calloc(1, size) : malloc(1);
        if (!b) { errno = ENOMEM; return NULL; }
        owns = 1;
    }

    FILE *f = (FILE *)malloc(sizeof *f);
    if (!f) { if (owns) free(b); errno = ENOMEM; return NULL; }
    memset(f, 0, sizeof *f);
    f->fd = -1;                       /* no fd -- fileno(3) must fail (see stdio.c) */
    f->flags = lf | F_ALLOC | F_MEM | (owns ? F_MEMOWN : 0);
    f->bufmode = _IOFBF;              /* a real fmemopen() stream IS buffered by default */
    f->ungot = -1;
    f->popen_pid = -1;
    f->mbuf = b;
    f->mcap = size;

    if (trunc) {
        /* "w"/"w+": truncated immediately, not lazily at first flush --
         * glibc places the NUL in buf[0] as part of fmemopen() itself. */
        f->mpos = f->mmax = 0;
        if (size) f->mbuf[0] = '\0';
    } else if (append) {
        f->flags |= F_MEMAPPEND;
        size_t i = 0;
        while (i < size && b[i]) i++;   /* position: the first NUL, or `size` if none */
        f->mpos = f->mmax = i;
    } else {
        /* "r"/"r+": no truncation -- the whole buffer is valid content from
         * the start, exactly as if it were a pre-existing file of length
         * `size` that happens to live in memory. */
        f->mpos = 0;
        f->mmax = size;
    }

    __libc_stream_register(f);
    return f;
}

/* Called from stdio.c's backend_write_all(). See the file-top comment for
 * why this never sacrifices a data byte for the terminator the way glibc's
 * does, and why a short return (less than n) is exactly the "attempts to
 * write more than size bytes... result in an error" case from fmemopen(3) --
 * stdio.c's flush_w()/fput_raw() turn that into F_ERR the same way a short
 * wr() to a real fd already does.
 *
 * THE TERMINATOR IS GATED ON EXTENDING mmax, NOT ON "IS THERE ROOM" ALONE --
 * found by an adversarial host diff against real glibc (a "r+" stream: write
 * 2 bytes over the START of a 6-byte buffer that already held "abcdef").
 * glibc leaves buf[2] as the original 'c'; an EARLIER version of this
 * function, which stamped '\0' at the new position whenever `pos < mcap`
 * with no further condition, clobbered it to '\0' instead -- because that
 * position, 2, was already < mcap (there was "room"), even though the write
 * never went anywhere near the logical end of the stream. The documented
 * rule (fmemopen(3)) is narrower than "room": a NUL is appended only when a
 * write actually PUSHES THE HIGH-WATER MARK OUT, i.e. only in the same
 * circumstance that updates mmax below -- a fresh "w" buffer's every write
 * satisfies that (mmax starts at 0), which is why the bug's own symptom
 * never showed up against this file's original, boundary-only "w"/"a" test
 * scenarios and needed a mid-buffer "r+" overwrite to surface. */
long __libc_fmem_commit(FILE *f, const char *p, size_t n)
{
    if (!(f->flags & F_WRITE)) { errno = EBADF; return -1; }
    size_t pos = (f->flags & F_MEMAPPEND) ? f->mmax : f->mpos;
    if (pos >= f->mcap) { errno = ENOSPC; return -1; }   /* already exactly full */
    size_t avail = f->mcap - pos;
    size_t take = n < avail ? n : avail;
    if (take) memcpy(f->mbuf + pos, p, take);
    size_t old_mmax = f->mmax;
    pos += take;
    if (pos > old_mmax) {
        f->mmax = pos;
        if (pos < f->mcap) f->mbuf[pos] = '\0';   /* terminator: only past the old
                                                     * end, and only where there is
                                                     * room -- see the comment above */
    }
    f->mpos = pos;
    return (long)take;
}

/* Called from stdio.c's refill(). Bounded by mmax (the high-water mark), NOT
 * mcap -- for "r"/"r+" those are equal (see fmemopen() above), but for
 * "w+"/"a+" a read must stop at what has actually been written, the same way
 * reading a real file stops at its length rather than at the filesystem's
 * notion of free space. The bytes served are whatever is physically in
 * mbuf -- fmemopen(3) is explicit that a read-mode stream is NOT NUL-
 * terminated for the caller. */
int __libc_fmem_read(FILE *f, unsigned char *dst, int cap)
{
    if (!(f->flags & F_READ)) { errno = EBADF; return -1; }
    if (f->mpos >= f->mmax) return 0;             /* logical EOF */
    size_t avail = f->mmax - f->mpos;
    size_t take = avail < (size_t)cap ? avail : (size_t)cap;
    memcpy(dst, f->mbuf + f->mpos, take);
    f->mpos += take;
    return (int)take;
}

/* ---------------------------------------------------------------------- */
/* open_memstream()                                                        */
/* ---------------------------------------------------------------------- */

FILE *open_memstream(char **bufp, size_t *sizep)
{
    if (!bufp || !sizep) { errno = EINVAL; return NULL; }
    FILE *f = (FILE *)malloc(sizeof *f);
    if (!f) { errno = ENOMEM; return NULL; }
    memset(f, 0, sizeof *f);
    f->fd = -1;
    f->flags = F_WRITE | F_ALLOC | F_MEMSTR;
    f->bufmode = _IOFBF;
    f->ungot = -1;
    f->popen_pid = -1;
    f->ms_bufp = bufp;
    f->ms_sizep = sizep;
    /* Deliberately NOT publishing *bufp and *sizep here: "these values remain
     * valid only as long as the caller performs no further output" implies
     * they are not meaningful before the first successful fflush()/fclose()
     * either, and glibc does not set them at open time (real *bufp is
     * whatever garbage the caller passed in until then). */
    __libc_stream_register(f);
    return f;
}

/* Called from stdio.c's backend_write_all(). Unlike fmemopen(), this never
 * refuses a write -- "the buffer automatically grows as needed"
 * (open_memstream(3)) -- so the return is always n or -1/ENOMEM, never a
 * short count. */
long __libc_msmem_commit(FILE *f, const char *p, size_t n)
{
    size_t end = f->mpos + n;
    if (end + 1 > f->mcap) {                      /* +1: always keep room for the NUL */
        size_t ncap = f->mcap ? f->mcap : 16;
        while (ncap < end + 1) ncap *= 2;
        unsigned char *nb = (unsigned char *)realloc(f->mbuf, ncap);
        if (!nb) { errno = ENOMEM; return -1; }
        f->mbuf = nb; f->mcap = ncap;
    }
    if (f->mpos > f->mmax)                         /* seeked past the old end: the gap
                                                     * reads as zero, like a sparse file
                                                     * (open_memstream(3): "fills the
                                                     * intervening space with null
                                                     * characters") */
        memset(f->mbuf + f->mmax, 0, f->mpos - f->mmax);
    memcpy(f->mbuf + f->mpos, p, n);
    f->mpos = end;
    if (f->mpos > f->mmax) f->mmax = f->mpos;
    return (long)n;
}

/* Called from stdio.c's do_fflush_one() on every fflush()/fclose() of an
 * open_memstream() stream -- the only two moments C allows *ms_bufp and *ms_sizep
 * to change. *sizep is mpos (the CURRENT position), not mmax -- see the
 * file-top comment, point 4: writing a shorter string after seeking back
 * shrinks the reported size, even though the underlying allocation and its
 * old trailing bytes are untouched. */
int __libc_msmem_publish(FILE *f)
{
    if (f->mpos + 1 > f->mcap) {
        size_t ncap = f->mcap ? f->mcap : 16;
        while (ncap < f->mpos + 1) ncap *= 2;
        unsigned char *nb = (unsigned char *)realloc(f->mbuf, ncap);
        if (!nb) { f->flags |= F_ERR; errno = ENOMEM; return -1; }
        f->mbuf = nb; f->mcap = ncap;
    }
    f->mbuf[f->mpos] = '\0';
    if (f->ms_bufp)  *f->ms_bufp  = (char *)f->mbuf;
    if (f->ms_sizep) *f->ms_sizep = f->mpos;
    return 0;
}
