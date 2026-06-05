#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>      /* FILE typedef + the API we implement */
#include <unistd.h>     /* read/write/lseek/close */
#include <fcntl.h>      /* open + O_* */
#include <stdlib.h>     /* malloc/free */

size_t strlen(const char *);
static size_t snlen(const char *s, int m) { int i = 0; while (i < m && s[i]) i++; return (size_t)i; }

/* fd-backed FILE: output is unbuffered (each write is a syscall); input has a
 * small refill buffer so fgetc/fgets aren't a syscall per byte. */
#define F_READ  1
#define F_WRITE 2
#define F_EOF   4
#define F_ERR   8
#define F_ALLOC 16
#define RBUFSZ  4096
struct _FILE {
    int fd, flags;
    unsigned char *rbuf; int rcap, rpos, rlen;
    int ungot;
};
static struct _FILE _stdin  = { 0, F_READ,  0, 0, 0, 0, -1 };
static struct _FILE _stdout = { 1, F_WRITE, 0, 0, 0, 0, -1 };
static struct _FILE _stderr = { 2, F_WRITE, 0, 0, 0, 0, -1 };
FILE *stdin = &_stdin, *stdout = &_stdout, *stderr = &_stderr;

/* write-all: loops over short writes (a pipe can take fewer bytes than asked);
 * returns 0 on success, -1 if the underlying write() ever fails. */
static int wr(int fd, const char *p, size_t n)
{
    size_t off = 0;
    while (off < n) { long k = write(fd, p + off, n - off); if (k <= 0) return -1; off += (size_t)k; }
    return 0;
}

/* ---------- integer/float formatting into a bounded buffer ---------- */
struct buf { char *p; size_t cap, len; };
static void bput(struct buf *b, char c) { if (b->len < b->cap) b->p[b->len] = c; b->len++; }
static void bputs(struct buf *b, const char *s, size_t n) { while (n--) bput(b, *s++); }

static int u64_to(char *out, unsigned long long v, int base, int upper)
{
    static const char *lo = "0123456789abcdef", *up = "0123456789ABCDEF";
    const char *d = upper ? up : lo;
    char tmp[32]; int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = d[v % base]; v /= base; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static void emit_uint(struct buf *b, unsigned long long v)
{ char t[24]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; } while (n--) bput(b, t[n]); }

/* %g strips trailing zeros from the mantissa (and a bare '.'), keeping any
 * exponent suffix. Operates in place on a rendered, sign-less body. */
static void trim_g(struct buf *lb)
{
    char *s = lb->p; int len = (int)lb->len, ee = -1, dot = -1;
    for (int i = 0; i < len; i++) if (s[i] == 'e' || s[i] == 'E') { ee = i; break; }
    int mend = ee < 0 ? len : ee;          /* mantissa = s[0..mend) */
    for (int i = 0; i < mend; i++) if (s[i] == '.') { dot = i; break; }
    if (dot < 0) return;                    /* no fraction -> nothing to trim */
    int last = mend - 1;
    while (last > dot && s[last] == '0') last--;
    if (last == dot) last--;                /* fraction emptied -> drop the '.' */
    int keep = last + 1;
    if (ee >= 0) { for (int i = ee; i < len; i++) s[keep + (i - ee)] = s[i]; lb->len = (size_t)(keep + (len - ee)); }
    else lb->len = (size_t)keep;
}

/* Correct %f / %e / %g — QuickJS's js_dtoa formats numbers via snprintf("%+.*e"),
 * so this must be right for JS Number->string to work. The numeric body is
 * rendered into a local buffer so %g can post-trim; %e/%f are copied verbatim. */
