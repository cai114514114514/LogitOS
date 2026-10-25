/* SPDX-License-Identifier: GPL-3.0-or-later
 * Encoding identifiers are IANA MIBenum values, not private array positions.
 * The existing browser's WHATWG mapping data is shared verbatim as DATA; no
 * alias generator or codec implementation from libparserutils is retained.
 * The byte maps retain their existing provenance in encoding_tables.inc.
 */
#include <parserutils/charset/mibenum.h>
#include "text.h"
#include <string.h>
#include "../../apps/gui/browser/js/text/encoding_tables.inc"

static bool label_equal(const char *a, size_t length, const char *b)
{
    if (strlen(b) != length) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

static const unsigned short *byte_map(uint16_t mib)
{
    if (mib == 12) return ENC_SB_WINDOWS_1254;
    /* IANA registers ISO-8859-11 as a TIS-620 alias (2259), not the old
     * library's private number 4014. Its graphic bytes share this table;
     * iso_single_byte excludes Windows extensions in the control range. */
    if (mib == 2259) return ENC_SB_WINDOWS_874;
    const char *name = parserutils_charset_mibenum_to_name(mib);
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof(ENC_SINGLE_BYTE) / sizeof(*ENC_SINGLE_BYTE); i++)
        if (label_equal(name, strlen(name), ENC_SINGLE_BYTE[i].name)) return ENC_SINGLE_BYTE[i].idx;
    return NULL;
}

bool text_supported(uint16_t mib)
{
    return mib == TEXT_ASCII || mib == TEXT_LATIN1 || mib == TEXT_UTF8 ||
           (mib >= TEXT_UTF16BE && mib <= TEXT_UTF16) ||
           (mib >= TEXT_UTF32 && mib <= TEXT_UTF32LE) || byte_map(mib);
}

static bool iso_single_byte(uint16_t mib)
{
    return (mib >= 4 && mib <= 13) || (mib >= 109 && mib <= 112) || mib == 2259;
}

static bool mapped_scalar(uint16_t mib, unsigned byte,
                          const unsigned short *map, uint32_t *scalar)
{
    *scalar = map[byte - 128];
    /* An earlier comment claimed the missing-entry sentinel was 0xffff.
     * gen_encoding_tables.py actually writes zero for missing entries. Neither
     * value can represent a valid mapping for these non-ASCII source bytes. */
    if (!*scalar || *scalar == 0xffff || *scalar == 0xfffd) return false;
    if (iso_single_byte(mib) && byte < 160) return false;
    /* Web decoding accepts some Windows control bytes that the standalone
     * codec rejects. CP1255 also leaves CA undefined; WHATWG maps it to U+05BA.
     * Keep the browser data unchanged and apply the codec repertoire here.
     * Source: unicode.org/Public/MAPPINGS/VENDORS/MICSFT/WINDOWS/CP1255.TXT */
    if (mib >= 2250 && mib <= 2258 && *scalar >= 0x80 && *scalar <= 0x9f)
        return false;
    return !(mib == 2255 && byte == 0xca);
}

static uint16_t unit16(const uint8_t *bytes, bool little)
{
    return little ? (uint16_t)(bytes[0] | (bytes[1] << 8)) : (uint16_t)((bytes[0] << 8) | bytes[1]);
}

