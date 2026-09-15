/* c/lib/video/mpeg4_mc.c -- motion compensation for MPEG-4 Part 2 and H.263.
 *
 * Three interpolators live here and they are not variations on each other:
 *
 *   HALF-PEL (H.263 and MPEG-4 Simple) is a bilinear average of the four
 *   neighbouring integer samples, and it has TWO roundings. The picture
 *   header's rounding_type bit selects +2 or +1 in the shift, alternating per
 *   picture so that repeated half-pel prediction of a static area does not
 *   drift in one direction. A decoder that ignores the bit still produces a
 *   plausible picture that is wrong by one step almost everywhere.
 *
 *   QUARTER-PEL (MPEG-4 Advanced Simple) is an 8-tap (1,-3,6,20,20,-6,3,-1)/32
 *   half-sample filter followed by a bilinear step to the quarter position.
 *   Its edge behaviour is MIRRORING, not clamping: the tap that would read one
 *   sample past the left edge reads sample 0, two past reads sample 1, three
 *   past reads sample 2 -- a reflection about -0.5, and the same about w+0.5
 *   on the right. Clamping instead is the classic wrong answer; it agrees with
 *   mirroring on the first tap and differs on the other two.
 *   -DMPEG4_QPEL_NO_ROUND drops the +16 rounding from the filter's shift,
 *   which is the negative control tests/mpeg4.mk watches redden.
 *
 *   CHROMA. Chroma always moves at half-pel, whatever the luma is doing. For
 *   a four-MV macroblock the four luma vectors are folded into ONE chroma
 *   vector through a table (14496-2 7.6.2 / H.263 6.1.1) rather than by
 *   averaging -- the table is a rounding rule chosen so the result is
 *   symmetric about zero, and averaging is off by one for half the inputs.
 *
 * Out-of-picture reads never touch memory outside a plane: the block is
 * copied into a scratch buffer first with the picture's border replicated.
 * That replication is the specified behaviour (unrestricted motion vectors),
 * not a safety net bolted on top -- a decoder that clipped the VECTOR instead
 * would decode a different picture.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "mpeg4_int.h"

#define M4_OP_PUT     0
#define M4_OP_PUT_NR  1
#define M4_OP_AVG     2

static inline int m4_clipi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t clip8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* Left-shifting a NEGATIVE signed value is undefined behaviour in C
 * regardless of the shift amount -- even by 0, per 6.5.7p4 -- and
 * `src_y`/`uvsrc_y` here are motion-derived coordinates that a motion
 * vector pushing the block above/left of the picture legitimately makes
 * negative (`field_based` is always 0 for a non-interlaced VOP, so the
 * shift is a literal no-op in that case and the UB was invisible on every
 * ordinary build -- found by tests/unit/mpeg4_mc_test.c under UBSan,
 * `-DUBSAN` flags it at src_y=-10, field_based=0). The unsigned-then-cast
 * idiom is well-defined (2's-complement wraparound) and reduces to the
 * identical instruction on every target this tree builds for -- same fix
 * shape as gfx_raster.c's `(x1-x0) << 16`, see CLAUDE.md. */
static inline int shl_coord(int v, int s)
{
    return (int)((unsigned)v << s);
}

/* ------------------------------------------------------------- edge copy -- */
/* Copy a block_w x block_h block whose top-left corner sits at (src_x, src_y)
 * inside a w x h picture, replicating the border for anything outside. `src`
 * already points at the (src_x, src_y) sample. */
static void edge_mc(uint8_t *buf, const uint8_t *src,
                    int buf_ls, int src_ls,
                    int block_w, int block_h, int src_x, int src_y,
                    int w, int h)
{
    int x, y;
    const uint8_t *origin = src - (ptrdiff_t)src_y * src_ls - src_x;

    if (w <= 0 || h <= 0) return;
    for (y = 0; y < block_h; y++) {
        int sy = m4_clipi(src_y + y, 0, h - 1);
        const uint8_t *row = origin + (ptrdiff_t)sy * src_ls;
        uint8_t *dst = buf + (ptrdiff_t)y * buf_ls;
        for (x = 0; x < block_w; x++)
            dst[x] = row[m4_clipi(src_x + x, 0, w - 1)];
    }
}

