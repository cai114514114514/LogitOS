#ifndef AQUA_CLIB_H
#define AQUA_CLIB_H

/* Tiny header-only support library for the CLI programs (sh + coreutils): the
 * string + stdio helpers they all need, on top of the raw syscalls in aqua.h.
 * Header-only (static inline) so each program just #includes it -- no separate
 * libc link, and none of the heavy mini-libc arena. */

#include "aqua.h"

static inline int   c_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline int   c_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static inline int   c_strncmp(const char *a, const char *b, int n)
{ for (int i = 0; i < n; i++) { if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i]; if (!a[i]) break; } return 0; }
static inline void  c_strcpy(char *d, const char *s, int max)
{ int i = 0; for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }
static inline int   c_atoi(const char *s)
{ int v = 0, sign = 1; if (*s == '-') { sign = -1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v * sign; }

/* output to a given fd */
static inline void  fputs_fd(int fd, const char *s) { sys_write(fd, s, c_strlen(s)); }
static inline void  outs(const char *s) { fputs_fd(1, s); }
static inline void  errs(const char *s) { fputs_fd(2, s); }
static inline void  outc(char c) { sys_write(1, &c, 1); }
static inline void  outn_fd(int fd, long v)
{
    char t[24]; int i = 0;
    if (v < 0) { sys_write(fd, "-", 1); v = -v; }
    if (!v) { sys_write(fd, "0", 1); return; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    char o[24]; int k = 0; while (i) o[k++] = t[--i];
    sys_write(fd, o, k);
}
static inline void  outn(long v) { outn_fd(1, v); }

/* Read one line from fd into buf (NUL-terminated, newline stripped). Returns the
 * length, or -1 at EOF with nothing read. Byte-at-a-time: stdin is a tty/pipe. */
static inline int   readline(int fd, char *buf, int max)
{
    int n = 0;
    for (;;) {
        char c;
        int r = sys_read(fd, &c, 1);
        if (r <= 0) { if (n == 0) return -1; break; }   /* EOF */
        if (c == '\n') break;
        if (c == '\b' || c == 127) { if (n > 0) n--; continue; }
        if (n < max - 1) buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

#endif /* AQUA_CLIB_H */
