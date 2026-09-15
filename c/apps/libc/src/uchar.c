/* C11 <uchar.h>, sharing wchar.c's strict UTF-8 codec rather than growing a
 * second decoder.  mbstate_t's otherwise-unused marker values carry exactly
 * one pending UTF-16 surrogate between calls. */
#include <uchar.h>
#include <errno.h>
#include <string.h>

#define U16_DEC_PENDING 0xfeu
#define U16_ENC_PENDING 0xfdu

size_t mbrtoc32(char32_t *pc32, const char *s, size_t n, mbstate_t *ps)
{
    wchar_t wc = 0;
    size_t r = mbrtowc(&wc, s, n, ps);
    if (r != (size_t)-1 && r != (size_t)-2 && pc32) *pc32 = (char32_t)wc;
    return r;
}

size_t c32rtomb(char *s, char32_t c32, mbstate_t *ps)
{
    /* As with wcrtomb, a null destination is a state-reset request and the
     * supplied code point is ignored. Validate only real conversions. */
    if (!s) return wcrtomb(0, 0, ps);
    if ((unsigned long)c32 > 0x10fffful || (c32 >= 0xd800 && c32 <= 0xdfff)) {
        if (ps) memset(ps, 0, sizeof *ps);
        errno = EILSEQ;
        return (size_t)-1;
    }
    return wcrtomb(s, (wchar_t)c32, ps);
}

size_t mbrtoc16(char16_t *pc16, const char *s, size_t n, mbstate_t *ps)
{
    static mbstate_t internal;
    if (!ps) ps = &internal;
    if (ps->__want == U16_DEC_PENDING) {
        if (pc16) *pc16 = (char16_t)ps->__wch;
        memset(ps, 0, sizeof *ps);
        return (size_t)-3;
    }
    wchar_t wc = 0;
    size_t r = mbrtowc(&wc, s, n, ps);
    if (r == (size_t)-1 || r == (size_t)-2) return r;
    if ((unsigned long)wc <= 0xfffful) {
        if (pc16) *pc16 = (char16_t)wc;
        return r;
    }
    unsigned long v = (unsigned long)wc - 0x10000ul;
    if (pc16) *pc16 = (char16_t)(0xd800u + (v >> 10));
    ps->__wch = 0xdc00u + (unsigned int)(v & 0x3ffu);
    ps->__have = 1;
    ps->__want = U16_DEC_PENDING;
    return r;
}

size_t c16rtomb(char *s, char16_t c16, mbstate_t *ps)
{
    static mbstate_t internal;
    if (!ps) ps = &internal;
    if (!s) { memset(ps, 0, sizeof *ps); return 1; }

    unsigned int c = (unsigned int)c16;
    if (ps->__want == U16_ENC_PENDING) {
        unsigned int hi = ps->__wch;
        memset(ps, 0, sizeof *ps);
        if (c < 0xdc00u || c > 0xdfffu) { errno = EILSEQ; return (size_t)-1; }
        wchar_t wc = (wchar_t)(0x10000u + ((hi - 0xd800u) << 10) + (c - 0xdc00u));
        return wcrtomb(s, wc, ps);
    }
    if (c >= 0xd800u && c <= 0xdbffu) {
        ps->__wch = c;
        ps->__have = 1;
        ps->__want = U16_ENC_PENDING;
        return 0;
    }
    if (c >= 0xdc00u && c <= 0xdfffu) { errno = EILSEQ; return (size_t)-1; }
    return wcrtomb(s, (wchar_t)c, ps);
}
