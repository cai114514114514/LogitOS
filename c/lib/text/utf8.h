#ifndef LOGIT_UTF8_H
#define LOGIT_UTF8_H

#include <stdint.h>

/* Decode one UTF-8 sequence at `s`, write the code point to *cp, and return a
 * pointer just past the consumed bytes. On a malformed lead/continuation byte,
 * *cp = 0xFFFD (replacement) and exactly one byte is consumed. A NUL byte
 * decodes to cp=0 and returns s+1 (callers test cp==0 to stop). */
const char *utf8_next(const char *s, uint32_t *cp);

#endif /* LOGIT_UTF8_H */
