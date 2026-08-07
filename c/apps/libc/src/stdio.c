/* mini-libc <stdio.h>: fd-backed buffered streams + a printf that agrees with
 * glibc byte for byte.
 *
 * The formatter used to render floats by peeling digits off a double with
 * repeated multiplication, which double-rounds; it also ignored the precision
 * on integer conversions, ignored the field width on every float conversion,
 * and had no %n, no %a, no %ls, and no length-modifier truncation for hh/h.
 * All of that is a silent wrong answer rather than a link error, so all of it
 * is now checked against glibc over ~150k format/value combinations by
 * `make test-libc-diff`. The exact decimal digits come from dtoa.c. */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <wchar.h>
#include <sys/stat.h>
#include "libc_internal.h"

static size_t snlen(const char *s, size_t m) { size_t i = 0; while (i < m && s[i]) i++; return i; }

/* ---------------------------------------------------------------------- */
/* FILE                                                                    */
/* ---------------------------------------------------------------------- */
#define F_READ    0x001
#define F_WRITE   0x002
#define F_EOF     0x004
#define F_ERR     0x008
#define F_ALLOC   0x010     /* the FILE itself came from malloc */
#define F_TMP     0x020     /* tmpfile(): unlink the path at fclose */
#define F_OWNBUF  0x040     /* we allocated wbuf/rbuf (setvbuf may not have) */
#define F_APPEND  0x080
#define F_WIDE    0x100     /* fwide() orientation, once chosen */
#define F_BYTE    0x200

#define RBUFSZ 4096

struct _FILE {
    int fd, flags;
    unsigned char *rbuf; int rcap, rpos, rlen;
    unsigned char *wbuf; int wcap, wlen;
    int bufmode;                 /* _IOFBF / _IOLBF / _IONBF */
    int ungot;
    char *tmpname;
    struct _FILE *next;
};

static struct _FILE _stdin  = { 0, F_READ,  0,0,0,0, 0,0,0, _IONBF, -1, 0, 0 };
static struct _FILE _stdout = { 1, F_WRITE, 0,0,0,0, 0,0,0, _IONBF, -1, 0, 0 };
static struct _FILE _stderr = { 2, F_WRITE, 0,0,0,0, 0,0,0, _IONBF, -1, 0, 0 };
FILE *stdin = &_stdin, *stdout = &_stdout, *stderr = &_stderr;

/* Every open stream, so exit() can flush them. stdout/stderr are unbuffered by
 * default -- the serial console is an interactive device and C explicitly
 * allows that -- but setvbuf() can make them buffered, and then the flush at
 * exit is the difference between seeing a program's last line and not. */
static struct _FILE *g_streams;
static void stream_register(struct _FILE *f)
{ f->next = g_streams; g_streams = f; }
static void stream_unregister(struct _FILE *f)
{
    struct _FILE **pp = &g_streams;
    while (*pp) { if (*pp == f) { *pp = f->next; return; } pp = &(*pp)->next; }
}

/* write-all: loops over short writes (a pipe can take fewer bytes than asked). */
static int wr(int fd, const char *p, size_t n)
{
    size_t off = 0;
    while (off < n) { long k = write(fd, p + off, n - off); if (k <= 0) return -1; off += (size_t)k; }
    return 0;
}

static int flush_w(FILE *f)
{
    if (!f || f->wlen == 0) return 0;
    int n = f->wlen;
    f->wlen = 0;                       /* drop first: a failing fd must not spin */
    if (wr(f->fd, (const char *)f->wbuf, (size_t)n) < 0) { f->flags |= F_ERR; return EOF; }
    return 0;
}

static int fput_raw(FILE *f, const char *p, size_t n)
{
    if (!f) return -1;
    if (f->bufmode == _IONBF || (!f->wbuf && f->wcap == 0 && f->bufmode != _IOFBF && f->bufmode != _IOLBF))
        return wr(f->fd, p, n);
    if (!f->wbuf) {
        f->wbuf = malloc(BUFSIZ);
        if (!f->wbuf) { f->bufmode = _IONBF; return wr(f->fd, p, n); }
        f->wcap = BUFSIZ; f->flags |= F_OWNBUF;
    }
    while (n) {
        int room = f->wcap - f->wlen;
        if (room == 0) { if (flush_w(f) < 0) return -1; room = f->wcap; }
        size_t take = n < (size_t)room ? n : (size_t)room;
        memcpy(f->wbuf + f->wlen, p, take);
        f->wlen += (int)take; p += take; n -= take;
        if (f->bufmode == _IOLBF && memchr(f->wbuf, '\n', (size_t)f->wlen))
            { if (flush_w(f) < 0) return -1; }
        else if (f->wlen == f->wcap) { if (flush_w(f) < 0) return -1; }
    }
    return 0;
}

