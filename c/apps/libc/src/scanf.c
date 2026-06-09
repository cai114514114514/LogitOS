/* mini-libc: sscanf / vsscanf -- formatted input from a string.
 * Supports: %d %i %u %o %x/%X (with h/hh/l/ll length mods), %f/%e/%g (and l/L
 * for double), %s, %c, %%, %n, '*' assignment-suppression, and field width.
 * Whitespace in the format matches any run of input whitespace; a literal char
 * must match. Returns the number of items assigned, or EOF (-1) if input ends
 * before the first conversion. (No %[scanset] / %p yet.) */
#include <stddef.h>
#include <stdarg.h>

long long          strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double             strtod(const char *, char **);

#define EOF (-1)
static int sp(int c) { return c == ' ' || (c >= 9 && c <= 13); }

/* copy up to `w` chars of the current input into buf for a bounded strto* parse */
static int bounded(const char *s, int w, char *buf, int cap)
{
    int k = 0;
    if (w <= 0 || w >= cap) w = cap - 1;
    while (k < w && s[k]) { buf[k] = s[k]; k++; }
    buf[k] = 0;
    return k;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    int assigned = 0;
    const char *s0 = s;                  /* for %n + EOF tracking */
    const char *p = fmt;
    while (*p) {
        if (sp(*p)) { while (sp(*p)) p++; while (sp(*s)) s++; continue; }
        if (*p != '%') { if (*s != *p) return assigned; s++; p++; continue; }
        p++;
        if (*p == '%') { while (sp(*s)) s++; if (*s != '%') return assigned; s++; p++; continue; }

        int suppress = 0; if (*p == '*') { suppress = 1; p++; }
        int width = 0; while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int lmod = 0;                                   /* -2=hh -1=h 0=int 1=l 2=ll */
        for (;;) {
            if (*p == 'l') { lmod = (lmod >= 1) ? 2 : 1; p++; }
            else if (*p == 'h') { lmod = (lmod <= -1) ? -2 : -1; p++; }
            else if (*p == 'L') { lmod = 1; p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        if (!conv) break;

        char buf[128]; char *e;

        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            while (sp(*s)) s++;
            int base = (conv == 'd' || conv == 'u') ? 10 : (conv == 'i') ? 0 : (conv == 'o') ? 8 : 16;
            int uns  = (conv == 'u' || conv == 'o' || conv == 'x' || conv == 'X');
            int n = bounded(s, width, buf, (int)sizeof buf); (void)n;
            if (uns) {
                unsigned long long v = strtoull(buf, &e, base);
                if (e == buf) return assigned;
                if (!suppress) {
                    if (lmod <= -2) *va_arg(ap, unsigned char *) = (unsigned char)v;
                    else if (lmod == -1) *va_arg(ap, unsigned short *) = (unsigned short)v;
                    else if (lmod == 0) *va_arg(ap, unsigned int *) = (unsigned int)v;
                    else if (lmod == 1) *va_arg(ap, unsigned long *) = (unsigned long)v;
                    else *va_arg(ap, unsigned long long *) = v;
                    assigned++;
                }
            } else {
                long long v = strtoll(buf, &e, base);
                if (e == buf) return assigned;
                if (!suppress) {
                    if (lmod <= -2) *va_arg(ap, signed char *) = (signed char)v;
                    else if (lmod == -1) *va_arg(ap, short *) = (short)v;
                    else if (lmod == 0) *va_arg(ap, int *) = (int)v;
                    else if (lmod == 1) *va_arg(ap, long *) = (long)v;
                    else *va_arg(ap, long long *) = v;
                    assigned++;
                }
            }
            s += (e - buf);
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            while (sp(*s)) s++;
            bounded(s, width, buf, (int)sizeof buf);
            double v = strtod(buf, &e);
            if (e == buf) return assigned;
            if (!suppress) {
                if (lmod >= 1) *va_arg(ap, double *) = v;
                else *va_arg(ap, float *) = (float)v;
                assigned++;
            }
            s += (e - buf);
            break;
        }
        case 's': {
            while (sp(*s)) s++;
            if (!*s) return assigned;                    /* nothing to read */
            char *out = suppress ? 0 : va_arg(ap, char *);
            int w = (width > 0) ? width : 0x7fffffff, k = 0;
            while (s[k] && !sp(s[k]) && k < w) { if (out) out[k] = s[k]; k++; }
            if (out) out[k] = 0;
            s += k; if (!suppress) assigned++;
            break;
        }
        case 'c': {
            int w = (width > 0) ? width : 1;
            if (!*s) return assigned;
            char *out = suppress ? 0 : va_arg(ap, char *);
            int k = 0;
            while (s[k] && k < w) { if (out) out[k] = s[k]; k++; }
            if (k < w) return assigned;                  /* fewer than requested -> matching failure */
            s += k; if (!suppress) assigned++;
            break;
        }
        case 'n':
            if (!suppress) *va_arg(ap, int *) = (int)(s - s0);  /* chars consumed so far */
            break;
        default:
            return assigned;                             /* unknown conversion */
        }
    }    return assigned;
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}
