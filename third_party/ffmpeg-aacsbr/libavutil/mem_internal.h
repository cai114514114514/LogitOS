#ifndef LOGIT_FFMPEG_AACSBR_MEM_INTERNAL_H
#define LOGIT_FFMPEG_AACSBR_MEM_INTERNAL_H

#if defined(__GNUC__) || defined(__clang__)
#define DECLARE_ALIGNED(n, t, v) t __attribute__((aligned(n))) v
#define LOGIT_LOCAL_ALIGNED3(t, v, a) t __attribute__((aligned(16))) v a
#define LOGIT_LOCAL_ALIGNED4(t, v, a, b) t __attribute__((aligned(16))) v a b
#define LOGIT_LOCAL_PICK(_1, _2, _3, _4, fn, ...) fn
#define LOCAL_ALIGNED_16(...) \
    LOGIT_LOCAL_PICK(__VA_ARGS__, LOGIT_LOCAL_ALIGNED4, LOGIT_LOCAL_ALIGNED3)(__VA_ARGS__)
#else
#define DECLARE_ALIGNED(n, t, v) t v
#define LOGIT_LOCAL_ALIGNED3(t, v, a) t v a
#define LOGIT_LOCAL_ALIGNED4(t, v, a, b) t v a b
#define LOGIT_LOCAL_PICK(_1, _2, _3, _4, fn, ...) fn
#define LOCAL_ALIGNED_16(...) \
    LOGIT_LOCAL_PICK(__VA_ARGS__, LOGIT_LOCAL_ALIGNED4, LOGIT_LOCAL_ALIGNED3)(__VA_ARGS__)
#endif

#endif
