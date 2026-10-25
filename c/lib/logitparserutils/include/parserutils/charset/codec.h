/* SPDX-License-Identifier: GPL-3.0-or-later
 * Stateful conversion between an external charset and big-endian UCS-4 bytes.
 * A call consumes source bytes by advancing *source and reducing *sourcelen.
 * A short destination returns NOMEM and retains one complete output character;
 * call again with the remaining input (or zero input) to drain it.
 * Default mode is LOOSE. STRICT leaves an invalid unit unconsumed. TRANSLIT
 * currently uses the documented LOOSE fallback; no linguistic transliterator
 * is installed. Reset drops both lanes' pending bytes but keeps the mode.
 * Zero-length input drains output, not an EOF signal. An unfinished source
 * character stays pending until more bytes arrive or reset discards it.
 */
#ifndef LOGIT_PARSER_CODEC_H
#define LOGIT_PARSER_CODEC_H
#include <parserutils/errors.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct parserutils_charset_codec parserutils_charset_codec;
#define PARSERUTILS_CHARSET_CODEC_NULL 0xffffffffU

typedef enum parserutils_charset_codec_errormode {
    PARSERUTILS_CHARSET_CODEC_ERROR_STRICT = 0,
    PARSERUTILS_CHARSET_CODEC_ERROR_LOOSE = 1,
    PARSERUTILS_CHARSET_CODEC_ERROR_TRANSLIT = 2
} parserutils_charset_codec_errormode;

typedef enum parserutils_charset_codec_opttype {
    PARSERUTILS_CHARSET_CODEC_ERROR_MODE = 1
} parserutils_charset_codec_opttype;

typedef union parserutils_charset_codec_optparams {
    struct { parserutils_charset_codec_errormode mode; } error_mode;
} parserutils_charset_codec_optparams;

parserutils_error parserutils_charset_codec_create(const char *label,
                                                  parserutils_charset_codec **result);
parserutils_error parserutils_charset_codec_destroy(parserutils_charset_codec *codec);
parserutils_error parserutils_charset_codec_setopt(parserutils_charset_codec *codec,
        parserutils_charset_codec_opttype type, parserutils_charset_codec_optparams *params);
parserutils_error parserutils_charset_codec_reset(parserutils_charset_codec *codec);
parserutils_error parserutils_charset_codec_encode(parserutils_charset_codec *codec,
        const uint8_t **source, size_t *remaining, uint8_t **dest, size_t *available);
parserutils_error parserutils_charset_codec_decode(parserutils_charset_codec *codec,
        const uint8_t **source, size_t *remaining, uint8_t **dest, size_t *available);
#ifdef __cplusplus
}
#endif
#endif
