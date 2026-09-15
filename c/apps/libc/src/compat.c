/* Small BSD/GNU/XSI compatibility functions whose complete implementation is
 * already expressible in terms of the core libc.  Keeping them here avoids
 * turning stdlib.c into an unrelated-API catch-all. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

void *reallocf(void *ptr, size_t size)
{
    void *next = realloc(ptr, size);
    if (!next && size) free(ptr);
    return next;
}

void freezero(void *ptr, size_t size)
{
    if (ptr) { explicit_bzero(ptr, size); free(ptr); }
}

char *canonicalize_file_name(const char *path) { return realpath(path, NULL); }

char *secure_getenv(const char *name)
{
    /* LogitOS has no saved-set IDs: effective and real IDs are equal by
     * construction today.  Keep the comparison anyway so this remains safe if
     * that kernel invariant changes later. */
    if (getuid() != geteuid() || getgid() != getegid()) return NULL;
    return getenv(name);
}

long a64l(const char *s)
{
    unsigned long value = 0;
    for (unsigned int shift = 0; s && *s && shift < 36; shift += 6, s++) {
        unsigned int d;
        if (*s == '.') d = 0;
        else if (*s == '/') d = 1;
        else if (*s >= '0' && *s <= '9') d = (unsigned int)(*s - '0') + 2;
        else if (*s >= 'A' && *s <= 'Z') d = (unsigned int)(*s - 'A') + 12;
        else if (*s >= 'a' && *s <= 'z') d = (unsigned int)(*s - 'a') + 38;
        else break;
        value |= (unsigned long)d << shift;
    }
    return (long)value;
}

char *l64a(long value)
{
    static char buf[7];
    static const char digits[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned long v = (unsigned long)value & 0xfffffffful;
    int n = 0;
    while (v && n < 6) { buf[n++] = digits[v & 63u]; v >>= 6; }
    buf[n] = 0;
    return buf;
}