int fflush(FILE *f)
{
    if (!f) {                      /* C11: NULL flushes every output stream */
        int r = 0;
        for (struct _FILE *s = g_streams; s; s = s->next) if (flush_w(s) < 0) r = EOF;
        if (flush_w(&_stdout) < 0) r = EOF;
        if (flush_w(&_stderr) < 0) r = EOF;
        return r;
    }
    return flush_w(f);
}
void __libc_flush_all(void) { fflush(NULL); }

/* ---------------------------------------------------------------------- */
/* formatting core                                                         */
/* ---------------------------------------------------------------------- */
static void sink_put(struct __printf_sink *sk, const char *s, size_t n)
{ sk->put(sk, s, n); sk->count += n; }

static void sink_pad(struct __printf_sink *sk, char c, long n)
{
    char blk[32]; if (n <= 0) return;
    memset(blk, c, sizeof blk);
    while (n > 0) { size_t k = (size_t)(n < 32 ? n : 32); sink_put(sk, blk, k); n -= (long)k; }
}

static int u64_to(char *out, unsigned long long v, int base, int upper)
{
    static const char lo[] = "0123456789abcdef", up[] = "0123456789ABCDEF";
    const char *d = upper ? up : lo;
    char tmp[32]; int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = d[v % (unsigned)base]; v /= (unsigned)base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* Emit sign/prefix/zero-pad/body/width in the one order C specifies. */
static void emit(struct __printf_sink *sk, char sign, const char *prefix,
                 const char *body, size_t blen, long width, int left, int zero)
{
    size_t plen = prefix ? strlen(prefix) : 0;
    long total = (long)(blen + plen + (sign ? 1u : 0u));
    long pad = width > total ? width - total : 0;
    if (!left && !zero) sink_pad(sk, ' ', pad);
    if (sign) sink_put(sk, &sign, 1);
    if (plen) sink_put(sk, prefix, plen);
    if (!left && zero) sink_pad(sk, '0', pad);
    sink_put(sk, body, blen);
    if (left) sink_pad(sk, ' ', pad);
}

/* ---- floating point ---------------------------------------------------- */
struct fbuf { char b[__LIBC_DTOA_MAX + 64]; size_t n; };
static void fb(struct fbuf *o, char c) { if (o->n < sizeof o->b) o->b[o->n++] = c; }
static void fbs(struct fbuf *o, const char *s, size_t n) { while (n--) fb(o, *s++); }
static void fbrep(struct fbuf *o, char c, long n) { while (n-- > 0) fb(o, c); }

/* `mind` differs by conversion and it is not a detail: %e/%E require AT LEAST
 * TWO exponent digits ("1e+05"), while %a/%A require at least ONE ("0x1p+5").
 * Padding both to two prints 0x0p+00, which no other libc produces. */
static void fmt_exp(struct fbuf *o, int e, char echar, int mind)
{
    fb(o, echar);
    fb(o, e < 0 ? '-' : '+');
    unsigned a = (unsigned)(e < 0 ? -e : e);
    char t[8]; int n = 0;
    do { t[n++] = (char)('0' + a % 10); a /= 10; } while (a);
    while (n < mind) t[n++] = '0';
    while (n--) fb(o, t[n]);
}

/* %a / %A: an exact hexadecimal float. Exact because the mantissa is already
 * binary -- no rounding at all unless a precision shorter than 13 hex digits
 * asks for it, and then it is round-half-to-even like everything else here. */
static void fmt_hexfloat(struct fbuf *o, double v, int prec, int upper, int alt)
{
    union { double d; uint64_t u; } u = { v };
    uint64_t frac = u.u & (((uint64_t)1 << 52) - 1);
    int bexp = (int)((u.u >> 52) & 0x7ff);
    int lead, e2;
    if (bexp == 0) { lead = frac ? 0 : 0; e2 = frac ? -1022 : 0; }
    else { lead = 1; e2 = bexp - 1023; }
    if (bexp == 0 && frac == 0) { e2 = 0; }

    /* 13 hex digits carry all 52 fraction bits. */
    char hd[13];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (int i = 0; i < 13; i++) hd[i] = dig[(frac >> (48 - 4 * i)) & 0xf];
    int nd = 13;
    if (prec >= 0 && prec < 13) {
        /* round at `prec` hex digits, ties to even */
        int cut = prec;
        uint64_t restmask = (cut == 0) ? frac : (frac & ((((uint64_t)1) << (52 - 4 * cut)) - 1));
        uint64_t half = (uint64_t)1 << (52 - 4 * cut - 1);
        int up = 0;
        if (restmask > half) up = 1;
        else if (restmask == half) {
            int lastdig = cut ? (int)((frac >> (52 - 4 * cut)) & 0xf) : lead;
            up = lastdig & 1;
        }
        if (up) {
            int i = cut - 1;
            for (; i >= 0; i--) {
                int dv = (int)(hd[i] <= '9' ? hd[i] - '0' : (hd[i] | 0x20) - 'a' + 10);
                if (dv < 15) { hd[i] = dig[dv + 1]; break; }
                hd[i] = dig[0];
            }
            if (i < 0) lead++;
        }
        nd = cut;
    } else if (prec < 0) {
        while (nd > 0 && hd[nd - 1] == '0') nd--;   /* shortest exact form */
    }

    /* The "0x" is NOT written here: it is a PREFIX in the emit() sense, so that
     * a '0' flag pads between it and the digits (%014a -> "0x000000000p+0")
     * exactly as it does for %#x, rather than in front of it. */
    fb(o, (char)('0' + lead));
    if (nd > 0 || alt || (prec > 0)) {
        fb(o, '.');
        fbs(o, hd, (size_t)nd);
        if (prec > nd) fbrep(o, '0', prec - nd);
    }
    fmt_exp(o, e2, upper ? 'P' : 'p', 1);
}

static void fmt_float(struct __printf_sink *sk, double v, int prec, char conv,
                      long width, int left, int zero, int plus, int space, int alt)
{
    int upper = (conv >= 'A' && conv <= 'Z');
    char lc = (char)(conv | 0x20);
    char sign = 0;
    union { double d; uint64_t u; } uv = { v };

    if (uv.u >> 63) { sign = '-'; v = -v; }
    else if (plus) sign = '+';
    else if (space) sign = ' ';

    if (v != v) {                                    /* NaN */
        emit(sk, sign, NULL, upper ? "NAN" : "nan", 3, width, left, 0);
        return;
    }
    if (v > 1.7976931348623157e308) {                /* infinity */
        emit(sk, sign, NULL, upper ? "INF" : "inf", 3, width, left, 0);
        return;
    }

    struct fbuf o; o.n = 0;
    char digs[__LIBC_DTOA_MAX + 2];
    int decpt = 0, nd;

    if (lc == 'a') {
        fmt_hexfloat(&o, v, prec, upper, alt);
        emit(sk, sign, upper ? "0X" : "0x", o.b, o.n, width, left, zero);
        return;
    }

    if (prec < 0) prec = 6;

    if (lc == 'g') {
        int P = prec ? prec : 1;
        if (P > __LIBC_DTOA_MAX) P = __LIBC_DTOA_MAX;
        nd = __libc_dtoa(v, 0, P, digs, &decpt);
        int X = decpt - 1;
        if (X < -4 || X >= P) { lc = 'e'; prec = P - 1; }
        else { lc = 'f'; prec = P - 1 - X; }
        /* digits are already rounded to P significant figures; reuse them */
    } else if (lc == 'e') {
        int want = prec + 1;
        if (want > __LIBC_DTOA_MAX) want = __LIBC_DTOA_MAX;
        nd = __libc_dtoa(v, 0, want, digs, &decpt);
    } else {
        int want = prec;
        if (want > __LIBC_DTOA_MAX) want = __LIBC_DTOA_MAX;
        nd = __libc_dtoa(v, 1, want, digs, &decpt);
    }
    int gconv = (conv | 0x20) == 'g';
    long dot_at = -1;                    /* where the '.' went, or -1 for none */

    /* %g removes trailing zeros from the FRACTIONAL portion only. Stripping the
     * whole tail instead turns "100000" into "1" and "0" into "", which is why
     * this is anchored on the decimal point rather than on the last character. */
    #define TRIM_G() do { \
        if (gconv && !alt && dot_at >= 0) { \
            while ((long)o.n > dot_at + 1 && o.b[o.n - 1] == '0') o.n--; \
            if ((long)o.n == dot_at + 1) o.n--; \
        } \
    } while (0)

    if (lc == 'e') {
        fb(&o, nd > 0 ? digs[0] : '0');
        if (prec > 0 || alt) {
            dot_at = (long)o.n; fb(&o, '.');
            for (int i = 1; i <= prec; i++) fb(&o, i < nd ? digs[i] : '0');
        }
        TRIM_G();
        fmt_exp(&o, nd > 0 ? decpt - 1 : 0, upper ? 'E' : 'e', 2);
    } else {
        if (decpt <= 0) fb(&o, '0');
        else for (int i = 0; i < decpt; i++) fb(&o, i < nd ? digs[i] : '0');
        if (prec > 0 || alt) {
            dot_at = (long)o.n; fb(&o, '.');
            for (int j = 1; j <= prec; j++) {
                int idx = decpt + j - 1;
                fb(&o, (idx >= 0 && idx < nd) ? digs[idx] : '0');
            }
        }
        TRIM_G();
    }
    #undef TRIM_G
    emit(sk, sign, NULL, o.b, o.n, width, left, zero);
}

