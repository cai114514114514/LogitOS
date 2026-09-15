#ifndef LOGIT_FFMPEG_AACSBR_AACPS_H
#define LOGIT_FFMPEG_AACSBR_AACPS_H

#include "aac_defines.h"
#include "get_bits.h"

typedef struct AVCodecContext AVCodecContext;

/* HE-AAC v1 does not use Parametric Stereo.  Keeping only the `start` state
 * makes the upstream SBR layout available without silently accepting AOT 29;
 * the public LogitOS decoder continues to reject HE-AAC v2 at ASC parsing. */
typedef struct PSCommonContext { int start; } PSCommonContext;
typedef struct PSContext { PSCommonContext common; } PSContext;

void ff_ps_init(void);
void ff_ps_ctx_init(PSContext *ps);
int ff_ps_read_data(AVCodecContext *avctx, GetBitContext *gb,
                    PSCommonContext *ps, int bits_left);
int ff_ps_apply(AVCodecContext *avctx, PSContext *ps,
                INTFLOAT L[2][38][64], INTFLOAT R[2][38][64], int top);

#endif
