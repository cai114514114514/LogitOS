/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unicode scalar rules are shared with the stream decoder. Byte loads avoid
 * alignment assumptions: a valid UTF-16 slice can start at an odd host address.
 */
#include <parserutils/charset/utf16.h>
#include "text.h"
#include <string.h>

parserutils_error parserutils_charset_utf16_to_ucs4(const uint8_t *bytes, size_t length,
                                                 uint32_t *scalar, size_t *consumed)
{
    if (!bytes || !scalar || !consumed) return PARSERUTILS_BADPARM;
    uint32_t value;
    size_t used;
    parserutils_error error = text_read(text_native_utf16(), bytes, length, &value, &used);
    if (error) return error;
    *scalar = value;
    *consumed = used;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf16_from_ucs4(uint32_t scalar, uint8_t *bytes, size_t *length)
{
    if (!bytes || !length) return PARSERUTILS_BADPARM;
    uint8_t encoded[4];
    size_t used;
    parserutils_error error = text_write(text_native_utf16(), scalar, encoded, &used);
    if (error) return error;
    if (*length < used) return PARSERUTILS_NOMEM;
    memcpy(bytes, encoded, used);
    *length = used;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf16_length(const uint8_t *bytes, size_t limit, size_t *length)
{
    if (!bytes || !length) return PARSERUTILS_BADPARM;
    size_t count = 0, offset = 0;
    while (offset < limit) {
        uint32_t scalar;
        size_t used;
        parserutils_error error = text_read(text_native_utf16(), bytes + offset,
                                           limit - offset, &scalar, &used);
        if (error) return error;
        offset += used;
        count++;
    }
    *length = count;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf16_char_byte_length(const uint8_t *bytes, size_t *length)
{
    if (!bytes || !length) return PARSERUTILS_BADPARM;
    uint16_t unit;
    memcpy(&unit, bytes, sizeof(unit));
    if (unit >= 0xdc00 && unit <= 0xdfff) return PARSERUTILS_INVALID;
    *length = unit >= 0xd800 && unit <= 0xdbff ? 4 : 2;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf16_prev(const uint8_t *bytes, uint32_t offset, uint32_t *previous)
{
    if (!bytes || !previous || (offset & 1)) return PARSERUTILS_BADPARM;
    if (!offset) { *previous = 0; return PARSERUTILS_OK; }
    uint32_t start = offset - 2;
    uint16_t unit;
    memcpy(&unit, bytes + start, sizeof(unit));
    if (unit >= 0xdc00 && unit <= 0xdfff) {
        if (start < 2) return PARSERUTILS_INVALID;
        start -= 2;
    }
    uint32_t scalar;
    size_t used;
    parserutils_error error = text_read(text_native_utf16(), bytes + start, offset - start, &scalar, &used);
    if (error || used != offset - start) return PARSERUTILS_INVALID;
    *previous = start;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf16_next(const uint8_t *bytes, uint32_t length,
                                              uint32_t offset, uint32_t *next)
{
    if (!bytes || !next || offset > length || (offset & 1)) return PARSERUTILS_BADPARM;
    if (offset == length) { *next = length; return PARSERUTILS_OK; }
    uint32_t scalar;
    size_t used;
    parserutils_error error = text_read(text_native_utf16(), bytes + offset, length - offset, &scalar, &used);
    if (error) return error;
    *next = offset + (uint32_t)used;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf16_next_paranoid(const uint8_t *bytes, uint32_t length,
                                                       uint32_t offset, uint32_t *next)
{
    return parserutils_charset_utf16_next(bytes, length, offset, next);
}
