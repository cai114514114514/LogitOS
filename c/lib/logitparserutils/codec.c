/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "codec_state.h"
#include "text.h"
#include <parserutils/charset/mibenum.h>
#include <stdlib.h>
#include <string.h>

parserutils_error parserutils_charset_codec_create(const char *label,
                                                  parserutils_charset_codec **result)
{
    if (!label || !result) return PARSERUTILS_BADPARM;
    *result = NULL;
    uint16_t encoding = parserutils_charset_mibenum_from_name(label, strlen(label));
    if (!text_supported(encoding)) return PARSERUTILS_BADENCODING;
    parserutils_charset_codec *codec = calloc(1, sizeof(*codec));
    if (!codec) return PARSERUTILS_NOMEM;
    /* The standalone API's UTF-16 format is native order, as are its UTF-16
     * utilities. Explicit UTF-16LE/BE and the stylesheet BOM path are separate. */
    codec->encoding = encoding == TEXT_UTF16 ? text_native_utf16() : encoding;
    codec->mode = PARSERUTILS_CHARSET_CODEC_ERROR_LOOSE;
    *result = codec;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_codec_destroy(parserutils_charset_codec *codec)
{
    if (!codec) return PARSERUTILS_BADPARM;
    free(codec);
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_codec_setopt(parserutils_charset_codec *codec,
        parserutils_charset_codec_opttype type, parserutils_charset_codec_optparams *params)
{
    if (!codec || !params || type != PARSERUTILS_CHARSET_CODEC_ERROR_MODE)
        return PARSERUTILS_BADPARM;
    unsigned mode = (unsigned)params->error_mode.mode;
    if (mode > PARSERUTILS_CHARSET_CODEC_ERROR_TRANSLIT) return PARSERUTILS_BADPARM;
    codec->mode = params->error_mode.mode;
    return PARSERUTILS_OK;
}

parserutils_error parserutils_charset_codec_reset(parserutils_charset_codec *codec)
{
    if (!codec) return PARSERUTILS_BADPARM;
    memset(&codec->encoder, 0, sizeof(codec->encoder));
    memset(&codec->decoder, 0, sizeof(codec->decoder));
    return PARSERUTILS_OK;
}

bool codec_arguments(parserutils_charset_codec *codec, const uint8_t **source,
                     size_t *remaining, uint8_t **dest, size_t *available)
{
    return codec && source && remaining && dest && available &&
           (*source || !*remaining) && (*dest || !*available);
}

parserutils_error codec_drain(codec_lane *lane, uint8_t **dest, size_t *available)
{
    if (!lane->output_length) return PARSERUTILS_OK;
    if (*available < lane->output_length) return PARSERUTILS_NOMEM;
    memcpy(*dest, lane->output, lane->output_length);
    *dest += lane->output_length;
    *available -= lane->output_length;
    lane->output_length = 0;
    return PARSERUTILS_OK;
}