/* ---- the formatter ----------------------------------------------------- */
int __libc_vformat(struct __printf_sink *sk, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            const char *run = fmt;
            while (fmt[1] && fmt[1] != '%') fmt++;
            sink_put(sk, run, (size_t)(fmt - run) + 1);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else if (*fmt == '\'') /* SUSv2 thousands grouping: no locale has it here */ ;
            else break;
        }
        long width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int); fmt++;
            if (width < 0) { left = 1; width = -width; }
        } else while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
            if (width > 0x1000000) width = 0x1000000;   /* clamp: overflow is UB */
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            else while (*fmt >= '0' && *fmt <= '9') {
                prec = prec * 10 + (*fmt++ - '0');
                if (prec > 0x1000000) prec = 0x1000000;
            }
        }
        /* length modifiers: -2 = hh, -1 = h, 0 = int, 1 = l, 2 = ll/j/z/t, 3 = L */
        int lng = 0;
        for (;;) {
            if (*fmt == 'l') { lng = (lng >= 1) ? 2 : 1; fmt++; }
            else if (*fmt == 'h') { lng = (lng <= -1) ? -2 : -1; fmt++; }
            else if (*fmt == 'j' || *fmt == 'z' || *fmt == 't') { lng = 2; fmt++; }
            else if (*fmt == 'L') { lng = 3; fmt++; }
            else if (*fmt == 'q') { lng = 2; fmt++; }
            else break;
        }

        char num[72]; size_t blen = 0; char sign = 0; const char *prefix = NULL;
        char c = *fmt;
        int is_signed = 0;
        unsigned long long uval = 0;

        switch (c) {
        case 'd': case 'i': {
            long long v = (lng >= 2) ? va_arg(ap, long long)
                        : (lng == 1) ? (long long)va_arg(ap, long)
                                     : (long long)va_arg(ap, int);
            if (lng == -1) v = (short)v;
            else if (lng <= -2) v = (signed char)v;
            if (v < 0) { sign = '-'; uval = ~(unsigned long long)v + 1ULL; }
            else { uval = (unsigned long long)v; if (plus) sign = '+'; else if (space) sign = ' '; }
            is_signed = 1;
            blen = (size_t)u64_to(num, uval, 10, 0);
            break; }
        case 'u': case 'o': case 'x': case 'X': {
            unsigned long long v = (lng >= 2) ? va_arg(ap, unsigned long long)
                                 : (lng == 1) ? (unsigned long long)va_arg(ap, unsigned long)
                                              : (unsigned long long)va_arg(ap, unsigned);
            if (lng == -1) v = (unsigned short)v;
            else if (lng <= -2) v = (unsigned char)v;
            int base = (c == 'u') ? 10 : (c == 'o') ? 8 : 16;
            blen = (size_t)u64_to(num, v, base, c == 'X');
            if (alt && (c == 'x' || c == 'X') && v) prefix = (c == 'X') ? "0X" : "0x";
            uval = v;
            break; }
        case 'c': {
            if (lng >= 1) {                       /* %lc: a wide character */
                wchar_t wc = (wchar_t)va_arg(ap, wint_t);
                char mb[8]; int n = wctomb(mb, wc);
                if (n < 0) n = 0;
                emit(sk, 0, NULL, mb, (size_t)n, width, left, 0);
            } else {
                num[0] = (char)va_arg(ap, int);
                emit(sk, 0, NULL, num, 1, width, left, 0);
            }
            continue; }
        case 's': {
            if (lng >= 1) {                       /* %ls: a wide string */
                const wchar_t *ws = va_arg(ap, const wchar_t *);
                if (!ws) { emit(sk, 0, NULL, "(null)", 6, width, left, 0); continue; }
                /* Measure first: the precision counts BYTES, and a partial
                 * multibyte character must not be emitted. */
                size_t bytes = 0; char mb[8];
                for (const wchar_t *p = ws; *p; p++) {
                    int n = wctomb(mb, *p); if (n < 0) break;
                    if (prec >= 0 && bytes + (size_t)n > (size_t)prec) break;
                    bytes += (size_t)n;
                }
                long pad = width > (long)bytes ? width - (long)bytes : 0;
                if (!left) sink_pad(sk, ' ', pad);
                size_t done = 0;
                for (const wchar_t *p = ws; *p; p++) {
                    int n = wctomb(mb, *p); if (n < 0) break;
                    if (done + (size_t)n > bytes) break;
                    sink_put(sk, mb, (size_t)n); done += (size_t)n;
                }
                if (left) sink_pad(sk, ' ', pad);
                continue;
            }
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t n = (prec >= 0) ? snlen(s, (size_t)prec) : strlen(s);
            long pad = width > (long)n ? width - (long)n : 0;
            if (!left) sink_pad(sk, ' ', pad);
            sink_put(sk, s, n);
            if (left) sink_pad(sk, ' ', pad);
            continue; }
        case 'p': {
            void *pv = va_arg(ap, void *);
            if (!pv) { emit(sk, 0, NULL, "(nil)", 5, width, left, 0); continue; }
            blen = (size_t)u64_to(num, (unsigned long long)(uintptr_t)pv, 16, 0);
            emit(sk, 0, "0x", num, blen, width, left, zero);
            continue; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d;
            if (lng == 3) d = (double)va_arg(ap, long double);   /* see the note below */
            else d = va_arg(ap, double);
            fmt_float(sk, d, prec, c, width, left, zero, plus, space, alt);
            continue; }
        case 'n': {
            long n = (long)sk->count;
            if (lng >= 2) *va_arg(ap, long long *) = n;
            else if (lng == 1) *va_arg(ap, long *) = n;
            else if (lng == -1) *va_arg(ap, short *) = (short)n;
            else if (lng <= -2) *va_arg(ap, signed char *) = (signed char)n;
            else *va_arg(ap, int *) = (int)n;
            continue; }
        case '%': sink_put(sk, "%", 1); continue;
        case 0: goto done;
        default:  /* unknown conversion: print it literally, as glibc does */
            sink_put(sk, "%", 1); sink_put(sk, &c, 1); continue;
        }

        /* Integer conversions share the precision / width / pad tail. C says an
         * explicit precision makes the '0' flag inoperative, and that a zero
         * value with precision 0 produces no digits at all. */
        {
            const char *body = num;
            char padbuf[80];
            (void)is_signed;
            if (prec >= 0) {
                zero = 0;                       /* C: precision disables '0' */
                if (uval == 0 && prec == 0) blen = 0;   /* zero at precision 0 prints nothing */
                if ((size_t)prec > blen && (size_t)prec < sizeof padbuf) {
                    size_t z = (size_t)prec - blen;
                    memset(padbuf, '0', z);
                    memcpy(padbuf + z, num, blen);
                    body = padbuf; blen = (size_t)prec;
                }
            }
            /* '#' on %o: "increase the precision, if and only if necessary, to
             * force the first digit to be a zero". Stated as a rule about the
             * FINISHED digit string, which is why it is applied here and not
             * before the precision padding -- %#.5o of 1 is "00001", already
             * starting with a zero, and must not gain a sixth digit. */
            if (alt && c == 'o' && (blen == 0 || body[0] != '0')) prefix = "0";
            emit(sk, sign, prefix, body, blen, width, left, zero);
        }
    }
