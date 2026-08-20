#include <stddef.h>
#include <stdint.h>

/* Freestanding clang may emit calls to these for struct/array codegen, so the
 * kernel must provide its own. That is why this file exists, and until
 * 2026-08-20 it was the whole of it: memset/memcpy/memmove/memcmp and
 * nothing else. The kernel had NO str* function at all.
 *
 * strcmp IS THE SEVENTH ANSWER TO A QUESTION ALREADY ANSWERED SIX TIMES.
 * Comparing two C strings is something this kernel needs, and every site
 * that needed it wrote its own private copy:
 *
 *     c/kernel/core/kdiag.c:38      static int streq
 *     c/kernel/gui/wm.c:731         static int streq
 *     c/kernel/module/ksyms.c:158   static int streq
 *     c/kernel/module/modelf.c:51   static int m_streq
 *     c/fs/logitfs.c:86             static int streq
 *     c/net/core/unix.c:164         static int pstreq
 *
 * The seventh caller (c/net/dns/dns.c:427, checking that an answer record's
 * owner name is the name we asked about) reached for strcmp instead, and
 * the kernel did not link. It linked only because a temporary fixture in
 * c/drivers/block supplied a weak strcmp -- in a directory C_SRC globs, so
 * a throwaway file was compiling into the kernel to hold the link together.
 * That file is deleted with this change.
 *
 * THE SIX ARE NOT COLLAPSED HERE, deliberately. They are static and
 * therefore harmless, they live in six files owned by six different lines
 * of work, and replacing them is a sweep rather than a fix. What is fixed
 * is that the seventh caller no longer has to be wrong to compile.
 *
 * NOT A GENERAL str* FAMILY. strlen, strcpy, strcat and the rest are still
 * absent and should stay absent until a caller in KERNEL code needs one --
 * c/net/http's fifty strlen calls are ring 3 (RING3_NET, Makefile:186) and
 * link mini-libc's. Adding a family for callers that do not exist is how
 * this file stops being about compiler-emitted calls. */

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

/* Bytewise, unsigned, and it returns the DIFFERENCE rather than -1/0/1.
 * C requires only the sign, and every caller in this tree compares against
 * zero -- but a caller that stores the result and compares two of them
 * gets what the standard library would give it, which costs nothing here
 * and removes one way for a port to behave differently on this machine.
 *
 * UNSIGNED CHAR IS NOT DECORATION. Plain char is signed on x86-64, so
 * comparing bytes above 0x7F as char makes every UTF-8 continuation byte
 * sort BEFORE ASCII -- and dns.c compares hostnames, which are exactly
 * where a non-ASCII byte arrives from the network. */
int strcmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    while (*p && *p == *q) { p++; q++; }
    return (int)*p - (int)*q;
}
