#ifndef LOGIT_FFMPEG_AACSBR_QSORT_H
#define LOGIT_FFMPEG_AACSBR_QSORT_H
#include <stdlib.h>
#define AV_QSORT(base, count, type, cmp) \
    qsort((base), (count), sizeof(type), (cmp))
#endif