/* --------------------------------------------------------------- half-pel -- */
/* dxy: bit0 = half-sample horizontally, bit1 = half-sample vertically. */
static void hpel(uint8_t *dst, const uint8_t *src, int stride, int w, int h,
                 int dxy, int op)
{
    int x, y;
    /* The negative control: MPEG4_HPEL_CONTROL_NO_ROUND drops the
     * rounding_type bit's effect entirely -- the file header's own words,
     * "a decoder that ignores the bit still produces a plausible picture
     * that is wrong by one step almost everywhere." Only redderns a
     * position whose 1/2/4-neighbour sum is not already even (dxy==0,
     * full-pel copy, is unaffected by construction). */
#ifdef MPEG4_HPEL_CONTROL_NO_ROUND
    int rnd = 0;
#else
    int rnd = (op == M4_OP_PUT_NR) ? 0 : 1;   /* the rounding_type bit */
#endif
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int v;
            switch (dxy) {
            case 0: v = src[x]; break;
            case 1: v = (src[x] + src[x + 1] + rnd) >> 1; break;
            case 2: v = (src[x] + src[x + stride] + rnd) >> 1; break;
            default:
                v = (src[x] + src[x + 1] + src[x + stride] + src[x + stride + 1]
                     + 1 + rnd) >> 2;
                break;
            }
            dst[x] = (op == M4_OP_AVG) ? (uint8_t)((dst[x] + v + 1) >> 1)
                                       : (uint8_t)v;
        }
        dst += stride;
        src += stride;
    }
}

/* Same, but source and destination have independent strides (used only by the
 * qpel bilinear step, which averages two scratch buffers). */
static void avg2(uint8_t *dst, const uint8_t *s1, const uint8_t *s2,
                 int ds, int s1s, int s2s, int w, int h, int op)
{
    int x, y;
    int rnd = (op == M4_OP_PUT_NR) ? 0 : 1;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int v = (s1[x] + s2[x] + rnd) >> 1;
            dst[x] = (op == M4_OP_AVG) ? (uint8_t)((dst[x] + v + 1) >> 1)
                                       : (uint8_t)v;
        }
        dst += ds; s1 += s1s; s2 += s2s;
    }
}

/* ------------------------------------------------------------ quarter-pel -- */
/* The rounding the negative control removes. */
#ifdef MPEG4_QPEL_NO_ROUND
#define QPEL_RND(op) 0
#else
#define QPEL_RND(op) ((op) == M4_OP_PUT_NR ? 15 : 16)
#endif

/* Reflect a tap index about -0.5 on the left and w+0.5 on the right. */
static inline int mir(int j, int w)
{
    if (j < 0)  return -1 - j;
    if (j > w)  return 2 * w + 1 - j;
    return j;
}

static void lowpass_h(uint8_t *dst, const uint8_t *src, int ds, int ss,
                      int w, int h, int op)
{
    const int rnd = QPEL_RND(op);
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int v = (src[mir(x, w)] + src[mir(x + 1, w)]) * 20
                  - (src[mir(x - 1, w)] + src[mir(x + 2, w)]) * 6
                  + (src[mir(x - 2, w)] + src[mir(x + 3, w)]) * 3
                  - (src[mir(x - 3, w)] + src[mir(x + 4, w)]);
            v = clip8((v + rnd) >> 5);
            dst[x] = (op == M4_OP_AVG) ? (uint8_t)((dst[x] + v + 1) >> 1)
                                       : (uint8_t)v;
        }
        dst += ds; src += ss;
    }
}

static void lowpass_v(uint8_t *dst, const uint8_t *src, int ds, int ss,
                      int w, int op)
{
    const int rnd = QPEL_RND(op);
    int x, y;
    for (x = 0; x < w; x++) {
        for (y = 0; y < w; y++) {
            int v = (src[mir(y, w) * ss + x]     + src[mir(y + 1, w) * ss + x]) * 20
                  - (src[mir(y - 1, w) * ss + x] + src[mir(y + 2, w) * ss + x]) * 6
                  + (src[mir(y - 2, w) * ss + x] + src[mir(y + 3, w) * ss + x]) * 3
                  - (src[mir(y - 3, w) * ss + x] + src[mir(y + 4, w) * ss + x]);
            v = clip8((v + rnd) >> 5);
            dst[y * ds + x] = (op == M4_OP_AVG)
                ? (uint8_t)((dst[y * ds + x] + v + 1) >> 1) : (uint8_t)v;
        }
    }
}

