/* SPDX-License-Identifier: GPL-3.0-or-later
 * Scalar UTF-8 operations. Bounds are checked before reading a continuation;
 * incomplete input is distinct from invalid input so a network chunk boundary
 * never turns a valid multi-byte character into a replacement character.
 */
#include <parserutils/charset/utf8.h>
#include "text.h"

static unsigned width(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if (lead >= 0xc2 && lead <= 0xdf) return 2;
    if (lead >= 0xe0 && lead <= 0xef) return 3;
    if (lead >= 0xf0 && lead <= 0xf4) return 4;
    return 0;
}

parserutils_error text_utf8_read(const uint8_t *bytes, size_t length,
                               uint32_t *scalar, size_t *consumed)
{
    *consumed = 0;
    if (!length) return PARSERUTILS_NEEDDATA;
    unsigned n = width(bytes[0]);
    if (!n) { *consumed = 1; return PARSERUTILS_INVALID; }
    if (n == 1) { *scalar = bytes[0]; *consumed = 1; return PARSERUTILS_OK; }
    uint32_t value = bytes[0] & ((1u << (7 - n)) - 1);
    for (unsigned i = 1; i < n; i++) {
        if (i == length) { *consumed = i; return PARSERUTILS_NEEDDATA; }
        unsigned c = bytes[i];
        bool bad = (c & 0xc0) != 0x80;
        if (i == 1) {
            bad |= bytes[0] == 0xe0 && c < 0xa0;
            bad |= bytes[0] == 0xed && c > 0x9f;
            bad |= bytes[0] == 0xf0 && c < 0x90;
            bad |= bytes[0] == 0xf4 && c > 0x8f;
        }
        if (bad) { *consumed = i; return PARSERUTILS_INVALID; }
        value = (value << 6) | (c & 0x3f);
    }
    *scalar = value;
    *consumed = n;
    return PARSERUTILS_OK;
}

size_t text_utf8_write(uint32_t scalar, uint8_t bytes[4])
{
    if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) return 0;
    unsigned n = scalar < 0x80 ? 1 : scalar < 0x800 ? 2 : scalar < 0x10000 ? 3 : 4;
    if (n == 1) bytes[0] = (uint8_t)scalar;
    else {
        for (unsigned i = n - 1; i; i--) {
            bytes[i] = 0x80 | (scalar & 0x3f);
            scalar >>= 6;
        }
        bytes[0] = (uint8_t)((0xffu << (8 - n)) | scalar);
    }
    return n;
}

parserutils_error parserutils_charset_utf8_to_ucs4(const uint8_t *bytes, size_t length,
                                                uint32_t *scalar, size_t *consumed)
{
    if (!bytes || !scalar || !consumed) return PARSERUTILS_BADPARM;
    return text_utf8_read(bytes, length, scalar, consumed);
}

parserutils_error parserutils_charset_utf8_from_ucs4(uint32_t scalar, uint8_t **dest, size_t *available)
{
    if (!dest || !*dest || !available) return PARSERUTILS_BADPARM;
    uint8_t bytes[4];
    size_t n = text_utf8_write(scalar, bytes);
    if (!n) return PARSERUTILS_INVALID;
    if (*available < n) return PARSERUTILS_NOMEM;
    for (size_t i = 0; i < n; i++) (*dest)[i] = bytes[i];
    *dest += n;
    *available -= n;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf8_char_byte_length(const uint8_t *bytes, size_t *length)
{
    if (!bytes || !length) return PARSERUTILS_BADPARM;
    *length = width(*bytes);
    return *length ? PARSERUTILS_OK : PARSERUTILS_INVALID;
}

parserutils_error parserutils_charset_utf8_length(const uint8_t *bytes, size_t limit, size_t *length)
{
    if (!bytes || !length) return PARSERUTILS_BADPARM;
    size_t count = 0;
    for (size_t offset = 0; offset < limit;) {
        uint32_t scalar;
        size_t n;
        parserutils_error error = text_utf8_read(bytes + offset, limit - offset, &scalar, &n);
        if (error) return error;
        offset += n;
        count++;
    }
    *length = count;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf8_prev(const uint8_t *bytes, uint32_t offset, uint32_t *previous)
{
    if (!bytes || !previous) return PARSERUTILS_BADPARM;
    if (!offset) { *previous = 0; return PARSERUTILS_OK; }
    uint32_t p = offset - 1;
    while (p && (bytes[p] & 0xc0) == 0x80) p--;
    uint32_t scalar;
    size_t n;
    parserutils_error error = text_utf8_read(bytes + p, offset - p, &scalar, &n);
    if (error || n != offset - p) return PARSERUTILS_INVALID;
    *previous = p;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf8_next(const uint8_t *bytes, uint32_t length,
                                             uint32_t offset, uint32_t *next)
{
    if (!bytes || !next || offset > length) return PARSERUTILS_BADPARM;
    if (offset == length) { *next = length; return PARSERUTILS_OK; }
    uint32_t scalar;
    size_t n;
    parserutils_error error = text_utf8_read(bytes + offset, length - offset, &scalar, &n);
    if (error) return error;
    *next = offset + (uint32_t)n;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_utf8_next_paranoid(const uint8_t *bytes, uint32_t length,
                                                      uint32_t offset, uint32_t *next)
{
    return parserutils_charset_utf8_next(bytes, length, offset, next);
}
