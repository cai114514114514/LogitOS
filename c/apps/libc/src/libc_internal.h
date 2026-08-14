#ifndef _LIBC_INTERNAL_H
#define _LIBC_INTERNAL_H
/* mini-libc internals shared between TUs. Nothing here is a public API; the
 * double-underscore prefix keeps these out of the namespace a C program may
 * legally use for its own identifiers. */

#include <stddef.h>
#include <stdio.h>          /* FILE (opaque typedef) -- struct _FILE below
                              * completes it for the TUs that need the real
                              * layout: stdio.c, memstream.c, popen.c. */

/* ---- dtoa.c: exact decimal <-> binary floating point --------------------
 *
 * Both directions are EXACT, not approximate: a double is a rational with a
 * power-of-two denominator, so its decimal expansion terminates (at most 767
 * significant digits) and can be computed with integer arithmetic alone. Every
 * other approach -- accumulating digits into a double and scaling by a power of
 * ten, or peeling digits off with repeated multiplication -- double-rounds, and
 * a libc that double-rounds is wrong in a way nothing reports. */

/* Parse a decimal or hexadecimal floating literal. `bits` is 64 for double and
 * 32 for float: strtof is NOT strtod-then-cast (that double-rounds and is wrong
 * for ~1 input in 2^29). Sets errno to ERANGE on overflow/underflow. */
double __libc_strtox(const char *s, char **end, int bits);

/* Render a finite v as decimal digits.
 *   mode 0 -> `ndig` significant digits   (%e, %g)
 *   mode 1 -> `ndig` digits after the point (%f)
 *   mode 2 -> the shortest digit string that round-trips is NOT provided; use
 *             mode 0 with 17 digits if you need one.
 * Digits (ASCII '0'..'9', no sign, no point, no trailing zeros) go to buf;
 * *decpt receives the exponent E with value = 0.DDDD x 10^E. Returns the digit
 * count, which may be 0 (the value rounded to nothing, i.e. zero at that
 * precision). buf must hold at least __LIBC_DTOA_MAX bytes. */
#define __LIBC_DTOA_MAX 800
int __libc_dtoa(double v, int mode, int ndig, char *buf, int *decpt);

/* ---- stdio.c: stream registry ------------------------------------------ */
void __libc_flush_all(void);      /* called from exit() */

/* ---- printf/scanf cores shared by the narrow and wide entry points ------ */
struct __printf_sink {
    void (*put)(struct __printf_sink *, const char *, size_t);
    void *ctx;
    size_t count;                 /* characters "produced", ignoring truncation */
};
int __libc_vformat(struct __printf_sink *sk, const char *fmt, __builtin_va_list ap);

/* A scanf input source. getc returns the next character or -1; ungetc pushes
 * exactly one character back. `nread` counts characters consumed (%n). */
struct __scan_src {
    int (*get)(struct __scan_src *);
    void (*unget)(struct __scan_src *, int);
    void *ctx;
    long nread;
    int  eof_before_any;
};
int __libc_vscan(struct __scan_src *src, const char *fmt, __builtin_va_list ap);

/* ---- locale.c ---------------------------------------------------------- */
/* The one and only locale is "C". Kept as a function so a future locale
 * implementation has a seam, and so <ctype.h> stays header-only. */
int __libc_locale_is_c(void);

/* ---------------------------------------------------------------------- */
/* struct _FILE -- the real layout behind the opaque `FILE` in <stdio.h>.   */
/* ---------------------------------------------------------------------- */
/* Shared (not just stdio.c's) because fmemopen()/open_memstream() (memstream.c)
 * and popen()/pclose() (popen.c) are FILE constructors that need to build and
 * inspect one directly -- there is no fd underneath either of them, so they
 * cannot go through fopen()/fdopen(). This used to be a stdio.c-local struct;
 * it moved here, unchanged in its first block, when those two files were
 * added, specifically so a header basename collision never has to happen
 * again to notice a second TU needs it (see CLAUDE.md's header-basename note
 * -- this is the same kind of shared-visibility problem, solved instead of
 * repeated: one struct, one place, three consumers).
 *
 * THE FIELD-ORDER RULE: everything up to `next` is untouched from before this
 * struct had readers outside stdio.c -- same order, same meaning, same
 * offsets. Only new fields were appended. If you add another one, append it
 * too; do not reorder. */
#define F_READ    0x001
#define F_WRITE   0x002
#define F_EOF     0x004
#define F_ERR     0x008
#define F_ALLOC   0x010     /* the FILE itself came from malloc */
#define F_TMP     0x020     /* tmpfile(): unlink the path at fclose */
#define F_OWNBUF  0x040     /* we allocated wbuf/rbuf (setvbuf may not have) */
#define F_APPEND  0x080     /* fopen("a"): the KERNEL forces writes to EOF (O_APPEND) */
#define F_WIDE    0x100     /* fwide() orientation, once chosen */
#define F_BYTE    0x200
/* --- memstream.c: fmemopen() / open_memstream() --- */
#define F_MEM       0x400   /* fmemopen(): fixed-capacity, no real fd */
#define F_MEMOWN    0x800   /* fmemopen(NULL,...): we malloc'd mbuf; free() it at fclose */
#define F_MEMSTR    0x1000  /* open_memstream(): growing, publishes ms_bufp/ms_sizep */
#define F_MEMAPPEND 0x2000  /* fmemopen(...,"a"/"a+"): writes always land at mmax, like
                              * O_APPEND -- this is a SEPARATE bit from F_APPEND on
                              * purpose: F_APPEND leans on the kernel's O_APPEND to force
                              * the position; a memory buffer has no kernel underneath
                              * it, so this library has to do the forcing itself (see
                              * __libc_fmem_commit in memstream.c). Conflating the two
                              * would be harmless today only because no FILE is ever
                              * both fd- and mem-backed -- reusing the bit anyway would
                              * leave that as an invariant nothing enforces. */
