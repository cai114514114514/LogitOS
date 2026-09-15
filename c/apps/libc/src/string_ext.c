/* Compatibility string primitives that do not belong on string.c's parser-hot
 * path.  The timing-safe functions always inspect all n bytes; explicit_bzero
 * writes through volatile storage so dead-store elimination cannot erase a
 * secret wipe immediately before free(). */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

void explicit_bzero(void *buf, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (n--) *p++ = 0;
}

void *memset_explicit(void *buf, int c, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)buf;
    while (n--) *p++ = (unsigned char)c;
    return buf;
}

int timingsafe_bcmp(const void *va, const void *vb, size_t n)
{
    const unsigned char *a = va, *b = vb;
    unsigned int diff = 0;
    while (n--) diff |= (unsigned int)(*a++ ^ *b++);
    return diff != 0;
}

int timingsafe_memcmp(const void *va, const void *vb, size_t n)
{
    const unsigned char *a = va, *b = vb;
    unsigned int lt = 0, gt = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned int ai = a[i], bi = b[i];
        unsigned int undecided = 1u ^ (lt | gt);
        lt |= (((ai - bi) >> (sizeof(unsigned int) * CHAR_BIT - 1)) & 1u) & undecided;
        gt |= (((bi - ai) >> (sizeof(unsigned int) * CHAR_BIT - 1)) & 1u) & undecided;
    }
    return (int)gt - (int)lt;
}

char *strnstr(const char *haystack, const char *needle, size_t len)
{
    size_t nl = strlen(needle);
    if (!nl) return (char *)haystack;
    size_t hl = strnlen(haystack, len);
    if (nl > hl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++)
        if (haystack[i] == needle[0] && memcmp(haystack + i, needle, nl) == 0)
            return (char *)haystack + i;
    return NULL;
}

long long strtonum(const char *s, long long minval, long long maxval,
                   const char **errstrp)
{
    const char *err = NULL;
    long long value = 0;
    if (minval > maxval) err = "invalid";
    else if (!s || !*s) err = "invalid";
    else {
        char *end;
        int saved_errno = errno;
        errno = 0;
        value = strtoll(s, &end, 10);
        int parse_errno = errno;
        if (end == s || *end) err = "invalid";
        else if ((value == LLONG_MIN && parse_errno == ERANGE) || value < minval)
            err = "too small";
        else if ((value == LLONG_MAX && parse_errno == ERANGE) || value > maxval)
            err = "too large";
        if (!err) errno = saved_errno;
    }
    if (err) {
        errno = !strcmp(err, "invalid") ? EINVAL : ERANGE;
        if (errstrp) *errstrp = err;
        return 0;
    }
    if (errstrp) *errstrp = NULL;
    return value;
}
