/* SPDX-License-Identifier: GPL-3.0-or-later
 * These utilities use host-order UTF-16 code units, including unaligned input.
 * Lengths and offsets are in bytes. from_ucs4 replaces capacity with bytes used;
 * unlike the UTF-8 writer, it does not advance a pointer supplied by the caller.
 */
#ifndef LOGIT_PARSER_UTF16_H
#define LOGIT_PARSER_UTF16_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif
parserutils_error parserutils_charset_utf16_to_ucs4(const uint8_t *bytes, size_t length,
                                                 uint32_t *scalar, size_t *consumed);
parserutils_error parserutils_charset_utf16_from_ucs4(uint32_t scalar, uint8_t *bytes, size_t *length);
parserutils_error parserutils_charset_utf16_length(const uint8_t *bytes, size_t limit, size_t *length);
parserutils_error parserutils_charset_utf16_char_byte_length(const uint8_t *bytes, size_t *length);
parserutils_error parserutils_charset_utf16_prev(const uint8_t *bytes, uint32_t offset, uint32_t *previous);
parserutils_error parserutils_charset_utf16_next(const uint8_t *bytes, uint32_t length,
                                              uint32_t offset, uint32_t *next);
parserutils_error parserutils_charset_utf16_next_paranoid(const uint8_t *bytes, uint32_t length,
                                                       uint32_t offset, uint32_t *next);
#ifdef __cplusplus
}
#endif
#endif
