#ifndef LOGIT_FFMPEG_AACSBR_AVASSERT_H
#define LOGIT_FFMPEG_AACSBR_AVASSERT_H

/* These are assertions over state already bounded by the parser.  Trap rather
 * than compiling them out: an invariant violation must not continue into a
 * table access and turn a bad network packet into memory corruption. */
#define av_assert0(c) do { if (!(c)) __builtin_trap(); } while (0)

#endif