parserutils_error text_read(uint16_t mib, const uint8_t *bytes, size_t length,
                           uint32_t *scalar, size_t *consumed)
{
    *consumed = 0;
    if (!length) return PARSERUTILS_NEEDDATA;
    if (mib == TEXT_UTF8) return text_utf8_read(bytes, length, scalar, consumed);
    if (mib >= TEXT_UTF16BE && mib <= TEXT_UTF16) {
        if (length < 2) return PARSERUTILS_NEEDDATA;
        bool little = mib == TEXT_UTF16LE;
        uint32_t first = unit16(bytes, little);
        *consumed = 2;
        if (first >= 0xdc00 && first <= 0xdfff) return PARSERUTILS_INVALID;
        if (first >= 0xd800 && first <= 0xdbff) {
            if (length < 4) return PARSERUTILS_NEEDDATA;
            uint32_t second = unit16(bytes + 2, little);
            if (second < 0xdc00 || second > 0xdfff) return PARSERUTILS_INVALID;
            first = 0x10000 + ((first - 0xd800) << 10) + second - 0xdc00;
            *consumed = 4;
        }
        *scalar = first;
        return PARSERUTILS_OK;
    }
    if (mib >= TEXT_UTF32 && mib <= TEXT_UTF32LE) {
        if (length < 4) return PARSERUTILS_NEEDDATA;
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; i++)
            value = (value << 8) | bytes[mib == TEXT_UTF32LE ? 3 - i : i];
        *consumed = 4;
        if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return PARSERUTILS_INVALID;
        *scalar = value;
        return PARSERUTILS_OK;
    }
    *consumed = 1;
    unsigned byte = *bytes;
    if (byte < 128) {
        *scalar = byte;
        return PARSERUTILS_OK;
    }
    if (mib == TEXT_ASCII) return PARSERUTILS_INVALID;
    /* Preserve the parser codec's repertoire: ISO graphic sets do not accept
     * C1 bytes. The first browser-only implementation passed those through.
     * Inspection of all 256 bytes exposed that difference in every ISO codec. */
    if (iso_single_byte(mib) && byte < 160) return PARSERUTILS_INVALID;
    if (mib == TEXT_LATIN1) {
        *scalar = byte;
        return PARSERUTILS_OK;
    }
    const unsigned short *map = byte_map(mib);
    if (!map) return PARSERUTILS_BADENCODING;
    return mapped_scalar(mib, byte, map, scalar) ? PARSERUTILS_OK : PARSERUTILS_INVALID;
}

parserutils_error text_write(uint16_t mib, uint32_t scalar, uint8_t output[4], size_t *length)
{
    *length = 0;
    if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
        return PARSERUTILS_INVALID;
    if (mib == TEXT_UTF8) {
        *length = text_utf8_write(scalar, output);
        return PARSERUTILS_OK;
    }
    if (mib >= TEXT_UTF16BE && mib <= TEXT_UTF16) {
        uint16_t units[2];
        size_t count = 1;
        if (scalar < 0x10000) units[0] = (uint16_t)scalar;
        else {
            uint32_t pair = scalar - 0x10000;
            units[0] = (uint16_t)(0xd800 + (pair >> 10));
            units[1] = (uint16_t)(0xdc00 + (pair & 0x3ff));
            count = 2;
        }
        bool little = mib == TEXT_UTF16LE;
        for (size_t i = 0; i < count; i++) {
            output[2 * i] = (uint8_t)(little ? units[i] : units[i] >> 8);
            output[2 * i + 1] = (uint8_t)(little ? units[i] >> 8 : units[i]);
        }
        *length = 2 * count;
        return PARSERUTILS_OK;
    }
    if (mib >= TEXT_UTF32 && mib <= TEXT_UTF32LE) {
        for (unsigned i = 0; i < 4; i++)
            output[i] = (uint8_t)(scalar >> (8 * (mib == TEXT_UTF32LE ? i : 3 - i)));
        *length = 4;
        return PARSERUTILS_OK;
    }
    if (scalar < 128 || (mib == TEXT_LATIN1 && scalar >= 160 && scalar <= 255)) {
        output[0] = (uint8_t)scalar;
        *length = 1;
        return PARSERUTILS_OK;
    }
    if (mib == TEXT_ASCII || mib == TEXT_LATIN1) return PARSERUTILS_INVALID;
    const unsigned short *map = byte_map(mib);
    if (!map) return PARSERUTILS_BADENCODING;
    for (unsigned byte = 128; byte < 256; byte++) {
        uint32_t mapped;
        if (!mapped_scalar(mib, byte, map, &mapped) || mapped != scalar) continue;
        output[0] = (uint8_t)byte;
        *length = 1;
        return PARSERUTILS_OK;
    }
    return PARSERUTILS_INVALID;
}
