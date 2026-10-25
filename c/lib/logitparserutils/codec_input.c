/* SPDX-License-Identifier: GPL-3.0-or-later
 * Decode with a four-byte lookahead. A strict error does not consume the bad
 * unit; loose mode consumes only its maximal invalid prefix, leaving a following
 * ASCII character to be decoded normally. Pending input belongs to this lane.
 */
#include "codec_state.h"
#include "text.h"
#include <string.h>

parserutils_error parserutils_charset_codec_decode(parserutils_charset_codec *codec,
        const uint8_t **source, size_t *remaining, uint8_t **dest, size_t *available)
{
    if (!codec_arguments(codec, source, remaining, dest, available))
        return PARSERUTILS_BADPARM;
    codec_lane *lane = &codec->decoder;
    parserutils_error error = codec_drain(lane, dest, available);
    if (error) return error;

    while (*remaining) {
        uint8_t candidate[4];
        size_t prefix = lane->input_length;
        size_t added = *remaining < sizeof(candidate) - prefix ?
                       *remaining : sizeof(candidate) - prefix;
        memcpy(candidate, lane->input, prefix);
        memcpy(candidate + prefix, *source, added);
        uint32_t scalar;
        size_t consumed;
        error = text_read(codec->encoding, candidate, prefix + added, &scalar, &consumed);

        if (error == PARSERUTILS_NEEDDATA) {
            memcpy(lane->input, candidate, prefix + added);
            lane->input_length = prefix + added;
            *source += added;
            *remaining -= added;
            return PARSERUTILS_OK;
        }
        if (error == PARSERUTILS_INVALID) {
            if (codec->mode == PARSERUTILS_CHARSET_CODEC_ERROR_STRICT) return error;
            scalar = 0xfffd;
        } else if (error) return error;

        /* A bad prefix can consist solely of bytes saved by a prior call.
         * Never subtract prefix from consumed with unsigned underflow. */
        size_t from_source = consumed > prefix ? consumed - prefix : 0;
        if (consumed < prefix) {
            memmove(lane->input, lane->input + consumed, prefix - consumed);
            lane->input_length = prefix - consumed;
        } else lane->input_length = 0;
        *source += from_source;
        *remaining -= from_source;

        for (unsigned i = 0; i < 4; i++)
            lane->output[i] = (uint8_t)(scalar >> (24 - 8 * i));
        lane->output_length = 4;
        error = codec_drain(lane, dest, available);
        if (error) return error;
    }
    return PARSERUTILS_OK;
}
