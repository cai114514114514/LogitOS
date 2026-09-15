#ifndef LOGIT_FFMPEG_AACSBR_INTERNAL_H
#define LOGIT_FFMPEG_AACSBR_INTERNAL_H

#include "attributes.h"
#include <stdint.h>

#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))

union av_intfloat32 { uint32_t i; float f; };

static av_always_inline float ff_exp2fi(int x)
{
    union av_intfloat32 v;
    if (x < -126) return 0.0f;
    if (x > 127) return __builtin_inff();
    v.i = (uint32_t)(x + 127) << 23;
    return v.f;
}

#endif