static void copy_block(uint8_t *dst, const uint8_t *src, int ds, int ss,
                       int w, int h)
{
    int y;
    for (y = 0; y < h; y++) memcpy(dst + (ptrdiff_t)y * ds, src + (ptrdiff_t)y * ss, (size_t)w);
}

/* Intermediate steps of a qpel interpolation always use the picture's own
 * rounding mode (put or put_no_rnd); only the FINAL step averages into the
 * destination when this is the second reference of a bi-predicted block. */
static inline int mid_op(int op) { return op == M4_OP_AVG ? M4_OP_PUT : op; }

/* dxy = (my & 3) << 2 | (mx & 3). S is 8 or 16. */
static void qpel_block(uint8_t *dst, const uint8_t *src, int stride,
                       int S, int dxy, int op)
{
    const int FS = S + 8;              /* stride of the `full` scratch copy */
    const int HS = S;                  /* stride of the half-sample scratch */
    const int mop = mid_op(op);
    uint8_t full[24 * 17];
    uint8_t halfH[24 * 17];
    uint8_t halfHV[16 * 16];

    switch (dxy) {
    case 0:                             /* mc00 */
        hpel(dst, src, stride, S, S, 0, op);
        break;
    case 1:                             /* mc10 */
        lowpass_h(halfH, src, HS, stride, S, S, mop);
        avg2(dst, src, halfH, stride, stride, HS, S, S, op);
        break;
    case 2:                             /* mc20 */
        lowpass_h(dst, src, stride, stride, S, S, op);
        break;
    case 3:                             /* mc30 */
        lowpass_h(halfH, src, HS, stride, S, S, mop);
        avg2(dst, src + 1, halfH, stride, stride, HS, S, S, op);
        break;
    case 4:                             /* mc01 */
        copy_block(full, src, FS, stride, S, S + 1);
        lowpass_v(halfH, full, HS, FS, S, mop);
        avg2(dst, full, halfH, stride, FS, HS, S, S, op);
        break;
    case 8:                             /* mc02 */
        copy_block(full, src, FS, stride, S, S + 1);
        lowpass_v(dst, full, stride, FS, S, op);
        break;
    case 12:                            /* mc03 */
        copy_block(full, src, FS, stride, S, S + 1);
        lowpass_v(halfH, full, HS, FS, S, mop);
        avg2(dst, full + FS, halfH, stride, FS, HS, S, S, op);
        break;
    case 5:                             /* mc11 */
    case 7:                             /* mc31 */
    case 13:                            /* mc13 */
    case 15: {                          /* mc33 */
        const int right = (dxy == 7 || dxy == 15);
        const int lower = (dxy == 13 || dxy == 15);
        copy_block(full, src, FS, stride, S + 1, S + 1);
        lowpass_h(halfH, full, HS, FS, S, S + 1, mop);
        avg2(halfH, halfH, full + (right ? 1 : 0), HS, HS, FS, S, S + 1, mop);
        lowpass_v(halfHV, halfH, S, HS, S, mop);
        avg2(dst, halfH + (lower ? HS : 0), halfHV, stride, HS, S, S, S, op);
        break;
    }
    case 6:                             /* mc21 */
        lowpass_h(halfH, src, HS, stride, S, S + 1, mop);
        lowpass_v(halfHV, halfH, S, HS, S, mop);
        avg2(dst, halfH, halfHV, stride, HS, S, S, S, op);
        break;
    case 14:                            /* mc23 */
        lowpass_h(halfH, src, HS, stride, S, S + 1, mop);
        lowpass_v(halfHV, halfH, S, HS, S, mop);
        avg2(dst, halfH + HS, halfHV, stride, HS, S, S, S, op);
        break;
    case 9:                             /* mc12 */
    case 11:                            /* mc32 */
        copy_block(full, src, FS, stride, S + 1, S + 1);
        lowpass_h(halfH, full, HS, FS, S, S + 1, mop);
        avg2(halfH, halfH, full + (dxy == 11 ? 1 : 0), HS, HS, FS, S, S + 1, mop);
        lowpass_v(dst, halfH, stride, HS, S, op);
        break;
    default:                            /* 10: mc22 */
        lowpass_h(halfH, src, HS, stride, S, S + 1, mop);
        lowpass_v(dst, halfH, stride, HS, S, op);
        break;
    }
}