done:
    return (int)sk->count;
}

/* NOTE on %Lf: x86-64's long double is the 80-bit x87 type, and this library
 * has no 80-bit decimal conversion; the value is narrowed to double first. So
 * %Lf is accurate to a double, not to a long double. It is not silently a
 * different conversion -- it is the same conversion at lower precision -- but a
 * program that needs all 64 mantissa bits printed will not get them here. */

/* ---------------------------------------------------------------------- */
/* sinks                                                                   */
/* ---------------------------------------------------------------------- */
struct memsink { struct __printf_sink sk; char *p; size_t cap, len; };
static void mem_put(struct __printf_sink *s, const char *b, size_t n)
{
    struct memsink *m = (struct memsink *)s;
    if (m->len < m->cap) {
        size_t take = m->cap - m->len; if (take > n) take = n;
        memcpy(m->p + m->len, b, take);
    }
    m->len += n;
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    struct memsink m;
    m.sk.put = mem_put; m.sk.ctx = NULL; m.sk.count = 0;
    m.p = out; m.cap = (out && cap) ? cap - 1 : 0; m.len = 0;
    int r = __libc_vformat(&m.sk, fmt, ap);
    if (out && cap) out[m.len < m.cap ? m.len : m.cap] = 0;
    return r;
}
int snprintf(char *out, size_t cap, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(out, cap, fmt, ap); va_end(ap); return r; }
int vsprintf(char *out, const char *fmt, va_list ap) { return vsnprintf(out, (size_t)-1, fmt, ap); }
int sprintf(char *out, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(out, (size_t)-1, fmt, ap); va_end(ap); return r; }

