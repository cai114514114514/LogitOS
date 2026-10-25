/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PARSER_UTF8_H
#define LOGIT_PARSER_UTF8_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif
parserutils_error parserutils_charset_utf8_to_ucs4(const uint8_t *bytes, size_t length,
                                                uint32_t *scalar, size_t *consumed);
parserutils_error parserutils_charset_utf8_from_ucs4(uint32_t scalar, uint8_t **dest, size_t *available);
parserutils_error parserutils_charset_utf8_length(const uint8_t *bytes, size_t limit, size_t *length);
parserutils_error parserutils_charset_utf8_char_byte_length(const uint8_t *bytes, size_t *length);
parserutils_error parserutils_charset_utf8_prev(const uint8_t *bytes, uint32_t offset, uint32_t *previous);
parserutils_error parserutils_charset_utf8_next(const uint8_t *bytes, uint32_t length,
                                             uint32_t offset, uint32_t *next);
parserutils_error parserutils_charset_utf8_next_paranoid(const uint8_t *bytes, uint32_t length,
                                                      uint32_t offset, uint32_t *next);
#ifdef __cplusplus
}
#endif
#endif
