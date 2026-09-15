#ifndef LOGIT_FFMPEG_AACSBR_CODEC_INTERNAL_H
#define LOGIT_FFMPEG_AACSBR_CODEC_INTERNAL_H

#define AV_LOG_ERROR 16
#define AV_LOG_WARNING 24
#define AV_LOG_VERBOSE 40
#define AVERROR_INVALIDDATA (-1094995529)
#define AVERROR_BUG (-558323010)

static inline void av_log(void *ctx, int level, const char *fmt, ...)
{
    (void)ctx; (void)level; (void)fmt;
}

static inline void avpriv_request_sample(void *ctx, const char *what)
{
    (void)ctx; (void)what;
}

#endif
