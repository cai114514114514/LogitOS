/* c/lib/video/mpeg12_mc.c -- half-pel motion compensation.
 *
 * 13818-2 7.6.4: a half-pel sample is the average of its two (or four)
 * neighbours, rounded UP -- (a+b+1)>>1 and (a+b+c+d+2)>>2. There is no
 * rounding-control bit here; that is H.263/MPEG-4. Combining a forward and a
 * backward prediction is the same rounded average, applied to the prediction
 * already formed, which is why `avg` rounds a second time rather than summing
 * four or eight taps at once: the two roundings are observable and the
 * standard specifies both.
 *
 * One stride serves the source and the destination. That is not a
 * simplification, it is the shape of every case in the standard: a field
 * prediction writes into a field of the current picture and reads from a
 * field of a reference, both at twice the frame stride; a frame prediction
 * does both at the frame stride.
 */
#include <stdint.h>
#include "mpeg12_int.h"

#define AVG2(a, b)       (((a) + (b) + 1) >> 1)
#define AVG4(a, b, c, e) (((a) + (b) + (c) + (e) + 2) >> 2)

void m12_pred(uint8_t *dst, const uint8_t *src, int stride,
              int w, int h, int hx, int hy, int avg)
{
    int x, y, p;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (!hx && !hy)      p = src[x];
            else if (hx && !hy)  p = AVG2(src[x], src[x + 1]);
            else if (!hx)        p = AVG2(src[x], src[x + stride]);
            else                 p = AVG4(src[x], src[x + 1],
                                          src[x + stride], src[x + stride + 1]);
            dst[x] = (uint8_t)(avg ? AVG2(dst[x], p) : p);
        }
        src += stride;
        dst += stride;
    }
}

/* The clamped path. A conforming MPEG-1/2 stream never asks for a sample
 * outside the coded picture -- the standard requires the referenced area to
 * lie inside it -- so this exists to keep a MALFORMED stream from reading off
 * the end of the plane, and the caller counts how often it fires. A decoder
 * that silently clamps and reports nothing cannot tell "this stream is fine"
 * from "we are inventing samples", which is the whole reason the counter is
 * carried out to mpeg12_counters().
 */
int m12_pred_edge(uint8_t *dst, const uint8_t *plane, int stride,
                  int src_x, int src_y, int pw, int ph,
                  int w, int h, int hx, int hy, int avg)
{
    int x, y, p, a, b, c, e, cx, cy;

    if (src_x >= 0 && src_y >= 0 &&
        src_x + w + hx <= pw && src_y + h + hy <= ph) {
        m12_pred(dst, plane + (long)src_y * stride + src_x, stride,
                 w, h, hx, hy, avg);
        return 0;
    }

#define FETCH(dx, dy)                                                          \
    (cx = src_x + x + (dx), cy = src_y + y + (dy),                             \
     cx = cx < 0 ? 0 : (cx >= pw ? pw - 1 : cx),                               \
     cy = cy < 0 ? 0 : (cy >= ph ? ph - 1 : cy),                               \
     plane[(long)cy * stride + cx])

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            a = FETCH(0, 0);
            if (!hx && !hy)      p = a;
            else if (hx && !hy)  { b = FETCH(1, 0); p = AVG2(a, b); }
            else if (!hx)        { c = FETCH(0, 1); p = AVG2(a, c); }
            else { b = FETCH(1, 0); c = FETCH(0, 1); e = FETCH(1, 1);
                   p = AVG4(a, b, c, e); }
            dst[x] = (uint8_t)(avg ? AVG2(dst[x], p) : p);
        }
        dst += stride;
    }
#undef FETCH
    return 1;
}