/* --------------------------------------------------------- emu scratch ---- */
static uint8_t *emu_luma(m4ctx *s)  { return s->emu; }
static uint8_t *emu_cb(m4ctx *s)    { return s->emu + 18 * s->linesize; }
static uint8_t *emu_cr(m4ctx *s)    { return emu_cb(s) + 10 * s->uvlinesize; }

/* ------------------------------------------------------------ 16x16 hpel -- */
static void mpeg_motion(m4ctx *s, uint8_t *dy, uint8_t *dcb, uint8_t *dcr,
                        int field_based, int bottom_field, int field_select,
                        uint8_t *const *ref, int op,
                        int motion_x, int motion_y, int h, int mb_y)
{
    const uint8_t *py, *pcb, *pcr;
    int dxy, uvdxy, src_x, src_y, uvsrc_x, uvsrc_y, v_edge_pos;
    int linesize, uvlinesize, block_y_half;

    v_edge_pos   = s->v_edge_pos >> field_based;
    linesize     = s->linesize   << field_based;
    uvlinesize   = s->uvlinesize << field_based;
    block_y_half = field_based;

    dxy   = ((motion_y & 1) << 1) | (motion_x & 1);
    src_x = s->mb_x * 16 + (motion_x >> 1);
    src_y = (mb_y << (4 - block_y_half)) + (motion_y >> 1);

    uvdxy   = dxy | (motion_y & 2) | ((motion_x & 2) >> 1);
    uvsrc_x = src_x >> 1;
    uvsrc_y = src_y >> 1;

    py  = ref[0] + (ptrdiff_t)src_y   * linesize   + src_x;
    pcb = ref[1] + (ptrdiff_t)uvsrc_y * uvlinesize + uvsrc_x;
    pcr = ref[2] + (ptrdiff_t)uvsrc_y * uvlinesize + uvsrc_x;

    if ((unsigned)src_x >= (unsigned)(s->h_edge_pos - (motion_x & 1) - 15 > 0
                                      ? s->h_edge_pos - (motion_x & 1) - 15 : 0) ||
        (unsigned)src_y >= (unsigned)(v_edge_pos - (motion_y & 1) - h + 1 > 0
                                      ? v_edge_pos - (motion_y & 1) - h + 1 : 0)) {
        int esy = shl_coord(src_y, field_based);
        int euy = shl_coord(uvsrc_y, field_based);
        edge_mc(emu_luma(s), py, s->linesize, s->linesize,
                17, 17 + field_based, src_x, esy, s->h_edge_pos, s->v_edge_pos);
        py = emu_luma(s);
        edge_mc(emu_cb(s), pcb, s->uvlinesize, s->uvlinesize,
                9, 9 + field_based, uvsrc_x, euy,
                s->h_edge_pos >> 1, s->v_edge_pos >> 1);
        edge_mc(emu_cr(s), pcr, s->uvlinesize, s->uvlinesize,
                9, 9 + field_based, uvsrc_x, euy,
                s->h_edge_pos >> 1, s->v_edge_pos >> 1);
        pcb = emu_cb(s);
        pcr = emu_cr(s);
    }

    if (bottom_field) {
        dy  += s->linesize;
        dcb += s->uvlinesize;
        dcr += s->uvlinesize;
    }
    if (field_select) {
        py  += s->linesize;
        pcb += s->uvlinesize;
        pcr += s->uvlinesize;
    }

    hpel(dy, py, linesize, 16, h, dxy, op);
    hpel(dcb, pcb, uvlinesize, 8, h >> 1, uvdxy, op);
    hpel(dcr, pcr, uvlinesize, 8, h >> 1, uvdxy, op);
}

/* --------------------------------------------------------------- 8x8 hpel -- */
static void hpel_motion(m4ctx *s, uint8_t *dest, uint8_t *src,
                        int src_x, int src_y, int op, int motion_x, int motion_y)
{
    int dxy = 0;

    src_x += motion_x >> 1;
    src_y += motion_y >> 1;

    src_x = m4_clipi(src_x, -16, s->width);
    if (src_x != s->width) dxy |= motion_x & 1;
    src_y = m4_clipi(src_y, -16, s->height);
    if (src_y != s->height) dxy |= (motion_y & 1) << 1;
    src += (ptrdiff_t)src_y * s->linesize + src_x;

    if ((unsigned)src_x >= (unsigned)(s->h_edge_pos - (motion_x & 1) - 7 > 0
                                      ? s->h_edge_pos - (motion_x & 1) - 7 : 0) ||
        (unsigned)src_y >= (unsigned)(s->v_edge_pos - (motion_y & 1) - 7 > 0
                                      ? s->v_edge_pos - (motion_y & 1) - 7 : 0)) {
        edge_mc(emu_luma(s), src, s->linesize, s->linesize, 9, 9,
                src_x, src_y, s->h_edge_pos, s->v_edge_pos);
        src = emu_luma(s);
    }
    hpel(dest, src, s->linesize, 8, 8, dxy, op);
}