static void fmt_float(struct buf *b, double v, int prec, char conv, int plus, int space, int alt)
{
    int upper = conv <= 'Z';
    char lc = conv | 0x20;
    int gconv = (lc == 'g');
    if (v != v) { bputs(b, upper ? "NAN" : "nan", 3); return; }
    char sgn = 0;
    if (v < 0 || (v == 0 && __builtin_signbit(v))) { sgn = '-'; v = -v; }
    else if (plus) sgn = '+'; else if (space) sgn = ' ';
    double inf = 1e308 * 10;
    if (v > inf - 1) { if (sgn) bput(b, sgn); bputs(b, upper ? "INF" : "inf", 3); return; }
    if (prec < 0) prec = 6;

    int exp = 0; double t = v;
    if (t != 0) { while (t >= 10.0) { t /= 10; exp++; } while (t < 1.0) { t *= 10; exp--; } }

    if (lc == 'g') {
        int P = prec ? prec : 1;
        if (exp < -4 || exp >= P) { lc = 'e'; prec = P - 1; }
        else { lc = 'f'; prec = P - 1 - exp; if (prec < 0) prec = 0; }
    }

    char nb[96]; struct buf lb = { nb, sizeof nb, 0 };
    if (lc == 'e') {
        double m = v; int e = 0;
        if (m != 0) { while (m >= 10.0) { m /= 10; e++; } while (m < 1.0) { m *= 10; e--; } }
        int nd = prec + 1; if (nd > 23) nd = 23;
        int d[24];
        for (int i = 0; i < nd; i++) { int di = (int)m; if (di < 0) di = 0; if (di > 9) di = 9; d[i] = di; m = (m - di) * 10; }
        if (m > 5.0 || (m == 5.0 && (d[nd - 1] & 1))) {   /* round half to even */
            int i = nd - 1; for (; i >= 0; i--) { if (++d[i] < 10) break; d[i] = 0; } if (i < 0) { d[0] = 1; e++; } }
        bput(&lb, (char)('0' + d[0]));
        if (prec > 0) { bput(&lb, '.'); for (int i = 1; i < nd; i++) bput(&lb, (char)('0' + d[i])); }
        bput(&lb, upper ? 'E' : 'e');
        int es = e < 0, ea = es ? -e : e;
        bput(&lb, es ? '-' : '+');
        if (ea < 10) bput(&lb, '0');
        emit_uint(&lb, (unsigned long long)ea);
    } else {                                   /* 'f' */
        unsigned long long ip = (unsigned long long)v;
        double frac = v - (double)ip;
        int np = prec > 39 ? 39 : prec; int fd[40];
        for (int i = 0; i < np; i++) { frac *= 10; int di = (int)frac; if (di < 0) di = 0; if (di > 9) di = 9; fd[i] = di; frac -= di; }
        int f_odd = np ? (fd[np - 1] & 1) : (int)(ip & 1ULL);   /* round half to even */
        if (frac > 0.5 || (frac == 0.5 && f_odd)) { int i = np - 1; for (; i >= 0; i--) { if (++fd[i] < 10) break; fd[i] = 0; } if (i < 0) ip++; }
        emit_uint(&lb, ip);
        if (prec > 0) { bput(&lb, '.'); for (int i = 0; i < np; i++) bput(&lb, (char)('0' + fd[i])); }
    }
    if (gconv && !alt) trim_g(&lb);
    if (sgn) bput(b, sgn);
    bputs(b, lb.p, lb.len);
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    struct buf b = { out, cap ? cap : 0, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { bput(&b, *fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1; else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1; else break;
        }
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; if (width < 0) { left = 1; width = -width; } }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') { fmt++; prec = 0; if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
                           else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        int lng = 0;
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't') { if (*fmt == 'l') lng++; if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 2; fmt++; }

        char num[40]; const char *body = num; size_t blen = 0; char sign = 0; const char *prefix = "";
        char c = *fmt;
        switch (c) {
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long long u; if (v < 0) { sign = '-'; u = (unsigned long long)(-v); } else { u = (unsigned long long)v; if (plus) sign = '+'; else if (space) sign = ' '; }
            blen = u64_to(num, u, 10, 0); break; }
        case 'u': { unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); blen = u64_to(num, v, 10, 0); break; }
        case 'o': { unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); blen = u64_to(num, v, 8, 0); break; }
        case 'x': case 'X': { unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); blen = u64_to(num, v, 16, c == 'X'); if (alt && v) prefix = c == 'X' ? "0X" : "0x"; break; }
        case 'p': { unsigned long long v = (unsigned long long)(size_t)va_arg(ap, void *); num[0] = '0'; num[1] = 'x'; blen = 2 + u64_to(num + 2, v, 16, 0); body = num; bputs(&b, num, blen); continue; }
        case 'c': { num[0] = (char)va_arg(ap, int); blen = 1; break; }
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; size_t n = prec >= 0 ? snlen(s, prec) : strlen(s); size_t pad = (size_t)width > n ? width - n : 0; if (!left) while (pad--) bput(&b, ' '); bputs(&b, s, n); if (left) while (pad--) bput(&b, ' '); continue; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': { double d = va_arg(ap, double); fmt_float(&b, d, prec, c, plus, space, alt); continue; }
        case '%': bput(&b, '%'); continue;
        case 0: goto done;
        default: bput(&b, '%'); bput(&b, c); continue;
        }
        /* emit integer body with sign/prefix/width/zero-pad */
        size_t total = blen + (sign ? 1 : 0) + strlen(prefix);
        size_t pad = (size_t)width > total ? width - total : 0;
        if (!left && !zero) while (pad--) bput(&b, ' ');
        if (sign) bput(&b, sign);
        bputs(&b, prefix, strlen(prefix));
        if (!left && zero) while (pad--) bput(&b, '0');
        bputs(&b, body, blen);
        if (left) while (pad--) bput(&b, ' ');
    }
