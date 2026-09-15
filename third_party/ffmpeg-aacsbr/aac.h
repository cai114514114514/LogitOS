#ifndef LOGIT_FFMPEG_AACSBR_AAC_H
#define LOGIT_FFMPEG_AACSBR_AAC_H

#include <stddef.h>
#include "config.h"
#include "aac_defines.h"

#define TYPE_SCE 0
#define TYPE_CPE 1
#define TYPE_CCE 2
#define TYPE_LFE 3
#define FF_PROFILE_AAC_HE_V2 28

typedef struct AVCodecContext {
    int profile;
} AVCodecContext;

typedef struct AVFloatDSPContext {
    void (*vector_fmul_reverse)(float *, const float *, const float *, int);
    void (*vector_fmul)(float *, const float *, const float *, int);
    void (*vector_fmul_add)(float *, const float *, const float *,
                            const float *, int);
} AVFloatDSPContext;

typedef struct MPEG4AudioConfig {
    int sample_rate;
    int ext_sample_rate;
    int ps;
} MPEG4AudioConfig;

typedef struct AACOutputConfig {
    MPEG4AudioConfig m4ac;
} AACOutputConfig;

typedef struct AACContext {
    AVCodecContext *avctx;
    AVFloatDSPContext *fdsp;
    AACOutputConfig oc[2];
} AACContext;

#endif
