/* GNU argz vectors.  Mutating operations allocate a complete replacement
 * before freeing the old vector instead of growing it in place.  That costs a
 * copy, but it makes two otherwise silent traps impossible: a source string
 * may point into the old vector, and ENOMEM must leave both pointer and length
 * unchanged. */
#include <argz.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static error_t replace_bytes(char **argz, size_t *argz_len,
                             size_t off, size_t old_len,
                             const char *insert, size_t insert_len)
{
    if (!argz || !argz_len || off > *argz_len || old_len > *argz_len - off)
        return EINVAL;
    if (insert_len > (size_t)-1 - (*argz_len - old_len)) return ENOMEM;
    size_t new_len = *argz_len - old_len + insert_len;
    if (new_len == 0) {
        free(*argz);
        *argz = NULL;
        *argz_len = 0;
        return 0;
    }
    char *next = malloc(new_len);
    if (!next) return ENOMEM;
    if (off) memcpy(next, *argz, off);
    if (insert_len) memcpy(next + off, insert, insert_len);
    if (*argz_len > off + old_len)
        memcpy(next + off + insert_len, *argz + off + old_len,
               *argz_len - off - old_len);
    free(*argz);
    *argz = next;
    *argz_len = new_len;
    return 0;
}

error_t argz_create(char *const argv[], char **argz, size_t *argz_len)
{
    if (!argz || !argz_len) return EINVAL;
    *argz = NULL;
    *argz_len = 0;
    if (!argv) return 0;
    size_t len = 0;
    for (size_t i = 0; argv[i]; i++) {
        size_t n = strlen(argv[i]) + 1;
        if (n > (size_t)-1 - len) return ENOMEM;
        len += n;
    }
    if (!len) return 0;
    char *out = malloc(len);
    if (!out) return ENOMEM;
    size_t at = 0;
    for (size_t i = 0; argv[i]; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(out + at, argv[i], n);
        at += n;
    }
    *argz = out;
    *argz_len = len;
    return 0;
}

error_t argz_create_sep(const char *string, int sep, char **argz, size_t *argz_len)
{
    if (!string || !argz || !argz_len || sep == 0) return EINVAL;
    *argz = NULL;
    *argz_len = 0;
    size_t input = strlen(string);
    if (!input) return 0;
    char *out = malloc(input + 1);
    if (!out) return ENOMEM;

    /* glibc collapses delimiter runs and discards leading/trailing empty
     * pieces.  Keeping that detail is what makes PATH-like "::" inputs agree
     * with the GNU interface rather than with a generic split routine. */
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)string;
    while (*p) {
        while (*p == (unsigned char)sep) p++;
        if (!*p) break;
        while (*p && *p != (unsigned char)sep) out[used++] = (char)*p++;
        out[used++] = 0;
    }
    if (!used) { free(out); return 0; }
    *argz = out;
    *argz_len = used;
    return 0;
}

size_t argz_count(const char *argz, size_t argz_len)
{
    size_t count = 0, at = 0;
    while (at < argz_len) {
        size_t n = strnlen(argz + at, argz_len - at);
        count++;
        if (n == argz_len - at) break; /* tolerate a malformed final element */
        at += n + 1;
    }
    return count;
}

void argz_extract(const char *argz, size_t argz_len, char **argv)
{
    size_t at = 0, n = 0;
    while (at < argz_len) {
        argv[n++] = (char *)argz + at;
        size_t len = strnlen(argz + at, argz_len - at);
        if (len == argz_len - at) break;
        at += len + 1;
    }
    argv[n] = NULL;
}

void argz_stringify(char *argz, size_t argz_len, int sep)
{
    if (!argz || argz_len < 2) return;
    for (size_t i = 0; i + 1 < argz_len; i++)
        if (argz[i] == 0) argz[i] = (char)sep;
}

error_t argz_append(char **argz, size_t *argz_len, const char *buf, size_t buf_len)
{
    if (!argz || !argz_len) return EINVAL;
    if (!buf_len) return 0;
    if (!buf) return EINVAL;
    return replace_bytes(argz, argz_len, *argz_len, 0, buf, buf_len);
}

error_t argz_add(char **argz, size_t *argz_len, const char *str)
{
    if (!str) return EINVAL;
    return argz_append(argz, argz_len, str, strlen(str) + 1);
}

