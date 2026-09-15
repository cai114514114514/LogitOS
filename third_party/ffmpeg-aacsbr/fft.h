#ifndef LOGIT_FFMPEG_AACSBR_FFT_H
#define LOGIT_FFMPEG_AACSBR_FFT_H

#include "afft.h"

typedef struct FFTContext FFTContext;
struct FFTContext {
    int mdct_bits;
    double scale;
    amdct *plan;
    void (*imdct_half)(FFTContext *, float *, const float *);
};

int ff_mdct_init(FFTContext *s, int bits, int inverse, double scale);
void ff_mdct_end(FFTContext *s);

#endif
