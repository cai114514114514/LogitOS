#ifndef LOGIT_LAYOUT_RATIO_H
#define LOGIT_LAYOUT_RATIO_H

/* A CSS ratio is two fractional numbers, while layout emits integer pixels.
 * Bound before converting to int: a valid tiny denominator can exceed the
 * engine's pixel range. Divide first so equally large numerator/denominator
 * do not overflow solely because the pixel size was multiplied first. */
static inline int ratio_px(int size, double numerator, double denominator)
{
    if (size <= 0) return 0;
    double result = (double)size * (numerator / denominator);
    if (!(result < 0x3fffffff)) return 0x3fffffff;
    return result > 0 ? (int)result : 0;
}

#endif
