/* SPDX-License-Identifier: GPL-3.0-or-later
 * Raw network input and decoded token bytes have different lifetimes. Keep a
 * pending source queue and a UTF-8 consumption window, and commit a source
 * character only after its complete output has been stored successfully.
 */
#include <parserutils/input/inputstream.h>
#include <parserutils/charset/mibenum.h>
#include <parserutils/utils/buffer.h>
#include "text.h"
#include "window.h"
#include <stdlib.h>
#include <string.h>

struct parserutils_inputstream {
    parserutils_buffer *pending;
    parser_window decoded;
    size_t source_offset;
    uint16_t encoding;
    uint32_t origin;
    parserutils_charset_detect_func detect;
    bool eof, selected;
};

parserutils_error parserutils_inputstream_create(const char *label, uint32_t origin,
        parserutils_charset_detect_func detect, parserutils_inputstream **result)
{
    if (!result) return PARSERUTILS_BADPARM;
    *result = NULL;
    uint16_t encoding = label ? parserutils_charset_mibenum_from_name(label, strlen(label)) : TEXT_UTF8;
    if (!text_supported(encoding)) return PARSERUTILS_BADENCODING;
    parserutils_inputstream *s = calloc(1, sizeof(*s));
    if (!s) return PARSERUTILS_NOMEM;
    parserutils_error error = parserutils_buffer_create(&s->pending);
    if (error) {
        if (s->pending) parserutils_buffer_destroy(s->pending);
        free(s);
        return error;
    }
    s->encoding = encoding;
    s->origin = origin;
    s->detect = detect;
    *result = s;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_inputstream_destroy(parserutils_inputstream *s)
{
    if (!s) return PARSERUTILS_BADPARM;
    parserutils_buffer_destroy(s->pending);
    window_free(&s->decoded);
    free(s);
    return PARSERUTILS_OK;
}

parserutils_error parserutils_inputstream_append(parserutils_inputstream *s,
                                               const uint8_t *bytes, size_t length)
{
    if (!s || (!bytes && length)) return PARSERUTILS_BADPARM;
    if (!bytes) { s->eof = true; return PARSERUTILS_OK; }
    if (s->eof) return PARSERUTILS_INVALID;
    if (s->source_offset) {
        parserutils_buffer_discard(s->pending, 0, s->source_offset);
        s->source_offset = 0;
    }
    return parserutils_buffer_append(s->pending, bytes, length);
}

static parserutils_error select_encoding(parserutils_inputstream *s)
{
    if (s->selected) return PARSERUTILS_OK;
    const uint8_t *bytes = s->pending->data;
    size_t length = s->pending->length;
    if (!length) {
        if (!s->eof) return PARSERUTILS_NEEDDATA;
        s->selected = true;
        return PARSERUTILS_OK;
    }
    /* Four bytes distinguish UTF-16LE's BOM from UTF-32LE's. At EOF a short
     * stylesheet is still valid input; a detector asking for more cannot keep
     * an already-closed stream in NEEDDATA forever. */
    if (length < 4 && !s->eof) return PARSERUTILS_NEEDDATA;
    uint16_t encoding = s->encoding;
    uint32_t origin = s->origin;
    if (s->detect) {
        parserutils_error error = s->detect(bytes, length, &encoding, &origin);
        if (error == PARSERUTILS_NEEDDATA && !s->eof) return error;
        if (error && error != PARSERUTILS_NEEDDATA) return error;
    }
    /* A name the registry knows but no decoder implements (Shift_JIS, GB2312)
     * must not discard the whole stylesheet: ignore the in-band declaration
     * and keep the encoding the caller chose, as for an unknown name. */
    if (!text_supported(encoding)) {
        encoding = s->encoding;
        origin = s->origin;
    }
    size_t skip = 0;
    if ((encoding == TEXT_UTF32 || encoding == TEXT_UTF32LE) && length >= 4 &&
        bytes[0] == 0xff && bytes[1] == 0xfe && bytes[2] == 0 && bytes[3] == 0) {
        encoding = TEXT_UTF32LE; skip = 4;
    } else if ((encoding == TEXT_UTF32 || encoding == TEXT_UTF32BE) && length >= 4 &&
               bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0xfe && bytes[3] == 0xff) {
        encoding = TEXT_UTF32BE; skip = 4;
    } else if ((encoding == TEXT_UTF16 || encoding == TEXT_UTF16LE) && length >= 2 &&
               bytes[0] == 0xff && bytes[1] == 0xfe) {
        encoding = TEXT_UTF16LE; skip = 2;
    } else if ((encoding == TEXT_UTF16 || encoding == TEXT_UTF16BE) && length >= 2 &&
               bytes[0] == 0xfe && bytes[1] == 0xff) {
        encoding = TEXT_UTF16BE; skip = 2;
    } else if (encoding == TEXT_UTF8 && length >= 3 &&
               bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) skip = 3;
    s->encoding = encoding;
    s->origin = origin;
    s->source_offset = skip;
    s->selected = true;
    return PARSERUTILS_OK;
}

static parserutils_error decode_pending(parserutils_inputstream *s)
{
    parserutils_error error = select_encoding(s);
    if (error) return error;
    while (s->source_offset < s->pending->length) {
        const uint8_t *bytes = s->pending->data + s->source_offset;
        size_t remaining = s->pending->length - s->source_offset;
        if (s->encoding == TEXT_UTF8) {
            size_t ascii = 0;
            while (ascii < remaining && bytes[ascii] < 128) ascii++;
            if (ascii) {
                error = window_append(&s->decoded, bytes, ascii);
                if (error) return error;
                s->source_offset += ascii;
                continue;
            }
        }
        uint32_t scalar = 0;
        size_t consumed = 0;
        error = text_read(s->encoding, bytes, remaining, &scalar, &consumed);
        if (error == PARSERUTILS_NEEDDATA) {
            if (!s->eof) return PARSERUTILS_NEEDDATA;
            consumed = remaining;
            scalar = 0xfffd;
        } else if (error == PARSERUTILS_INVALID) {
            if (!consumed) consumed = 1;
            scalar = 0xfffd;
        } else if (error) return error;
        /* The first browser-only version replaced U+0000 here. It is a valid
         * Unicode scalar: general streams and inserted UTF-8 must both preserve
         * it. Language-specific preprocessing belongs to the consumer. */
        uint8_t output[4];
        size_t count = text_utf8_write(scalar, output);
        if (!count) return PARSERUTILS_INVALID;
        error = window_append(&s->decoded, output, count);
        if (error) return error;
        s->source_offset += consumed;
    }
    return PARSERUTILS_OK;
}

parserutils_error parserutils_inputstream_peek(parserutils_inputstream *s, size_t offset,
                                             const uint8_t **bytes, size_t *length)
{
    if (!s || !bytes || !length) return PARSERUTILS_BADPARM;
    *bytes = NULL;
    *length = 0;
    if (offset > SIZE_MAX - s->decoded.cursor) return PARSERUTILS_BADPARM;
    size_t position = s->decoded.cursor + offset;
    if (position >= s->decoded.length) {
        /* Compact only before extending the decoded window. A token is an
         * offset from cursor while it is being scanned; dropping the consumed
         * prefix preserves that offset. No compaction occurs on a fast peek. */
        parserutils_error error = window_compact(&s->decoded);
        if (error) return error;
        position = s->decoded.cursor + offset;
        error = decode_pending(s);
        if (error && error != PARSERUTILS_NEEDDATA) return error;
    }
    if (position >= s->decoded.length)
        return s->eof && s->source_offset == s->pending->length ? PARSERUTILS_EOF : PARSERUTILS_NEEDDATA;
    uint32_t scalar;
    parserutils_error error = text_utf8_read(s->decoded.data + position,
            s->decoded.length - position, &scalar, length);
    if (error) return error;
    *bytes = s->decoded.data + position;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_inputstream_peek_slow(parserutils_inputstream *s, size_t offset,
                                                  const uint8_t **bytes, size_t *length)
{
    return parserutils_inputstream_peek(s, offset, bytes, length);
}

void parserutils_inputstream_advance(parserutils_inputstream *s, size_t count)
{
    if (!s) return;
    window_advance(&s->decoded, count);
}

parserutils_error parserutils_inputstream_insert(parserutils_inputstream *s,
                                               const uint8_t *bytes, size_t length)
{
    if (!s || (!bytes && length)) return PARSERUTILS_BADPARM;
    /* Insertions are already UTF-8, irrespective of the source encoding. */
    size_t at = 0;
    while (at < length) {
        uint32_t scalar;
        size_t n;
        parserutils_error error = text_utf8_read(bytes + at, length - at, &scalar, &n);
        if (error) return error;
        at += n;
    }
    return window_insert(&s->decoded, bytes, length);
}

const char *parserutils_inputstream_read_charset(parserutils_inputstream *s, uint32_t *origin)
{
    if (!s || !origin) return NULL;
    *origin = s->origin;
    return parserutils_charset_mibenum_to_name(s->encoding);
}

parserutils_error parserutils_inputstream_change_charset(parserutils_inputstream *s,
                                                       const char *label, uint32_t origin)
{
    if (!s || !label) return PARSERUTILS_BADPARM;
    if (s->selected) return PARSERUTILS_INVALID;
    uint16_t encoding = parserutils_charset_mibenum_from_name(label, strlen(label));
    if (!text_supported(encoding)) return PARSERUTILS_BADENCODING;
    s->encoding = encoding;
    s->origin = origin;
    return PARSERUTILS_OK;
}
