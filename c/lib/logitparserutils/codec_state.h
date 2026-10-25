/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_CODEC_STATE_H
#define LOGIT_CODEC_STATE_H
#include <parserutils/charset/codec.h>

/* Each direction owns at most one unfinished input character and one finished
 * output character. No input-sized allocation or shared encode/decode cursor. */
typedef struct {
    uint8_t input[4];
    uint8_t output[4];
    size_t input_length;
    size_t output_length;
} codec_lane;

struct parserutils_charset_codec {
    uint16_t encoding;
    parserutils_charset_codec_errormode mode;
    codec_lane encoder;
    codec_lane decoder;
};

parserutils_error codec_drain(codec_lane *, uint8_t **, size_t *);
bool codec_arguments(parserutils_charset_codec *, const uint8_t **, size_t *, uint8_t **, size_t *);
#endif