/* --------------------------------------------------------- 16x16 qpel ----- */
static void qpel_motion(m4ctx *s, uint8_t *dy, uint8_t *dcb, uint8_t *dcr,
                        int field_based, int bottom_field, int field_select,
                        uint8_t *const *ref, int op,
                        int motion_x, int motion_y, int h)
{
    const uint8_t *py, *pcb, *pcr;
    int dxy, uvdxy, mx, my, src_x, src_y, uvsrc_x, uvsrc_y, v_edge_pos;
    int linesize, uvlinesize;

    dxy   = ((motion_y & 3) << 2) | (motion_x & 3);
    src_x = s->mb_x *  16                 + (motion_x >> 2);
    src_y = s->mb_y * (16 >> field_based) + (motion_y >> 2);

    v_edge_pos = s->v_edge_pos >> field_based;
    linesize   = s->linesize   << field_based;
    uvlinesize = s->uvlinesize << field_based;

    if (field_based) {
        mx = motion_x / 2;
        my = motion_y >> 1;
    } else {
        mx = motion_x / 2;
        my = motion_y / 2;
    }
    mx = (mx >> 1) | (mx & 1);
    my = (my >> 1) | (my & 1);

    uvdxy = (mx & 1) | ((my & 1) << 1);
    mx  >>= 1;
    my  >>= 1;

    uvsrc_x = s->mb_x *  8                 + mx;
    uvsrc_y = s->mb_y * (8 >> field_based) + my;

    py  = ref[0] + (ptrdiff_t)src_y   * linesize   + src_x;
    pcb = ref[1] + (ptrdiff_t)uvsrc_y * uvlinesize + uvsrc_x;
    pcr = ref[2] + (ptrdiff_t)uvsrc_y * uvlinesize + uvsrc_x;

    if ((unsigned)src_x >= (unsigned)(s->h_edge_pos - (motion_x & 3) - 15 > 0
                                      ? s->h_edge_pos - (motion_x & 3) - 15 : 0) ||
        (unsigned)src_y >= (unsigned)(v_edge_pos - (motion_y & 3) - h + 1 > 0
                                      ? v_edge_pos - (motion_y & 3) - h + 1 : 0)) {
        edge_mc(emu_luma(s), py, s->linesize, s->linesize,
                17, 17 + field_based, src_x, shl_coord(src_y, field_based),
                s->h_edge_pos, s->v_edge_pos);
        py = emu_luma(s);
        edge_mc(emu_cb(s), pcb, s->uvlinesize, s->uvlinesize,
                9, 9 + field_based, uvsrc_x, shl_coord(uvsrc_y, field_based),
                s->h_edge_pos >> 1, s->v_edge_pos >> 1);
        edge_mc(emu_cr(s), pcr, s->uvlinesize, s->uvlinesize,
                9, 9 + field_based, uvsrc_x, shl_coord(uvsrc_y, field_based),
                s->h_edge_pos >> 1, s->v_edge_pos >> 1);
        pcb = emu_cb(s);
        pcr = emu_cr(s);
    }

    if (!field_based) {
        qpel_block(dy, py, linesize, 16, dxy, op);
    } else {
        if (bottom_field) {
            dy  += s->linesize;
            dcb += s->uvlinesize;
            dcr += s->uvlinesize;
        }
        if (field_select) {
            py  += s->linesize;
            pcb += s->uvlinesize;
            pcr += s->uvlinesize;
        }
        qpel_block(dy,     py,     linesize, 8, dxy, op);
        qpel_block(dy + 8, py + 8, linesize, 8, dxy, op);
    }
    hpel(dcr, pcr, uvlinesize, 8, h >> 1, uvdxy, op);
    hpel(dcb, pcb, uvlinesize, 8, h >> 1, uvdxy, op);
}

/* --------------------------------------------------------- chroma 4MV ----- */
/* 14496-2 7.6.2: fold the four luma vectors into one chroma vector with the
 * table, not by averaging. */
