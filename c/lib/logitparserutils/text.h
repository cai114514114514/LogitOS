/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_TEXT_H
#define LOGIT_PARSER_TEXT_H
#include <parserutils/errors.h>

enum {
    TEXT_ASCII = 3, TEXT_LATIN1 = 4, TEXT_UTF8 = 106,
    TEXT_UTF16BE = 1013, TEXT_UTF16LE = 1014, TEXT_UTF16 = 1015,
    TEXT_UTF32 = 1017, TEXT_UTF32BE = 1018, TEXT_UTF32LE = 1019
};

bool text_supported(uint16_t);
parserutils_error text_read(uint16_t, const uint8_t *, size_t, uint32_t *, size_t *);
parserutils_error text_write(uint16_t, uint32_t, uint8_t[4], size_t *);
parserutils_error text_utf8_read(const uint8_t *, size_t, uint32_t *, size_t *);
size_t text_utf8_write(uint32_t, uint8_t[4]);

static inline uint16_t text_native_utf16(void)
{
    const uint16_t one = 1;
    return *(const uint8_t *)&one ? TEXT_UTF16LE : TEXT_UTF16BE;
}
#endif
