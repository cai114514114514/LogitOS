#include "utf8.h"

/* RFC 3629 UTF-8 decode. Rejects overlong-friendly here only loosely: we accept
 * the well-formed 1..4 byte forms and replace anything malformed with U+FFFD,
 * consuming a single byte so callers always make progress. */
const char *utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char c = p[0];

    if (c < 0x80) { *cp = c; return s + 1; }            /* ASCII / NUL */

    int n;                                              /* continuation bytes */
    uint32_t v;
    if      ((c & 0xE0) == 0xC0) { n = 1; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; v = c & 0x07; }
    else { *cp = 0xFFFD; return s + 1; }                /* bad lead byte */

    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return s + 1; }  /* bad cont. */
        v = (v << 6) | (p[i] & 0x3F);
    }
    *cp = v;
    return s + 1 + n;
}
