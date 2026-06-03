#include <stddef.h>
#include <stdarg.h>

#define SYS_WRITE 1
static long sys(long n, long a, long b, long c)
{ long r; __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory"); return r; }

size_t strlen(const char *);
static size_t snlen(const char *s, int m) { int i = 0; while (i < m && s[i]) i++; return (size_t)i; }

/* ---- the FILE type is opaque; we only distinguish stdout/stderr ---- */
struct _FILE { int fd; };
static struct _FILE _stdout = { 1 }, _stderr = { 2 }, _stdin = { 0 };
struct _FILE *stdout = &_stdout, *stderr = &_stderr, *stdin = &_stdin;

static void console(const char *s, int len) { sys(SYS_WRITE, 1, (long)s, len); }

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

/* Correct %f / %e / %g — QuickJS's js_dtoa formats numbers via snprintf("%+.*e"),
 * so this must be right for JS Number->string to work. */
static void fmt_float(struct buf *b, double v, int prec, char conv, int plus, int space)
{
    int upper = conv <= 'Z';
    char lc = conv | 0x20;
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
    if (sgn) bput(b, sgn);

    if (lc == 'e') {
        double m = v; int e = 0;
        if (m != 0) { while (m >= 10.0) { m /= 10; e++; } while (m < 1.0) { m *= 10; e--; } }
        int nd = prec + 1; if (nd > 23) nd = 23;
        int d[24];
        for (int i = 0; i < nd; i++) { int di = (int)m; if (di < 0) di = 0; if (di > 9) di = 9; d[i] = di; m = (m - di) * 10; }
        if (m >= 5.0) { int i = nd - 1; for (; i >= 0; i--) { if (++d[i] < 10) break; d[i] = 0; } if (i < 0) { d[0] = 1; e++; } }
        bput(b, (char)('0' + d[0]));
        if (prec > 0) { bput(b, '.'); for (int i = 1; i < nd; i++) bput(b, (char)('0' + d[i])); }
        bput(b, upper ? 'E' : 'e');
        int es = e < 0, ea = es ? -e : e;
        bput(b, es ? '-' : '+');
        if (ea < 10) bput(b, '0');
        emit_uint(b, (unsigned long long)ea);
    } else {                                   /* 'f' */
        unsigned long long ip = (unsigned long long)v;
        double frac = v - (double)ip;
        int np = prec > 39 ? 39 : prec; int fd[40];
        for (int i = 0; i < np; i++) { frac *= 10; int di = (int)frac; if (di < 0) di = 0; if (di > 9) di = 9; fd[i] = di; frac -= di; }
        if (frac >= 0.5) { int i = np - 1; for (; i >= 0; i--) { if (++fd[i] < 10) break; fd[i] = 0; } if (i < 0) ip++; }
        emit_uint(b, ip);
        if (prec > 0) { bput(b, '.'); for (int i = 0; i < np; i++) bput(b, (char)('0' + fd[i])); }
    }
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
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = -1;
        if (*fmt == '.') { fmt++; prec = 0; if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
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
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': { double d = va_arg(ap, double); fmt_float(&b, d, prec, c, plus, space); continue; }
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

static int vlog(int fd, const char *fmt, va_list ap)
{ char tmp[2048]; int r = vsnprintf(tmp, sizeof tmp, fmt, ap); int n = r < (int)sizeof tmp ? r : (int)sizeof tmp - 1; (void)fd; console(tmp, n); return r; }

int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vlog(1, fmt, ap); va_end(ap); return r; }
int vprintf(const char *fmt, va_list ap) { return vlog(1, fmt, ap); }
int fprintf(struct _FILE *f, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vlog(f ? f->fd : 2, fmt, ap); va_end(ap); return r; }
int vfprintf(struct _FILE *f, const char *fmt, va_list ap) { return vlog(f ? f->fd : 2, fmt, ap); }
int putchar(int c) { char ch = (char)c; console(&ch, 1); return c; }
int puts(const char *s) { console(s, (int)strlen(s)); console("\n", 1); return 0; }
int fputc(int c, struct _FILE *f) { (void)f; char ch = (char)c; console(&ch, 1); return c; }
int fputs(const char *s, struct _FILE *f) { (void)f; console(s, (int)strlen(s)); return 0; }
size_t fwrite(const void *p, size_t sz, size_t n, struct _FILE *f) { (void)f; console((const char *)p, (int)(sz * n)); return n; }
int fflush(struct _FILE *f) { (void)f; return 0; }