/* --- popen.c --- */
#define F_POPEN     0x4000  /* fd is a pipe to a forked child; see popen.c */

#define RBUFSZ 4096

struct _FILE {
    int fd, flags;
    unsigned char *rbuf; int rcap, rpos, rlen;
    unsigned char *wbuf; int wcap, wlen;
    int bufmode;                 /* _IOFBF / _IOLBF / _IONBF */
    int ungot;
    char *tmpname;
    struct _FILE *next;
    /* ---- memstream.c's backend state -------------------------------------
     * Deliberately shaped like an fd's: mpos plays lseek(fd,0,SEEK_CUR)'s
     * role (the position AS IF every staged wbuf byte had already been
     * committed) and mmax is the high-water mark -- the "logical size" that
     * SEEK_END and a read-mode stream's EOF are relative to, which is NOT
     * always mcap (see the block comment at the top of memstream.c for why
     * fmemopen's r+/w+/a+ track it separately from the fixed capacity, and
     * why open_memstream's *sizep tracks mpos, not mmax). Shaping it this way
     * means fseek()/ftell() in stdio.c need only a couple of extra branches
     * instead of a whole parallel code path -- see those two functions. */
    unsigned char *mbuf;
    size_t mcap, mpos, mmax;
    char **ms_bufp;               /* open_memstream: caller's *bufp to publish, or NULL */
    size_t *ms_sizep;              /* open_memstream: caller's *sizep to publish, or NULL */
    /* ---- popen.c's backend state ------------------------------------------
     * The child this stream's pipe fd talks to, so pclose() knows what to
     * waitpid() on. -1 for every stream that isn't one of popen()'s. */
    long popen_pid;
};

/* ---- memstream.c: exported to stdio.c and to each other ------------------
 *
 * fmemopen()/open_memstream() build a `struct _FILE` directly (there is no fd
 * to fopen()/fdopen()), so they need a way onto stdio.c's stream registry --
 * the same list fflush(NULL)/exit() walk -- without a second, parallel list
 * that could drift out of sync with it. */
void __libc_stream_register(FILE *f);

/* Backend write for a mem-backed stream: commit up to n bytes from p.
 * Returns the number of bytes actually committed (see memstream.c for why
 * that can be less than n for fmemopen, and is always n for
 * open_memstream -- it grows instead of refusing), or -1/errno if nothing
 * could be committed at all (e.g. EBADF: not opened for writing). Called
 * from stdio.c's flush path; not meant for anything else to call directly. */
long __libc_fmem_commit(FILE *f, const char *p, size_t n);
long __libc_msmem_commit(FILE *f, const char *p, size_t n);

/* Backend read for fmemopen(): fill up to cap bytes of dst from the current
 * position. Returns bytes filled, 0 at the stream's logical EOF (position ==
 * mmax), or -1/errno (EBADF: not opened for reading). open_memstream has no
 * read counterpart -- see the file comment in memstream.c for why reading
 * from one is refused here even though glibc happens to allow it. */
int __libc_fmem_read(FILE *f, unsigned char *dst, int cap);

/* Stamp the buffer's terminating NUL and publish *ms_bufp and *ms_sizep. Called
 * by stdio.c on every fflush()/fclose() of an open_memstream() FILE (that is
 * the ONLY place these values are allowed to become visible -- see
 * open_memstream(3)). Returns 0, or -1/errno (ENOMEM) if growing the buffer
 * to make room for the NUL failed. */
int __libc_msmem_publish(FILE *f);

/* ---- popen.c: exported to stdio.c and vice versa -------------------------
 *
 * See the block comment at the top of popen.c for why the child of one
 * popen() call has to close every OTHER popen()'s still-open fd by hand on
 * this kernel, and why that walk rides stdio.c's existing stream registry
 * instead of a list of its own. */

/* Wrap an already-open fd (one end of popen()'s pipe) as a FILE, tagged so
 * __libc_close_popen_streams() and pclose() can find it again. Mirrors
 * stdio.c's own fdopen() plumbing; separate entry point only because popen.c
 * needs to also stash the child's pid, which fdopen()'s signature has no room
 * for. */
FILE *__libc_popen_wrap(int fd, int for_write, long pid);
/* -1 if f is not a stream popen() produced (including f == NULL); otherwise
 * the pid pclose() should waitpid() on. */
long __libc_popen_pid(FILE *f);
/* Close the fd of every currently-registered popen() stream. Called by the
 * CHILD of a fresh popen(), before it dup2()s its own pipe end and execs --
 * see popen.c. Does nothing to plain fopen()'d files (out of scope; see the
 * same comment for why). */
void __libc_close_popen_streams(void);

#endif /* _LIBC_INTERNAL_H */