struct filesink { struct __printf_sink sk; FILE *f; int err; };
static void file_put(struct __printf_sink *s, const char *b, size_t n)
{
    struct filesink *m = (struct filesink *)s;
    if (!m->err && fput_raw(m->f, b, n) < 0) m->err = 1;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (!f) f = stderr;
    struct filesink m; m.sk.put = file_put; m.sk.ctx = NULL; m.sk.count = 0; m.f = f; m.err = 0;
    int r = __libc_vformat(&m.sk, fmt, ap);
    if (m.err) { f->flags |= F_ERR; return -1; }
    return r;
}
int fprintf(FILE *f, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }
int printf(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

int vdprintf(int fd, const char *fmt, va_list ap)
{
    FILE tmp; memset(&tmp, 0, sizeof tmp);
    tmp.fd = fd; tmp.flags = F_WRITE; tmp.bufmode = _IONBF; tmp.ungot = -1;
    return vfprintf(&tmp, fmt, ap);
}
int dprintf(int fd, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vdprintf(fd, fmt, ap); va_end(ap); return r; }

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    if (n < 0) { va_end(ap2); return -1; }
    char *p = malloc((size_t)n + 1);
    if (!p) { va_end(ap2); return -1; }
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    *out = p;
    return n;
}
int asprintf(char **out, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vasprintf(out, fmt, ap); va_end(ap); return r; }

