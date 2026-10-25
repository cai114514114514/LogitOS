/* SPDX-License-Identifier: GPL-3.0-or-later
 * Each UCS-4 value is network-order, independent of the host CPU. Output stays
 * pending as one complete character when the destination cannot fit it, so a
 * caller can retry with the remaining input without duplicating a character.
 */
#include "codec_state.h"
#include "text.h"
#include <parserutils/charset/mibenum.h>
#include <string.h>

parserutils_error parserutils_charset_codec_encode(parserutils_charset_codec *codec,
        const uint8_t **source, size_t *remaining, uint8_t **dest, size_t *available)
{
    if (!codec_arguments(codec, source, remaining, dest, available))
        return PARSERUTILS_BADPARM;
    codec_lane *lane = &codec->encoder;
    parserutils_error error = codec_drain(lane, dest, available);
    if (error) return error;

    while (*remaining) {
        uint8_t candidate[4];
        size_t prefix = lane->input_length;
        size_t added = *remaining < 4 - prefix ? *remaining : 4 - prefix;
        memcpy(candidate, lane->input, prefix);
        memcpy(candidate + prefix, *source, added);
        if (prefix + added < 4) {
            memcpy(lane->input, candidate, prefix + added);
            lane->input_length = prefix + added;
            *source += added;
            *remaining -= added;
            return PARSERUTILS_OK;
        }
        uint32_t scalar = 0;
        for (unsigned i = 0; i < 4; i++) scalar = (scalar << 8) | candidate[i];
        size_t count;
        error = text_write(codec->encoding, scalar, lane->output, &count);
        if (error == PARSERUTILS_INVALID) {
            if (codec->mode == PARSERUTILS_CHARSET_CODEC_ERROR_STRICT) return error;
            /* No language-specific transliteration table is installed. The
             * documented TRANSLIT fallback is the same replacement as LOOSE. */
            uint32_t replacement = parserutils_charset_mibenum_is_unicode(codec->encoding) ? 0xfffd : '?';
            error = text_write(codec->encoding, replacement, lane->output, &count);
        }
        if (error) return error;
        lane->input_length = 0;
        *source += added;
        *remaining -= added;
        lane->output_length = count;
        error = codec_drain(lane, dest, available);
        if (error) return error;
    }
    return PARSERUTILS_OK;
}
