#ifndef LOGIT_FFMPEG_AACSBR_LIBM_H
#define LOGIT_FFMPEG_AACSBR_LIBM_H
#include <math.h>
#include "amath.h"

/* c/lib/audio is freestanding and deliberately owns its transform math in
 * amath rather than depending on the browser's optional libm link. Keep the
 * imported SBR core on that same path on both host and guest; these are the
 * only float libm calls in the narrow import. */
static inline float logit_sbr_powf(float x, float y)
{
    return (float)a_pow((double)x, (double)y);
}
static inline float logit_sbr_log2f(float x)
{
    return (float)a_log2((double)x);
}
static inline float logit_sbr_sqrtf(float x)
{
    return (float)a_sqrt((double)x);
}
static inline long logit_sbr_lrintf(float x)
{
    double lo_d = a_floor((double)x);
    long lo = (long)lo_d;
    double frac = (double)x - lo_d;
    return frac > 0.5 || (frac == 0.5 && (lo & 1)) ? lo + 1 : lo;
}

#define powf   logit_sbr_powf
#define log2f  logit_sbr_log2f
#define sqrtf  logit_sbr_sqrtf
#define lrintf logit_sbr_lrintf
#endif