/* ---------------------------------------------------------------------- */
/* character / block output                                                */
/* ---------------------------------------------------------------------- */
int fputc(int c, FILE *f)
{
    unsigned char ch = (unsigned char)c;
    if (!f) f = stdout;
    if (fput_raw(f, (const char *)&ch, 1) < 0) { f->flags |= F_ERR; return EOF; }
    return (int)ch;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }
int fputs(const char *s, FILE *f)
{
    if (!f) f = stdout;
    if (fput_raw(f, s, strlen(s)) < 0) { f->flags |= F_ERR; return EOF; }
    return 0;
}
int puts(const char *s)
{ return (fput_raw(stdout, s, strlen(s)) < 0 || fput_raw(stdout, "\n", 1) < 0) ? EOF : 0; }

size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (sz == 0 || n == 0) return 0;
    if (n > (size_t)-1 / sz) { if (f) f->flags |= F_ERR; return 0; }
    if (!f) f = stdout;
    if (fput_raw(f, (const char *)p, sz * n) < 0) { f->flags |= F_ERR; return 0; }
    return n;
}

void perror(const char *s)
{
    int e = errno;                          /* capture before anything can clobber it */
    fflush(stdout);
    if (s && *s) { fput_raw(stderr, s, strlen(s)); fput_raw(stderr, ": ", 2); }
    const char *m = strerror(e);
    fput_raw(stderr, m, strlen(m));
    fput_raw(stderr, "\n", 1);
}

/* ---------------------------------------------------------------------- */
/* input                                                                   */
/* ---------------------------------------------------------------------- */
static int refill(FILE *f)
{
    if (!f->rbuf) {
        f->rbuf = (unsigned char *)malloc(RBUFSZ);
        if (!f->rbuf) { f->flags |= F_ERR; return -1; }
        f->rcap = RBUFSZ;
    }
    long r = read(f->fd, f->rbuf, (size_t)f->rcap);
    if (r < 0) { f->flags |= F_ERR; f->rlen = f->rpos = 0; return -1; }
    if (r == 0) { f->rlen = f->rpos = 0; f->flags |= F_EOF; return -1; }
    f->rlen = (int)r; f->rpos = 0;
    return 0;
}

int fgetc(FILE *f)
{
    if (!f) return EOF;
    if (f->wlen) flush_w(f);                    /* interleaved read after write */
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->rpos >= f->rlen && refill(f) < 0) return EOF;
    return f->rbuf[f->rpos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE *f)
{
    if (c == EOF || !f) return EOF;
    f->ungot = (unsigned char)c; f->flags &= ~F_EOF;
    return (unsigned char)c;
}

size_t fread(void *ptr, size_t sz, size_t n, FILE *f)
{
    if (sz == 0 || n == 0) return 0;
    if (n > (size_t)-1 / sz) return 0;
    size_t total = sz * n, got = 0;
    unsigned char *out = (unsigned char *)ptr;
    /* Bulk path: copy straight out of the refill buffer instead of one fgetc
     * per byte -- fread of a megabyte was a megabyte of function calls. */
    while (got < total) {
        if (f->ungot >= 0) { out[got++] = (unsigned char)f->ungot; f->ungot = -1; continue; }
        if (f->rpos >= f->rlen && refill(f) < 0) break;
        size_t avail = (size_t)(f->rlen - f->rpos), want = total - got;
        size_t take = avail < want ? avail : want;
        memcpy(out + got, f->rbuf + f->rpos, take);
        f->rpos += (int)take; got += take;
    }
    return got / sz;
}

char *fgets(char *s, int size, FILE *f)
{
    int i = 0;
    if (size <= 0 || !s || !f) return NULL;
    while (i < size - 1) { int c = fgetc(f); if (c == EOF) break; s[i++] = (char)c; if (c == '\n') break; }
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f)
{
    if (!lineptr || !n || !f) { errno = EINVAL; return -1; }
    if (!*lineptr || *n == 0) { *n = 128; *lineptr = malloc(*n); if (!*lineptr) { *n = 0; return -1; } }
    size_t len = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) { if (len == 0) return -1; break; }
        if (len + 2 > *n) {
            size_t nn = *n * 2;
            char *p = realloc(*lineptr, nn);
            if (!p) return -1;
            *lineptr = p; *n = nn;
        }
        (*lineptr)[len++] = (char)c;
        if (c == delim) break;
    }
    (*lineptr)[len] = 0;
    return (ssize_t)len;
}
ssize_t getline(char **lineptr, size_t *n, FILE *f) { return getdelim(lineptr, n, '\n', f); }

