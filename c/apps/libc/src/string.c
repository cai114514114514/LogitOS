#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include "libc_internal.h"

static int lc_(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int strcasecmp(const char *a, const char *b)
{ while (*a && lc_((unsigned char)*a) == lc_((unsigned char)*b)) { a++; b++; }
  return lc_((unsigned char)*a) - lc_((unsigned char)*b); }
int strncasecmp(const char *a, const char *b, size_t n)
{ for (; n; n--, a++, b++) { int d = lc_((unsigned char)*a) - lc_((unsigned char)*b); if (d || !*a) return d; } return 0; }

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    /* Word-at-a-time bulk copy only when both ends share alignment. */
    if (n >= sizeof(uint64_t) &&
        (((uintptr_t)dd ^ (uintptr_t)ss) & (sizeof(uint64_t) - 1)) == 0) {
        while (((uintptr_t)dd & (sizeof(uint64_t) - 1)) && n) { *dd++ = *ss++; n--; }
        while (n >= sizeof(uint64_t)) {
            *(uint64_t *)dd = *(const uint64_t *)ss;
            dd += sizeof(uint64_t); ss += sizeof(uint64_t); n -= sizeof(uint64_t);
        }
    }
    while (n--) *dd++ = *ss++;
    return d;
}
void *mempcpy(void *d, const void *s, size_t n) { return (char *)memcpy(d, s, n) + n; }

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    if (dd == ss) return d;
    if (dd < ss) {
        if (n >= sizeof(uint64_t) &&
            (((uintptr_t)dd ^ (uintptr_t)ss) & (sizeof(uint64_t) - 1)) == 0) {
            while (((uintptr_t)dd & (sizeof(uint64_t) - 1)) && n) { *dd++ = *ss++; n--; }
            while (n >= sizeof(uint64_t)) {
                *(uint64_t *)dd = *(const uint64_t *)ss;
                dd += sizeof(uint64_t); ss += sizeof(uint64_t); n -= sizeof(uint64_t);
            }
        }
        while (n--) *dd++ = *ss++;
    }
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void *memset(void *d, int c, size_t n)
{
    unsigned char *dd = d;
    if (n >= sizeof(uint64_t)) {
        uint64_t w = (uint64_t)(unsigned char)c * 0x0101010101010101ULL;
        while (((uintptr_t)dd & (sizeof(uint64_t) - 1)) && n) { *dd++ = (unsigned char)c; n--; }
        while (n >= sizeof(uint64_t)) {
            *(uint64_t *)dd = w;
            dd += sizeof(uint64_t); n -= sizeof(uint64_t);
        }
    }
    while (n--) *dd++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}
void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) { if (*--p == (unsigned char)c) return (void *)p; }
    return NULL;
}
void *memccpy(void *d, const void *s, int c, size_t n)
{
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) { if ((*dd++ = *ss++) == (unsigned char)c) return dd; }
    return NULL;
}

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t m) { size_t i = 0; while (i < m && s[i]) i++; return i; }
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{ for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) break; } return 0; }

/* In the "C" locale -- the only locale this library has, see locale.c -- the
 * collation sequence IS the character-code order, so strcoll is strcmp and
 * strxfrm is a bounded copy. These are the standard-mandated degenerate forms,
 * not placeholders: a conforming C-locale program cannot tell them from a
 * table-driven implementation. */
int strcoll(const char *a, const char *b) { return strcmp(a, b); }
size_t strxfrm(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(d, s, k); d[k] = 0; }
    return l;
}

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *stpcpy(char *d, const char *s) { while ((*d = *s)) { d++; s++; } return d; }
char *strncpy(char *d, const char *s, size_t n)
{ char *r = d; while (n && (*d = *s)) { d++; s++; n--; } while (n--) *d++ = 0; return r; }
char *stpncpy(char *d, const char *s, size_t n)
{
    char *end;
    while (n && *s) { *d++ = *s++; n--; }
    end = d;
    while (n--) *d++ = 0;
    return end;
}
char *strcat(char *d, const char *s) { char *r = d; while (*d) d++; while ((*d++ = *s++)) ; return r; }
char *strncat(char *d, const char *s, size_t n)
{ char *r = d; while (*d) d++; while (n-- && *s) *d++ = *s++; *d = 0; return r; }

char *strchr(const char *s, int c)
{ for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strchrnul(const char *s, int c)
{ for (;; s++) { if (*s == (char)c || !*s) return (char *)s; } }
char *strrchr(const char *s, int c)
{ const char *last = NULL; do { if (*s == (char)c) last = s; } while (*s++); return (char *)last; }
char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }

char *strstr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}
char *strcasestr(const char *h, const char *n)
{
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *b && lc_((unsigned char)*a) == lc_((unsigned char)*b)) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return NULL;
}