static inline int round_chroma(int x)
{
    return m4_chroma_roundtab[x & 0xf] + (x >> 3);
}

static void chroma_4mv_motion(m4ctx *s, uint8_t *dcb, uint8_t *dcr,
                              uint8_t *const *ref, int op, int mx, int my)
{
    const uint8_t *ptr;
    int src_x, src_y, dxy, emu = 0;
    ptrdiff_t offset;

    mx = round_chroma(mx);
    my = round_chroma(my);

    dxy  = ((my & 1) << 1) | (mx & 1);
    mx >>= 1;
    my >>= 1;

    src_x = s->mb_x * 8 + mx;
    src_y = s->mb_y * 8 + my;
    src_x = m4_clipi(src_x, -8, s->width >> 1);
    if (src_x == (s->width >> 1))  dxy &= ~1;
    src_y = m4_clipi(src_y, -8, s->height >> 1);
    if (src_y == (s->height >> 1)) dxy &= ~2;

    offset = (ptrdiff_t)src_y * s->uvlinesize + src_x;
    ptr    = ref[1] + offset;
    if ((unsigned)src_x >= (unsigned)((s->h_edge_pos >> 1) - (dxy & 1) - 7 > 0
                                      ? (s->h_edge_pos >> 1) - (dxy & 1) - 7 : 0) ||
        (unsigned)src_y >= (unsigned)((s->v_edge_pos >> 1) - (dxy >> 1) - 7 > 0
                                      ? (s->v_edge_pos >> 1) - (dxy >> 1) - 7 : 0)) {
        edge_mc(emu_luma(s), ptr, s->uvlinesize, s->uvlinesize, 9, 9,
                src_x, src_y, s->h_edge_pos >> 1, s->v_edge_pos >> 1);
        ptr = emu_luma(s);
        emu = 1;
    }
    hpel(dcb, ptr, s->uvlinesize, 8, 8, dxy, op);

    ptr = ref[2] + offset;
    if (emu) {
        edge_mc(emu_luma(s), ptr, s->uvlinesize, s->uvlinesize, 9, 9,
                src_x, src_y, s->h_edge_pos >> 1, s->v_edge_pos >> 1);
        ptr = emu_luma(s);
    }
    hpel(dcr, ptr, s->uvlinesize, 8, 8, dxy, op);
}

/* ------------------------------------------------------------ four-MV ----- */
static void apply_8x8(m4ctx *s, uint8_t *dy, uint8_t *dcb, uint8_t *dcr,
                      int dir, uint8_t *const *ref, int op)
{
    int dxy, src_x, src_y, i;
    int mx = 0, my = 0;
    const uint8_t *ptr;

    if (s->quarter_sample) {
        for (i = 0; i < 4; i++) {
            int motion_x = s->mv[dir][i][0];
            int motion_y = s->mv[dir][i][1];

            dxy   = ((motion_y & 3) << 2) | (motion_x & 3);
            src_x = s->mb_x * 16 + (motion_x >> 2) + (i & 1) * 8;
            src_y = s->mb_y * 16 + (motion_y >> 2) + (i >> 1) * 8;

            src_x = m4_clipi(src_x, -16, s->width);
            if (src_x == s->width)  dxy &= ~3;
            src_y = m4_clipi(src_y, -16, s->height);
            if (src_y == s->height) dxy &= ~12;

            ptr = ref[0] + (ptrdiff_t)src_y * s->linesize + src_x;
            if ((unsigned)src_x >= (unsigned)(s->h_edge_pos - (motion_x & 3) - 7 > 0
                                              ? s->h_edge_pos - (motion_x & 3) - 7 : 0) ||
                (unsigned)src_y >= (unsigned)(s->v_edge_pos - (motion_y & 3) - 7 > 0
                                              ? s->v_edge_pos - (motion_y & 3) - 7 : 0)) {
                edge_mc(emu_luma(s), ptr, s->linesize, s->linesize, 9, 9,
                        src_x, src_y, s->h_edge_pos, s->v_edge_pos);
                ptr = emu_luma(s);
            }
            qpel_block(dy + ((i & 1) * 8) + (ptrdiff_t)(i >> 1) * 8 * s->linesize,
                       ptr, s->linesize, 8, dxy, op);

            mx += s->mv[dir][i][0] / 2;
            my += s->mv[dir][i][1] / 2;
        }
    } else {
        for (i = 0; i < 4; i++) {
            hpel_motion(s,
                        dy + ((i & 1) * 8) + (ptrdiff_t)(i >> 1) * 8 * s->linesize,
                        ref[0],
                        s->mb_x * 16 + (i & 1) * 8,
                        s->mb_y * 16 + (i >> 1) * 8,
                        op, s->mv[dir][i][0], s->mv[dir][i][1]);
            mx += s->mv[dir][i][0];
            my += s->mv[dir][i][1];
        }
    }
    chroma_4mv_motion(s, dcb, dcr, ref, op, mx, my);
}