/* ---------------------------------------------------------------------- */
/* scanf entry points (the engine lives in scanf.c)                        */
/* ---------------------------------------------------------------------- */
struct strsrc { struct __scan_src s; const char *p; const char *start; };
static int str_get(struct __scan_src *s)
{ struct strsrc *m = (struct strsrc *)s; return *m->p ? (unsigned char)*m->p++ : -1; }
static void str_unget(struct __scan_src *s, int c)
{ struct strsrc *m = (struct strsrc *)s; if (c != -1 && m->p > m->start) m->p--; }

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    struct strsrc m;
    m.s.get = str_get; m.s.unget = str_unget; m.s.ctx = NULL; m.s.nread = 0; m.s.eof_before_any = 0;
    m.p = s; m.start = s;
    return __libc_vscan(&m.s, fmt, ap);
}
int sscanf(const char *s, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r; }

struct filesrc { struct __scan_src s; FILE *f; };
static int file_get(struct __scan_src *s) { return fgetc(((struct filesrc *)s)->f); }
static void file_unget(struct __scan_src *s, int c) { if (c != -1) ungetc(c, ((struct filesrc *)s)->f); }

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    struct filesrc m;
    m.s.get = file_get; m.s.unget = file_unget; m.s.ctx = NULL; m.s.nread = 0; m.s.eof_before_any = 0;
    m.f = f;
    return __libc_vscan(&m.s, fmt, ap);
}
int fscanf(FILE *f, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfscanf(f, fmt, ap); va_end(ap); return r; }
int vscanf(const char *fmt, va_list ap) { return vfscanf(stdin, fmt, ap); }
int scanf(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vfscanf(stdin, fmt, ap); va_end(ap); return r; }

/* ---------------------------------------------------------------------- */
/* status / positioning                                                    */
/* ---------------------------------------------------------------------- */
int   feof(FILE *f)   { return f && (f->flags & F_EOF) != 0; }
int   ferror(FILE *f) { return f && (f->flags & F_ERR) != 0; }
void  clearerr(FILE *f) { if (f) f->flags &= ~(F_EOF | F_ERR); }
int   fileno(FILE *f) { if (!f) { errno = EBADF; return -1; } return f->fd; }

int fseek(FILE *f, long off, int whence)
{
    if (!f) { errno = EBADF; return -1; }
    if (flush_w(f) < 0) return -1;
    /* A pending ungetc and a read buffer both mean the fd offset is ahead of
     * the stream position; SEEK_CUR has to be corrected for both. */
    if (whence == SEEK_CUR) off -= (long)(f->rlen - f->rpos) + (f->ungot >= 0 ? 1 : 0);
    long r = lseek(f->fd, off, whence);
    if (r < 0) return -1;
    f->rpos = f->rlen = 0; f->ungot = -1; f->flags &= ~F_EOF;
    return 0;
}
int fseeko(FILE *f, off_t off, int whence) { return fseek(f, (long)off, whence); }
long ftell(FILE *f)
{
    if (!f) { errno = EBADF; return -1; }
    long r = lseek(f->fd, 0, SEEK_CUR);
    if (r < 0) return -1;
    return r - (long)(f->rlen - f->rpos) - (f->ungot >= 0 ? 1 : 0) + f->wlen;
}
off_t ftello(FILE *f) { return (off_t)ftell(f); }
void rewind(FILE *f) { if (f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; } }
int fgetpos(FILE *f, fpos_t *pos) { if (!pos) { errno = EINVAL; return -1; } long r = ftell(f); if (r < 0) return -1; *pos = r; return 0; }
int fsetpos(FILE *f, const fpos_t *pos) { if (!pos) { errno = EINVAL; return -1; } return fseek(f, (long)*pos, SEEK_SET); }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    if (!f || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) { errno = EINVAL; return -1; }
    flush_w(f);
    if (f->wbuf && (f->flags & F_OWNBUF)) { free(f->wbuf); f->flags &= ~F_OWNBUF; }
    f->wbuf = NULL; f->wcap = 0; f->wlen = 0;
    f->bufmode = mode;
    if (mode != _IONBF) {
        if (buf && size >= 2) { f->wbuf = (unsigned char *)buf; f->wcap = (int)size; }
        else if (size >= 2 && size < (size_t)INT_MAX) {
            f->wbuf = malloc(size); if (f->wbuf) { f->wcap = (int)size; f->flags |= F_OWNBUF; }
        }
        /* else: buffer is allocated lazily at BUFSIZ on first write */
    }
    return 0;
}
void setbuf(FILE *f, char *buf) { setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }
void setlinebuf(FILE *f) { setvbuf(f, NULL, _IOLBF, BUFSIZ); }

/* fwide: this implementation never changes a stream's byte/wide orientation
 * behaviour -- both fputc and fputwc work on any stream -- so fwide only
 * records and reports the orientation, which is all a conforming program can
 * observe from it. */