done:
    if (cap) b.p[b.len < cap ? b.len : cap - 1] = 0;
    return (int)b.len;
}

int snprintf(char *out, size_t cap, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(out, cap, fmt, ap); va_end(ap); return r; }
int vsprintf(char *out, const char *fmt, va_list ap) { return vsnprintf(out, (size_t)1 << 30, fmt, ap); }
int sprintf(char *out, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); int r = vsnprintf(out, (size_t)1 << 30, fmt, ap); va_end(ap); return r; }

/* Format then write. Output longer than the stack buffer is sized exactly via
 * malloc (a re-format with a va_copy) instead of being silently truncated. */
static int vlog(int fd, const char *fmt, va_list ap)
{
    char tmp[1024];
    va_list ap2; va_copy(ap2, ap);
    int r = vsnprintf(tmp, sizeof tmp, fmt, ap);
    if (r < 0) { va_end(ap2); return r; }
    if (r < (int)sizeof tmp) { wr(fd, tmp, (size_t)r); va_end(ap2); return r; }
    char *big = (char *)malloc((size_t)r + 1);
    if (big) { vsnprintf(big, (size_t)r + 1, fmt, ap2); wr(fd, big, (size_t)r); free(big); }
    else wr(fd, tmp, sizeof tmp - 1);           /* OOM: emit the truncated prefix */
    va_end(ap2);
    return r;
}

int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vlog(1, fmt, ap); va_end(ap); return r; }
int vprintf(const char *fmt, va_list ap) { return vlog(1, fmt, ap); }
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vlog(f ? f->fd : 2, fmt, ap); va_end(ap); return r; }
int vfprintf(FILE *f, const char *fmt, va_list ap) { return vlog(f ? f->fd : 2, fmt, ap); }
int putchar(int c) { unsigned char ch = (unsigned char)c; return wr(1, (const char *)&ch, 1) < 0 ? EOF : (int)ch; }
int puts(const char *s) { return (wr(1, s, strlen(s)) < 0 || wr(1, "\n", 1) < 0) ? EOF : 0; }
int fputc(int c, FILE *f)
{ unsigned char ch = (unsigned char)c; if (wr(f ? f->fd : 1, (const char *)&ch, 1) < 0) { if (f) f->flags |= F_ERR; return EOF; } return (int)ch; }
int fputs(const char *s, FILE *f)
{ if (wr(f ? f->fd : 1, s, strlen(s)) < 0) { if (f) f->flags |= F_ERR; return EOF; } return 0; }
size_t fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (sz == 0 || n == 0) return 0;
    size_t total = sz * n, off = 0; const char *cp = (const char *)p;
    while (off < total) { long k = write(f ? f->fd : 1, cp + off, total - off); if (k <= 0) { if (f) f->flags |= F_ERR; break; } off += (size_t)k; }
    return off / sz;
}
int fflush(FILE *f) { (void)f; return 0; }   /* output is unbuffered */