/* -------------------------------------------------------------- dispatch -- */
void m4_mc(m4ctx *s, uint8_t *dy, uint8_t *dcb, uint8_t *dcr, int dir,
           uint8_t *const *ref, int op)
{
    int i;

    switch (s->mv_type) {
    case M4_MV_16X16:
        if (s->quarter_sample)
            qpel_motion(s, dy, dcb, dcr, 0, 0, 0, ref, op,
                        s->mv[dir][0][0], s->mv[dir][0][1], 16);
        else
            mpeg_motion(s, dy, dcb, dcr, 0, 0, 0, ref, op,
                        s->mv[dir][0][0], s->mv[dir][0][1], 16, s->mb_y);
        break;
    case M4_MV_8X8:
        apply_8x8(s, dy, dcb, dcr, dir, ref, op);
        break;
    case M4_MV_FIELD:
        if (s->quarter_sample) {
            for (i = 0; i < 2; i++)
                qpel_motion(s, dy, dcb, dcr, 1, i, s->field_select[dir][i],
                            ref, op, s->mv[dir][i][0], s->mv[dir][i][1], 8);
        } else {
            mpeg_motion(s, dy, dcb, dcr, 1, 0, s->field_select[dir][0],
                        ref, op, s->mv[dir][0][0], s->mv[dir][0][1], 8, s->mb_y);
            mpeg_motion(s, dy, dcb, dcr, 1, 1, s->field_select[dir][1],
                        ref, op, s->mv[dir][1][0], s->mv[dir][1][1], 8, s->mb_y);
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------- H.263 loop filter --- */
/* H.263 Annex J. Applied to the reconstructed picture at the block edges of
 * the macroblock just decoded and of its already-finished neighbours, so the
 * filtered samples feed the NEXT picture's prediction -- it is IN the loop,
 * not a post-process, and a decoder that skipped it drifts.
 *
 * Two details are deliberately not "cleaned up": p1/p2 are clamped by the
 * bit-8 test (`if (p & 256) p = ~(p >> 31)`) rather than by a conditional
 * pair, and p0/p3 are stored with NO clamp at all -- the outer samples move by
 * at most |d1|/2 and the standard's arithmetic lets that wrap. Both are what
 * the pinned reference does, and both are observable on a saturated edge.
 */
static void h263_h_loop_filter(uint8_t *src, int stride, int qscale)
{
    const int strength = m4_h263_loop_strength[qscale];
    int y;

    for (y = 0; y < 8; y++) {
        int d1, d2, ad1;
        int p0 = src[y * stride - 2];
        int p1 = src[y * stride - 1];
        int p2 = src[y * stride + 0];
        int p3 = src[y * stride + 1];
        int d  = (p0 - p3 + 4 * (p2 - p1)) / 8;

        if      (d < -2 * strength) d1 = 0;
        else if (d < -strength)     d1 = -2 * strength - d;
        else if (d <  strength)     d1 = d;
        else if (d <  2 * strength) d1 = 2 * strength - d;
        else                        d1 = 0;

        p1 += d1;
        p2 -= d1;
        if (p1 & 256) p1 = ~(p1 >> 31);
        if (p2 & 256) p2 = ~(p2 >> 31);
        src[y * stride - 1] = (uint8_t)p1;
        src[y * stride + 0] = (uint8_t)p2;

        ad1 = (d1 < 0 ? -d1 : d1) >> 1;
        d2  = m4_clipi((p0 - p3) / 4, -ad1, ad1);
        src[y * stride - 2] = (uint8_t)(p0 - d2);
        src[y * stride + 1] = (uint8_t)(p3 + d2);
    }
}

static void h263_v_loop_filter(uint8_t *src, int stride, int qscale)
{
    const int strength = m4_h263_loop_strength[qscale];
    int x;

    for (x = 0; x < 8; x++) {
        int d1, d2, ad1;
        int p0 = src[x - 2 * stride];
        int p1 = src[x - 1 * stride];
        int p2 = src[x + 0 * stride];
        int p3 = src[x + 1 * stride];
        int d  = (p0 - p3 + 4 * (p2 - p1)) / 8;

        if      (d < -2 * strength) d1 = 0;
        else if (d < -strength)     d1 = -2 * strength - d;
        else if (d <  strength)     d1 = d;
        else if (d <  2 * strength) d1 = 2 * strength - d;
        else                        d1 = 0;

        p1 += d1;
        p2 -= d1;
        if (p1 & 256) p1 = ~(p1 >> 31);
        if (p2 & 256) p2 = ~(p2 >> 31);
        src[x - 1 * stride] = (uint8_t)p1;
        src[x + 0 * stride] = (uint8_t)p2;

        ad1 = (d1 < 0 ? -d1 : d1) >> 1;
        d2  = m4_clipi((p0 - p3) / 4, -ad1, ad1);
        src[x - 2 * stride] = (uint8_t)(p0 - d2);
        src[x + stride]     = (uint8_t)(p3 + d2);
    }
}

void m4_loop_filter(m4ctx *s)
{
    int qp_c;
    const int linesize   = s->linesize;
    const int uvlinesize = s->uvlinesize;
    const int xy = s->mb_y * s->mb_stride + s->mb_x;
    uint8_t *dest_y  = s->dest[0];
    uint8_t *dest_cb = s->dest[1];
    uint8_t *dest_cr = s->dest[2];

    if (!(s->cur->mb_type[xy] & M4_MB_SKIP)) {
        qp_c = s->qscale;
        h263_v_loop_filter(dest_y + 8 * linesize,     linesize, qp_c);
        h263_v_loop_filter(dest_y + 8 * linesize + 8, linesize, qp_c);
    } else
        qp_c = 0;

    if (s->mb_y) {
        int qp_dt, qp_tt, qp_tc;

        if (s->cur->mb_type[xy - s->mb_stride] & M4_MB_SKIP) qp_tt = 0;
        else qp_tt = s->cur->qscale_table[xy - s->mb_stride];

        qp_tc = qp_c ? qp_c : qp_tt;

        if (qp_tc) {
            const int cqp = s->chroma_qscale_table[qp_tc];
            h263_v_loop_filter(dest_y,     linesize, qp_tc);
            h263_v_loop_filter(dest_y + 8, linesize, qp_tc);
            h263_v_loop_filter(dest_cb, uvlinesize, cqp);
            h263_v_loop_filter(dest_cr, uvlinesize, cqp);
        }
        if (qp_tt)
            h263_h_loop_filter(dest_y - 8 * linesize + 8, linesize, qp_tt);

        if (s->mb_x) {
            if (qp_tt || (s->cur->mb_type[xy - 1 - s->mb_stride] & M4_MB_SKIP))
                qp_dt = qp_tt;
            else
                qp_dt = s->cur->qscale_table[xy - 1 - s->mb_stride];
            if (qp_dt) {
                const int cqp = s->chroma_qscale_table[qp_dt];
                h263_h_loop_filter(dest_y  - 8 * linesize,   linesize,   qp_dt);
                h263_h_loop_filter(dest_cb - 8 * uvlinesize, uvlinesize, cqp);
                h263_h_loop_filter(dest_cr - 8 * uvlinesize, uvlinesize, cqp);
            }
        }
    }

    if (qp_c) {
        h263_h_loop_filter(dest_y + 8, linesize, qp_c);
        if (s->mb_y + 1 == s->mb_height)
            h263_h_loop_filter(dest_y + 8 * linesize + 8, linesize, qp_c);
    }

    if (s->mb_x) {
        int qp_lc;
        if (qp_c || (s->cur->mb_type[xy - 1] & M4_MB_SKIP)) qp_lc = qp_c;
        else qp_lc = s->cur->qscale_table[xy - 1];

        if (qp_lc) {
            h263_h_loop_filter(dest_y, linesize, qp_lc);
            if (s->mb_y + 1 == s->mb_height) {
                const int cqp = s->chroma_qscale_table[qp_lc];
                h263_h_loop_filter(dest_y + 8 * linesize, linesize, qp_lc);
                h263_h_loop_filter(dest_cb, uvlinesize, cqp);
                h263_h_loop_filter(dest_cr, uvlinesize, cqp);
            }
        }
    }
}