int fwide(FILE *f, int mode)
{
    if (!f) return 0;
    if (mode > 0 && !(f->flags & (F_WIDE | F_BYTE))) f->flags |= F_WIDE;
    else if (mode < 0 && !(f->flags & (F_WIDE | F_BYTE))) f->flags |= F_BYTE;
    return (f->flags & F_WIDE) ? 1 : ((f->flags & F_BYTE) ? -1 : 0);
}

/* ---------------------------------------------------------------------- */
/* open / close                                                            */
/* ---------------------------------------------------------------------- */
static int mode_flags(const char *m, int *libc_flags)
{
    int o, lf;
    if (!m) return -1;
    if (m[0] == 'r') { o = O_RDONLY; lf = F_READ; }
    else if (m[0] == 'w') { o = O_WRONLY | O_CREAT | O_TRUNC; lf = F_WRITE; }
    else if (m[0] == 'a') { o = O_WRONLY | O_CREAT | O_APPEND; lf = F_WRITE | F_APPEND; }
    else return -1;
    for (const char *p = m; *p; p++) {
        if (*p == '+') { o = (o & ~3) | O_RDWR; lf |= F_READ | F_WRITE; }
        else if (*p == 'e') o |= 0;              /* 'e' = O_CLOEXEC: no exec-close here */
        /* 'b' is a no-op: this is not a text/binary-distinguishing system */
    }
    *libc_flags = lf;
    return o;
}

static FILE *file_wrap(int fd, int lf)
{
    FILE *f = (FILE *)malloc(sizeof *f);
    if (!f) { close(fd); errno = ENOMEM; return NULL; }
    memset(f, 0, sizeof *f);
    f->fd = fd; f->flags = lf | F_ALLOC; f->ungot = -1;
    f->bufmode = _IOFBF;                    /* real files are fully buffered */
    stream_register(f);
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int lf, oflags = mode_flags(mode, &lf);
    if (oflags < 0) { errno = EINVAL; return NULL; }
    int fd = open(path, oflags);
    if (fd < 0) return NULL;
    return file_wrap(fd, lf);
}
FILE *fdopen(int fd, const char *mode)
{ int lf; if (mode_flags(mode, &lf) < 0) { errno = EINVAL; return NULL; } return file_wrap(fd, lf); }

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    int lf, oflags = mode_flags(mode, &lf);
    if (!f || oflags < 0) { errno = EINVAL; return NULL; }
    flush_w(f);
    if (!path) return f;                    /* mode change only: nothing to redo */
    int fd = open(path, oflags);
    if (fd < 0) { return NULL; }
    close(f->fd);
    f->fd = fd;
    f->flags = (f->flags & (F_ALLOC | F_OWNBUF)) | lf;
    f->rpos = f->rlen = 0; f->wlen = 0; f->ungot = -1;
    return f;
}

int fclose(FILE *f)
{
    if (!f) { errno = EBADF; return EOF; }
    int r = flush_w(f);
    int fd = f->fd;
    if (f->rbuf) free(f->rbuf);
    if (f->wbuf && (f->flags & F_OWNBUF)) free(f->wbuf);
    if (f->flags & F_TMP) { close(fd); if (f->tmpname) { unlink(f->tmpname); free(f->tmpname); } }
    else if (f->flags & F_ALLOC) close(fd);
    stream_unregister(f);
    if (f->flags & F_ALLOC) free(f);
    else { f->rbuf = f->wbuf = NULL; f->rcap = f->rpos = f->rlen = f->wcap = f->wlen = 0; }
    return r;
}

int remove(const char *path) { return unlink(path); }

/* ---------------------------------------------------------------------- */
/* temporary files                                                         */
/* ---------------------------------------------------------------------- */
/* LogitOS cannot unlink an open file and keep the data (LogitFS has no link
 * count), so a tmpfile()'s path is removed at fclose instead of at creation.
 * The visible difference: a program that crashes between tmpfile() and fclose()
 * leaves the file behind in /tmp, where C says it should already be gone. */
FILE *tmpfile(void)
{
    char name[64];
    mkdir("/tmp", 0777);
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof name, "/tmp/tmpfXXXXXX");
        if (!mktemp(name) || !name[0]) return NULL;
        int fd = open(name, O_RDWR | O_CREAT | O_TRUNC);
        if (fd < 0) continue;
        FILE *f = file_wrap(fd, F_READ | F_WRITE);
        if (!f) { unlink(name); return NULL; }
        f->flags |= F_TMP;
        f->tmpname = strdup(name);
        return f;
    }
    return NULL;
}

char *tmpnam(char *s)
{
    static char store[L_tmpnam];
    char *out = s ? s : store;
    mkdir("/tmp", 0777);
    strcpy(out, "/tmp/tmpnXXXXXX");
    mktemp(out);
    return out[0] ? out : NULL;
}