error_t argz_add_sep(char **argz, size_t *argz_len, const char *str, int sep)
{
    char *tail = NULL;
    size_t tail_len = 0;
    error_t e = argz_create_sep(str, sep, &tail, &tail_len);
    if (!e) e = argz_append(argz, argz_len, tail, tail_len);
    free(tail);
    return e;
}

static int entry_offset(const char *argz, size_t argz_len,
                        const char *entry, size_t *off)
{
    size_t at = 0;
    while (at < argz_len) {
        if (argz + at == entry) { *off = at; return 1; }
        size_t n = strnlen(argz + at, argz_len - at);
        if (n == argz_len - at) break;
        at += n + 1;
    }
    return 0;
}

void argz_delete(char **argz, size_t *argz_len, char *entry)
{
    if (!argz || !argz_len || !*argz || !entry) return;
    size_t off;
    if (!entry_offset(*argz, *argz_len, entry, &off)) return;
    size_t n = strnlen(*argz + off, *argz_len - off);
    if (n < *argz_len - off) n++;
    memmove(*argz + off, *argz + off + n, *argz_len - off - n);
    *argz_len -= n;
    if (!*argz_len) { free(*argz); *argz = NULL; }
}

error_t argz_insert(char **argz, size_t *argz_len, char *before, const char *entry)
{
    if (!argz || !argz_len || !entry) return EINVAL;
    if (!before) return argz_add(argz, argz_len, entry);
    size_t off;
    if (!*argz || !entry_offset(*argz, *argz_len, before, &off)) return EINVAL;
    return replace_bytes(argz, argz_len, off, 0, entry, strlen(entry) + 1);
}

char *argz_next(const char *argz, size_t argz_len, const char *entry)
{
    if (!argz || !argz_len) return NULL;
    if (!entry) return (char *)argz;
    size_t off;
    if (!entry_offset(argz, argz_len, entry, &off)) return NULL;
    size_t n = strnlen(entry, argz_len - off);
    if (n >= argz_len - off || off + n + 1 >= argz_len) return NULL;
    return (char *)entry + n + 1;
}

error_t argz_replace(char **argz, size_t *argz_len, const char *str,
                     const char *with, unsigned int *replace_count)
{
    if (!argz || !argz_len || !str || !with) return EINVAL;
    size_t sl = strlen(str), wl = strlen(with);
    if (!sl || !*argz || !*argz_len) return 0;

    size_t hits = 0, out_len = *argz_len, at = 0;
    while (at < *argz_len) {
        size_t el = strnlen(*argz + at, *argz_len - at);
        const char *p = *argz + at, *end = p + el;
        while (p + sl <= end) {
            const char *hit = strstr(p, str);
            if (!hit || hit + sl > end) break;
            hits++;
            if (wl >= sl) {
                if (wl - sl > (size_t)-1 - out_len) return ENOMEM;
                out_len += wl - sl;
            } else out_len -= sl - wl;
            p = hit + sl;
        }
        if (el == *argz_len - at) break;
        at += el + 1;
    }
    if (!hits) return 0;

    char *out = malloc(out_len);
    if (!out) return ENOMEM;
    size_t src = 0, dst = 0;
    while (src < *argz_len) {
        size_t el = strnlen(*argz + src, *argz_len - src);
        const char *p = *argz + src, *end = p + el;
        while (p < end) {
            const char *hit = strstr(p, str);
            if (!hit || hit + sl > end) {
                size_t n = (size_t)(end - p);
                memcpy(out + dst, p, n); dst += n; p = end;
            } else {
                size_t n = (size_t)(hit - p);
                memcpy(out + dst, p, n); dst += n;
                memcpy(out + dst, with, wl); dst += wl;
                p = hit + sl;
            }
        }
        if (src + el < *argz_len) out[dst++] = 0;
        src += el + (src + el < *argz_len);
    }

#ifdef LIBC_EXPAND_NEGATIVE_CONTROL
    /* The gate builds this exact TU with the second replacement deliberately
     * hidden.  Its multi-hit case must fail, proving the positive check is
     * observing replacement behaviour rather than mere symbol presence. */
    if (hits > 1) out[0] ^= 1;
#endif
    free(*argz);
    *argz = out;
    *argz_len = out_len;
    if (replace_count) *replace_count += (unsigned int)hits;
    return 0;
}