size_t strspn(const char *s, const char *set)
{ size_t i = 0; for (; s[i]; i++) { const char *p = set; while (*p && *p != s[i]) p++; if (!*p) break; } return i; }
size_t strcspn(const char *s, const char *set)
{ size_t i = 0; for (; s[i]; i++) { const char *p = set; while (*p && *p != s[i]) p++; if (*p) break; } return i; }
char *strpbrk(const char *s, const char *set)
{ s += strcspn(s, set); return *s ? (char *)s : NULL; }

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);            /* skip leading delimiters */
    if (!*s) { *save = s; return NULL; }
    char *tok = s;
    s += strcspn(s, delim);           /* run to next delimiter */
    if (*s) { *s = 0; *save = s + 1; }
    else      *save = s;
    return tok;
}
char *strtok(char *s, const char *delim)
{ static char *save; return strtok_r(s, delim, &save); }

void *memmem(const void *hay, size_t hn, const void *ndl, size_t nn)
{
    const unsigned char *h = hay, *n = ndl;
    if (nn == 0) return (void *)hay;
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && memcmp(h + i, n, nn) == 0) return (void *)(h + i);
    return NULL;
}

char *strdup(const char *s)
{ size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); else errno = ENOMEM; return p; }
char *strndup(const char *s, size_t m)
{ size_t n = strnlen(s, m); char *p = malloc(n + 1); if (p) { memcpy(p, s, n); p[n] = 0; } else errno = ENOMEM; return p; }

/* ---------------------------------------------------------------------- */
/* strerror                                                               */
/* ---------------------------------------------------------------------- */
/* The old one returned the string "error" for every code, which is the exact
 * shape of failure this library is meant to stop having: perror() and
 * strerror() are how a C program tells its user WHICH thing went wrong, and a
 * constant makes every failure look the same. Wording follows glibc's, because
 * that is what a user reading the message will have seen before. */
struct emsg { int e; const char *s; };
static const struct emsg emsgs[] = {
    { 0,          "Success" },
    { EPERM,      "Operation not permitted" },
    { ENOENT,     "No such file or directory" },
    { ESRCH,      "No such process" },
    { EINTR,      "Interrupted system call" },
    { EIO,        "Input/output error" },
    { ENXIO,      "No such device or address" },
    { E2BIG,      "Argument list too long" },
    { ENOEXEC,    "Exec format error" },
    { EBADF,      "Bad file descriptor" },
    { ECHILD,     "No child processes" },
    { EAGAIN,     "Resource temporarily unavailable" },
    { ENOMEM,     "Cannot allocate memory" },
    { EACCES,     "Permission denied" },
    { EFAULT,     "Bad address" },
    { EBUSY,      "Device or resource busy" },
    { EEXIST,     "File exists" },
    { EXDEV,      "Invalid cross-device link" },
    { ENODEV,     "No such device" },
    { ENOTDIR,    "Not a directory" },
    { EISDIR,     "Is a directory" },
    { EINVAL,     "Invalid argument" },
    { ENFILE,     "Too many open files in system" },
    { EMFILE,     "Too many open files" },
    { ENOTTY,     "Inappropriate ioctl for device" },
    { EFBIG,      "File too large" },
    { ENOSPC,     "No space left on device" },
    { ESPIPE,     "Illegal seek" },
    { EROFS,      "Read-only file system" },
    { EMLINK,     "Too many links" },
    { EPIPE,      "Broken pipe" },
    { EDOM,       "Numerical argument out of domain" },
    { ERANGE,     "Numerical result out of range" },
    { EDEADLK,    "Resource deadlock avoided" },
    { ENAMETOOLONG, "File name too long" },
    { ENOLCK,     "No locks available" },
    { ENOSYS,     "Function not implemented" },
    { ENOTEMPTY,  "Directory not empty" },
    { ELOOP,      "Too many levels of symbolic links" },
    { ENOMSG,     "No message of desired type" },
    { EILSEQ,     "Invalid or incomplete multibyte or wide character" },
    { EOVERFLOW,  "Value too large for defined data type" },
    { ENOTSOCK,   "Socket operation on non-socket" },
    { EMSGSIZE,   "Message too long" },
    { EPROTOTYPE, "Protocol wrong type for socket" },
    { ENOPROTOOPT, "Protocol not available" },
    { EPROTONOSUPPORT, "Protocol not supported" },
    { EAFNOSUPPORT, "Address family not supported by protocol" },
    { EADDRINUSE, "Address already in use" },
    { EADDRNOTAVAIL, "Cannot assign requested address" },
    { ENETDOWN,   "Network is down" },
    { ENETUNREACH, "Network is unreachable" },
    { ECONNABORTED, "Software caused connection abort" },
    { ECONNRESET, "Connection reset by peer" },
    { ENOBUFS,    "No buffer space available" },
    { EISCONN,    "Transport endpoint is already connected" },
    { ENOTCONN,   "Transport endpoint is not connected" },
    { ETIMEDOUT,  "Connection timed out" },
    { ECONNREFUSED, "Connection refused" },
    { EHOSTUNREACH, "No route to host" },
    { EINPROGRESS, "Operation now in progress" },
    { ENOTSUP,    "Operation not supported" },
    { ECANCELED,  "Operation canceled" },
};

