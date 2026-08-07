#ifndef _LIBC_INTERNAL_H
#define _LIBC_INTERNAL_H
/* mini-libc internals shared between TUs. Nothing here is a public API; the
 * double-underscore prefix keeps these out of the namespace a C program may
 * legally use for its own identifiers. */

#include <stddef.h>

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

#endif /* _LIBC_INTERNAL_H */