/* ---------- input (buffered) + file open/close/seek ---------- */
static int refill(FILE *f)
{
    if (!f->rbuf) { f->rbuf = (unsigned char *)malloc(RBUFSZ); f->rcap = RBUFSZ; }
    if (!f->rbuf) { f->flags |= F_ERR; return -1; }
    long r = read(f->fd, f->rbuf, f->rcap);
    if (r <= 0) { f->rlen = f->rpos = 0; f->flags |= F_EOF; return -1; }
    f->rlen = (int)r; f->rpos = 0;
    return 0;
}

int fgetc(FILE *f)
{
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->rpos >= f->rlen && refill(f) < 0) return EOF;
    return f->rbuf[f->rpos++];
}
int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }
int ungetc(int c, FILE *f) { if (c == EOF) return EOF; f->ungot = (unsigned char)c; f->flags &= ~F_EOF; return c; }

size_t fread(void *ptr, size_t sz, size_t n, FILE *f)
{
    size_t total = sz * n, got = 0; unsigned char *out = (unsigned char *)ptr;
    if (sz == 0) return 0;
    while (got < total) { int c = fgetc(f); if (c == EOF) break; out[got++] = (unsigned char)c; }
    return got / sz;
}

char *fgets(char *s, int size, FILE *f)
{
    int i = 0;
    if (size <= 0) return NULL;
    while (i < size - 1) { int c = fgetc(f); if (c == EOF) break; s[i++] = (char)c; if (c == '\n') break; }
    if (i == 0) return NULL;       /* EOF with nothing read */
    s[i] = 0;
    return s;
}

int   feof(FILE *f)   { return (f->flags & F_EOF) != 0; }
int   ferror(FILE *f) { return (f->flags & F_ERR) != 0; }
void  clearerr(FILE *f) { f->flags &= ~(F_EOF | F_ERR); }
int   fileno(FILE *f) { return f->fd; }

int fseek(FILE *f, long off, int whence)
{
    long r = lseek(f->fd, off, whence);
    if (r < 0) return -1;
    f->rpos = f->rlen = 0; f->ungot = -1; f->flags &= ~F_EOF;
    return 0;
}
long ftell(FILE *f) { long r = lseek(f->fd, 0, SEEK_CUR); return r < 0 ? -1 : r - (f->rlen - f->rpos); }
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); f->flags &= ~F_ERR; }

static int mode_flags(const char *m, int *libc_flags)
{
    int o, lf;
    if (m[0] == 'r') { o = O_RDONLY; lf = F_READ; }
    else if (m[0] == 'w') { o = O_WRONLY | O_CREAT | O_TRUNC; lf = F_WRITE; }
    else if (m[0] == 'a') { o = O_WRONLY | O_CREAT | O_APPEND; lf = F_WRITE; }
    else return -1;
    for (const char *p = m; *p; p++) if (*p == '+') { o = (o & ~3) | O_RDWR; lf = F_READ | F_WRITE; }
    *libc_flags = lf;
    return o;
}

static FILE *file_wrap(int fd, int lf)
{
    FILE *f = (FILE *)malloc(sizeof *f);
    if (!f) { close(fd); return NULL; }
    f->fd = fd; f->flags = lf | F_ALLOC; f->rbuf = NULL; f->rcap = f->rpos = f->rlen = 0; f->ungot = -1;
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int lf, oflags = mode_flags(mode, &lf);
    if (oflags < 0) return NULL;
    int fd = open(path, oflags);
    if (fd < 0) return NULL;
    return file_wrap(fd, lf);
}
FILE *fdopen(int fd, const char *mode) { int lf; if (mode_flags(mode, &lf) < 0) return NULL; return file_wrap(fd, lf); }

int fclose(FILE *f)
{
    if (!f) return EOF;
    int fd = f->fd;
    if (f->flags & F_ALLOC) { if (f->rbuf) free(f->rbuf); close(fd); free(f); }
    return 0;
}
int remove(const char *path) { return unlink(path); }