char *strerror(int e)
{
    static char unknown[40];
    for (size_t i = 0; i < sizeof emsgs / sizeof *emsgs; i++)
        if (emsgs[i].e == e) return (char *)emsgs[i].s;
    /* Not a code we know: say so, and say which -- never a generic word. */
    const char *pre = "Unknown error ";
    size_t n = 0;
    while (pre[n]) { unknown[n] = pre[n]; n++; }
    int v = e; char t[16]; int k = 0;
    if (v < 0) { unknown[n++] = '-'; v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k--) unknown[n++] = t[k];
    unknown[n] = 0;
    return unknown;
}

int strerror_r(int e, char *buf, size_t n)
{
    const char *s = strerror(e);
    size_t l = strlen(s);
    if (n == 0) return ERANGE;
    if (l >= n) { memcpy(buf, s, n - 1); buf[n - 1] = 0; return ERANGE; }
    memcpy(buf, s, l + 1);
    return 0;
}

static const struct emsg sigmsgs[] = {
    { SIGHUP, "Hangup" }, { SIGINT, "Interrupt" }, { SIGQUIT, "Quit" },
    { SIGILL, "Illegal instruction" }, { SIGTRAP, "Trace/breakpoint trap" },
    { SIGABRT, "Aborted" }, { SIGBUS, "Bus error" },
    { SIGFPE, "Floating point exception" }, { SIGKILL, "Killed" },
    { SIGUSR1, "User defined signal 1" }, { SIGSEGV, "Segmentation fault" },
    { SIGUSR2, "User defined signal 2" }, { SIGPIPE, "Broken pipe" },
    { SIGALRM, "Alarm clock" }, { SIGTERM, "Terminated" },
    { SIGCHLD, "Child exited" }, { SIGCONT, "Continued" }, { SIGSTOP, "Stopped (signal)" },
};
char *strsignal(int s)
{
    for (size_t i = 0; i < sizeof sigmsgs / sizeof *sigmsgs; i++)
        if (sigmsgs[i].e == s) return (char *)sigmsgs[i].s;
    return (char *)"Unknown signal";
}

/* ---------------------------------------------------------------------- */
/* BSD / POSIX extras                                                     */
/* ---------------------------------------------------------------------- */
/* BSD strlcpy/strlcat: size is the FULL dst buffer; always NUL-terminate when
 * size>0; return the length they tried to build (>= size means truncation). */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t sl = strlen(src);
    if (size) { size_t n = sl < size - 1 ? sl : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return sl;
}
size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dl = strnlen(dst, size), sl = strlen(src);
    if (dl == size) return size + sl;                 /* dst already full / unterminated */
    size_t n = sl < size - dl - 1 ? sl : size - dl - 1;
    memcpy(dst + dl, src, n); dst[dl + n] = 0;
    return dl + sl;
}
/* strsep: return the token before the next delimiter; advance *sp past it (NUL
 * the delimiter). Unlike strtok it returns empty tokens and is reentrant. */
char *strsep(char **sp, const char *delim)
{
    char *s = *sp;
    if (!s) return 0;
    for (char *p = s; *p; p++)
        for (const char *d = delim; *d; d++)
            if (*p == *d) { *p = 0; *sp = p + 1; return s; }
    *sp = 0;
    return s;
}

/* strverscmp: "file10" sorts after "file9". Used by anything that lists
 * version-numbered names; the GNU semantics are followed (digit runs compare
 * numerically, leading zeros make a fractional part that sorts first). */
int strverscmp(const char *a, const char *b)
{
    const unsigned char *s1 = (const unsigned char *)a, *s2 = (const unsigned char *)b;
    size_t i = 0;
    while (s1[i] && s1[i] == s2[i]) i++;
    if (s1[i] == s2[i]) return 0;
    size_t start = i;
    while (start > 0 && s1[start - 1] >= '0' && s1[start - 1] <= '9') start--;
    int d1 = s1[i] >= '0' && s1[i] <= '9', d2 = s2[i] >= '0' && s2[i] <= '9';
    if (!d1 && !d2) return (int)s1[i] - (int)s2[i];
    if (start < i || (d1 && d2)) {
        int lead = (start < i) ? (s1[start] == '0') : (d1 && s1[i] == '0') || (d2 && s2[i] == '0');
        size_t e1 = i, e2 = i;
        while (s1[e1] >= '0' && s1[e1] <= '9') e1++;
        while (s2[e2] >= '0' && s2[e2] <= '9') e2++;
        if (lead) return (int)s1[i] - (int)s2[i];      /* fractional: compare digitwise */
        if (e1 != e2) return e1 < e2 ? -1 : 1;
        return (int)s1[i] - (int)s2[i];
    }
    return (int)s1[i] - (int)s2[i];
}

int  bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
void bcopy(const void *s, void *d, size_t n) { memmove(d, s, n); }
void bzero(void *d, size_t n) { memset(d, 0, n); }
int  ffs(int v)  { return v ? __builtin_ctz((unsigned)v) + 1 : 0; }
int  ffsl(long v) { return v ? __builtin_ctzl((unsigned long)v) + 1 : 0; }
int  ffsll(long long v) { return v ? __builtin_ctzll((unsigned long long)v) + 1 : 0; }
