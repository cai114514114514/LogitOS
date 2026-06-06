#include <stddef.h>
#include <stdint.h>

/* Freestanding clang may emit calls to these for struct/array codegen, so the
 * kernel must provide its own. */

void *memset(void *dst, int value, size_t n)
{
    unsigned char *p = dst;
    unsigned char v = (unsigned char)value;

    /* Word-at-a-time fill: byte preamble until 8-byte aligned, then bulk
     * uint64_t stores, then byte tail. Same bytes written, fewer stores. */
    if (n >= sizeof(uint64_t)) {
        while ((uintptr_t)p & (sizeof(uint64_t) - 1)) {
            *p++ = v;
            n--;
        }
        uint64_t word = 0x0101010101010101ULL * v;
        uint64_t *pw = (uint64_t *)p;
        size_t words = n / sizeof(uint64_t);
        for (size_t i = 0; i < words; i++)
            pw[i] = word;
        p = (unsigned char *)(pw + words);
        n &= sizeof(uint64_t) - 1;
    }
    while (n--)
        *p++ = v;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    /* Word-at-a-time copy: byte preamble until the destination is 8-byte
     * aligned, then bulk uint64_t loads/stores, then byte tail. */
    if (n >= sizeof(uint64_t)) {
        while ((uintptr_t)d & (sizeof(uint64_t) - 1)) {
            *d++ = *s++;
            n--;
        }
        uint64_t *dw = (uint64_t *)d;
        const uint64_t *sw = (const uint64_t *)s;
        size_t words = n / sizeof(uint64_t);
        for (size_t i = 0; i < words; i++)
            dw[i] = sw[i];
        d = (unsigned char *)(dw + words);
        s = (const unsigned char *)(sw + words);
        n &= sizeof(uint64_t) - 1;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}
