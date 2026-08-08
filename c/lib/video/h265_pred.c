/* c/lib/video/h265_pred.c -- intra prediction, dequantisation and the inverse
 * transforms (spec 8.4.4.2 and 8.6).
 *
 * All of it is exactly specified integer arithmetic, so all of it is testable
 * on its own: tests/unit/h265_pred_test.c drives these functions directly.
 *
 * The transform matrix is BUILT, not typed out. HEVC's 32-point matrix is
 * nested -- the even rows of the N-point matrix are the rows of the N/2-point
 * one, and column N-1-n is +-column n by row parity -- so the whole 32x32
 * table is determined by four small "odd" blocks (2, 4, 8 and 16 values wide)
 * plus that structure. Typing 1024 numbers by hand and getting one of them
 * wrong is a day of bisecting; deriving them from 340 is not, and the nesting
 * property is checked by the unit test rather than assumed.
 */
#include <string.h>
#include "h265.h"
#include "h265_int.h"

/* ------------------------------------------------------------ transform -- */
static int16_t T32[32][32];      /* T[frequency k][position n] */
static int16_t diag_inv8[64];    /* (y*8+x) -> up-right-diagonal scan index */
static int16_t diag_inv4[16];
static int tables_built;

static const int16_t odd4[2][2] = {
    { 83, 36 },
    { 36, -83 }
};
static const int16_t odd8[4][4] = {
    { 89,  75,  50,  18 },
    { 75, -18, -89, -50 },
    { 50, -89,  18,  75 },
    { 18, -50,  75, -89 }
};
static const int16_t odd16[8][8] = {
    { 90,  87,  80,  70,  57,  43,  25,   9 },
    { 87,  57,   9, -43, -80, -90, -70, -25 },
    { 80,   9, -70, -87, -25,  57,  90,  43 },
    { 70, -43, -87,   9,  90,  25, -80, -57 },
    { 57, -80, -25,  90,  -9, -87,  43,  70 },
    { 43, -90,  57,  25, -87,  70,   9, -80 },
    { 25, -70,  90, -80,  43,   9, -57,  87 },
    {  9, -25,  43, -57,  70, -80,  87, -90 }
};
static const int16_t odd32[16][16] = {
    { 90, 90, 88, 85, 82, 78, 73, 67, 61, 54, 46, 38, 31, 22, 13,  4 },
    { 90, 82, 67, 46, 22, -4,-31,-54,-73,-85,-90,-88,-78,-61,-38,-13 },
    { 88, 67, 31,-13,-54,-82,-90,-78,-46, -4, 38, 73, 90, 85, 61, 22 },
    { 85, 46,-13,-67,-90,-73,-22, 38, 82, 88, 54, -4,-61,-90,-78,-31 },
    { 82, 22,-54,-90,-61, 13, 78, 85, 31,-46,-90,-67,  4, 73, 88, 38 },
    { 78, -4,-82,-73, 13, 85, 67,-22,-88,-61, 31, 90, 54,-38,-90,-46 },
    { 73,-31,-90,-22, 78, 67,-38,-90,-13, 82, 61,-46,-88, -4, 85, 54 },
    { 67,-54,-78, 38, 85,-22,-90,  4, 90, 13,-88,-31, 82, 46,-73,-61 },
    { 61,-73,-46, 82, 31,-88,-13, 90, -4,-90, 22, 85,-38,-78, 54, 67 },
    { 54,-85, -4, 88,-46,-61, 82, 13,-90, 38, 67,-78,-22, 90,-31,-73 },
    { 46,-90, 38, 54,-90, 31, 61,-88, 22, 67,-85, 13, 73,-82,  4, 78 },
    { 38,-88, 73, -4,-67, 90,-46,-31, 85,-78, 13, 61,-90, 54, 22,-82 },
    { 31,-78, 90,-61,  4, 54,-88, 82,-38,-22, 73,-90, 67,-13,-46, 85 },
    { 22,-61, 85,-90, 73,-38, -4, 46,-78, 90,-82, 54,-13,-31, 67,-88 },
    { 13,-38, 61,-78, 88,-90, 85,-73, 54,-31,  4, 22,-46, 67,-82, 90 },
    {  4,-13, 22,-31, 38,-46, 54,-61, 67,-73, 78,-82, 85,-88, 90,-90 }
};

/* The 4x4 DST-VII (trType 1), rows = frequency. */
static const int16_t DST4[4][4] = {
    { 29, 55, 74, 84 },
    { 74, 74,  0,-74 },
    { 84,-29,-74, 55 },
    { 55,-84, 74,-29 }
};

static void build_tables(void)
{
    const int16_t m2[2][2] = { { 64, 64 }, { 64, -64 } };
    int16_t cur[32][32], nxt[32][32];
    memset(cur, 0, sizeof cur);
    for (int k = 0; k < 2; k++)
        for (int n = 0; n < 2; n++) cur[k][n] = m2[k][n];

    int N = 2;
    while (N < 32) {
        int NN = N * 2;
        const int16_t *odd;
        int ostride;
        if (NN == 4)       { odd = &odd4[0][0];  ostride = 2; }
        else if (NN == 8)  { odd = &odd8[0][0];  ostride = 4; }
        else if (NN == 16) { odd = &odd16[0][0]; ostride = 8; }
        else               { odd = &odd32[0][0]; ostride = 16; }
        memset(nxt, 0, sizeof nxt);
        for (int k = 0; k < N; k++)
            for (int n = 0; n < N; n++) nxt[2 * k][n] = cur[k][n];
        for (int k = 0; k < N; k++)
            for (int n = 0; n < N; n++) nxt[2 * k + 1][n] = odd[k * ostride + n];
        for (int k = 0; k < NN; k++)
            for (int n = 0; n < N; n++)
                nxt[k][NN - 1 - n] = (k & 1) ? (int16_t)-nxt[k][n] : nxt[k][n];
        memcpy(cur, nxt, sizeof cur);
        N = NN;
    }
    memcpy(T32, cur, sizeof T32);

    /* up-right diagonal scan inverses, for the scaling-list expansion */
    for (int l = 2; l <= 3; l++) {
        int n = 1 << l, i = 0, x = 0, y = 0;
        int16_t *inv = (l == 2) ? diag_inv4 : diag_inv8;
        while (i < n * n) {
            while (y >= 0) {
                if (x < n && y < n) inv[y * n + x] = (int16_t)i++;
                y--; x++;
            }
            y = x; x = 0;
        }
    }
    tables_built = 1;
}

/* The N-point matrix is the 32-point one sub-sampled in frequency. */
static const int16_t *trow(int N, int k)
{
    return T32[k * (32 / N)];
}

/* ---------------------------------------------------------- scaling ------ */
static const int level_scale[6] = { 40, 45, 51, 57, 64, 72 };

/* m[x][y] of 8.6.3 from a compact scaling list (16 or 64 entries in
 * up-right-diagonal order) plus, for 16x16/32x32, an explicit DC. */
static int scaling_factor(const uint8_t *list, int dc, int log2_size, int x, int y)
{
    if (!list) return 16;
    if (log2_size == 2) return list[diag_inv4[y * 4 + x]];
    if (log2_size == 3) return list[diag_inv8[y * 8 + x]];
    if (x == 0 && y == 0) return dc;
    int sh = log2_size - 3;
    return list[diag_inv8[(y >> sh) * 8 + (x >> sh)]];
}

void h265_dequant(int16_t *coeff, int n, int qp, int log2_size,
                  const uint8_t *scaling, int dc_scale, int bd)
{
    if (!tables_built) build_tables();
    int bd_shift = bd + log2_size - 5;     /* 8.6.3: BitDepth + log2 + 10 - 15 */
    int64_t add = (int64_t)1 << (bd_shift - 1);
    int ls = level_scale[qp % 6];
    int sh = qp / 6;
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            int c = coeff[y * n + x];
            if (!c) continue;
            int m = scaling_factor(scaling, dc_scale, log2_size, x, y);
            /* Multiply rather than shift: a left shift of a negative value is
             * undefined in C, and coefficients are routinely negative. The two
             * are identical on every two's-complement target, which is why
             * this only ever showed up under UBSan and never as a wrong
             * pixel -- and why it is worth fixing before a compiler decides
             * otherwise. */
            int64_t v = (int64_t)c * m * ls * ((int64_t)1 << sh);
            v = (v + add) >> bd_shift;
            if (v < -32768) v = -32768;
            if (v > 32767) v = 32767;
            coeff[y * n + x] = (int16_t)v;
        }
    }
}

/* ----------------------------------------------------- inverse transform -- */
void h265_itransform_add(const int16_t *coeff, int log2_size, int tr_type,
                         uint16_t *dst, int stride, int bd)
{
    if (!tables_built) build_tables();
    int n = 1 << log2_size;
    int32_t g[32 * 32];
    /* 8.6.4.2: bdShift = 20 - BitDepth. Stage 1 is bit-depth independent (it
     * always rounds to 16 bits); only the second stage, which lands on the
     * sample grid, knows how many bits a sample has. */
    int bdshift = 20 - bd;
    int32_t bdround = (int32_t)1 << (bdshift - 1);

    /* stage 1: the vertical (column) transform, rounded to 16 bits */
    for (int x = 0; x < n; x++) {
        for (int i = 0; i < n; i++) {
            int32_t s = 0;
            if (tr_type) {
                for (int k = 0; k < 4; k++) s += DST4[k][i] * coeff[k * n + x];
            } else {
                for (int k = 0; k < n; k++) {
                    int16_t c = coeff[k * n + x];
                    if (c) s += trow(n, k)[i] * c;
                }
            }
            s = (s + 64) >> 7;
            g[i * n + x] = h265_clip3(-32768, 32767, s);
        }
    }
    /* stage 2: the horizontal (row) transform, then to residual precision */
    for (int y = 0; y < n; y++) {
        for (int i = 0; i < n; i++) {
            int32_t s = 0;
            if (tr_type) {
                for (int k = 0; k < 4; k++) s += DST4[k][i] * g[y * n + k];
            } else {
                for (int k = 0; k < n; k++) {
                    int32_t c = g[y * n + k];
                    if (c) s += trow(n, k)[i] * c;
                }
            }
            s = (s + bdround) >> bdshift;
            dst[y * stride + i] =
                (uint16_t)h265_clip_pix(dst[y * stride + i] + s, bd);
        }
    }
}

void h265_transform_skip_add(const int16_t *coeff, int log2_size,
                             uint16_t *dst, int stride, int bd)
{
    int n = 1 << log2_size;
    int ts_shift = 5 + log2_size;
    int bdshift = 20 - bd;
    int32_t bdround = (int32_t)1 << (bdshift - 1);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int32_t r = (int32_t)coeff[y * n + x] * (int32_t)(1 << ts_shift);
            r = (r + bdround) >> bdshift;
            dst[y * stride + x] =
                (uint16_t)h265_clip_pix(dst[y * stride + x] + r, bd);
        }
}

void h265_bypass_add(const int16_t *coeff, int log2_size,
                     uint16_t *dst, int stride, int bd)
{
    int n = 1 << log2_size;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            dst[y * stride + x] =
                (uint16_t)h265_clip_pix(dst[y * stride + x] + coeff[y * n + x], bd);
}

/* ======================================================= intra prediction = */
/* Neighbour array layout (nbs = nTbS):
 *   nb[i]           = p[-1][2*nbs-1-i]   for i = 0 .. 2*nbs-1   (left column)
 *   nb[2*nbs]       = p[-1][-1]                                 (corner)
 *   nb[2*nbs+1+i]   = p[i][-1]           for i = 0 .. 2*nbs-1   (top row)
 * so the whole thing is one contiguous run from the bottom-left sample,
 * around the corner, to the top-right one -- which is exactly what the
 * [1,2,1] smoothing filter of 8.4.4.2.3 wants. */
#define NB_LEFT(nb, nbs, y)   ((nb)[2 * (nbs) - 1 - (y)])
#define NB_TOP(nb, nbs, x)    ((nb)[2 * (nbs) + 1 + (x)])
#define NB_CORNER(nb, nbs)    ((nb)[2 * (nbs)])

static const int8_t intra_angle[35] = {
    0, 0, 32, 26, 21, 17, 13, 9, 5, 2, 0, -2, -5, -9, -13, -17, -21, -26,
    -32, -26, -21, -17, -13, -9, -5, -2, 0, 2, 5, 9, 13, 17, 21, 26, 32
};
/* invAngle for the 15 modes with a negative angle (11..25). */
static const int16_t intra_inv_angle[35] = {
    0,0,0,0,0,0,0,0,0,0,0,
    -4096, -1638, -910, -630, -482, -390, -315, -256,
    -315, -390, -482, -630, -910, -1638, -4096,
    0,0,0,0,0,0,0,0,0
};

void h265_intra_filter(uint16_t *nb, int nbs, int mode, int c_idx,
                       int strong_smoothing, int bd)
{
    if (c_idx != 0) return;                      /* 4:2:0: chroma is never filtered */
    if (mode == 1 || nbs == 4) return;           /* DC and 4x4 are never filtered */
    int dist = mode - 26; if (dist < 0) dist = -dist;
    int dist2 = mode - 10; if (dist2 < 0) dist2 = -dist2;
    int mind = dist < dist2 ? dist : dist2;
    int thres = nbs == 8 ? 7 : nbs == 16 ? 1 : 0;
    if (mind <= thres) return;

    int len = 4 * nbs + 1;
    if (strong_smoothing && nbs == 32) {
        int corner = NB_CORNER(nb, nbs);
        int bl = nb[0];                          /* p[-1][63] */
        int tr = nb[len - 1];                    /* p[63][-1] */
        int a = corner + tr - 2 * (int)NB_TOP(nb, nbs, nbs - 1);
        int b = corner + bl - 2 * (int)NB_LEFT(nb, nbs, nbs - 1);
        if (a < 0) a = -a;
        if (b < 0) b = -b;
        /* 8.4.4.2.3: the bi-linear-substitution test is 1 << (BitDepthY - 5),
         * i.e. 8 at 8 bits and 32 at 10. Leaving it at 8 does not corrupt a
         * sample directly -- it just stops choosing the strong filter on
         * gradients that qualify, so a 10-bit stream decodes with the WRONG
         * filter on large flat 32x32 intra blocks and drifts from there. */
        if (a < (1 << (bd - 5)) && b < (1 << (bd - 5))) {
            uint16_t f[4 * 32 + 1];
            f[0] = (uint16_t)bl;
            f[2 * nbs] = (uint16_t)corner;
            f[len - 1] = (uint16_t)tr;
            for (int y = 0; y < 63; y++)
                f[2 * nbs - 1 - y] =
                    (uint16_t)(((63 - y) * corner + (y + 1) * bl + 32) >> 6);
            for (int x = 0; x < 63; x++)
                f[2 * nbs + 1 + x] =
                    (uint16_t)(((63 - x) * corner + (x + 1) * tr + 32) >> 6);
            memcpy(nb, f, (size_t)len * sizeof *f);
            return;
        }
    }
    uint16_t f[4 * 32 + 1];
    f[0] = nb[0];
    f[len - 1] = nb[len - 1];
    for (int i = 1; i < len - 1; i++)
        f[i] = (uint16_t)((nb[i - 1] + 2 * nb[i] + nb[i + 1] + 2) >> 2);
    memcpy(nb, f, (size_t)len * sizeof *f);
}

static void pred_planar(uint16_t *dst, int stride, const uint16_t *nb, int nbs)
{
    int sh = 0;
    while ((1 << sh) < nbs) sh++;
    sh += 1;
    int right = NB_TOP(nb, nbs, nbs);            /* p[nTbS][-1] */
    int below = NB_LEFT(nb, nbs, nbs);           /* p[-1][nTbS] */
    for (int y = 0; y < nbs; y++) {
        int l = NB_LEFT(nb, nbs, y);
        for (int x = 0; x < nbs; x++) {
            int t = NB_TOP(nb, nbs, x);
            int v = (nbs - 1 - x) * l + (x + 1) * right +
                    (nbs - 1 - y) * t + (y + 1) * below + nbs;
            dst[y * stride + x] = (uint16_t)(v >> sh);
        }
    }
}

static void pred_dc(uint16_t *dst, int stride, const uint16_t *nb, int nbs, int c_idx)
{
    int sh = 0;
    while ((1 << sh) < nbs) sh++;
    int sum = nbs;
    for (int i = 0; i < nbs; i++) sum += NB_TOP(nb, nbs, i) + NB_LEFT(nb, nbs, i);
    int dc = sum >> (sh + 1);
    for (int y = 0; y < nbs; y++)
        for (int x = 0; x < nbs; x++) dst[y * stride + x] = (uint16_t)dc;
    if (c_idx == 0 && nbs < 32) {
        dst[0] = (uint16_t)((NB_LEFT(nb, nbs, 0) + 2 * dc + NB_TOP(nb, nbs, 0) + 2) >> 2);
        for (int x = 1; x < nbs; x++)
            dst[x] = (uint16_t)((NB_TOP(nb, nbs, x) + 3 * dc + 2) >> 2);
        for (int y = 1; y < nbs; y++)
            dst[y * stride] = (uint16_t)((NB_LEFT(nb, nbs, y) + 3 * dc + 2) >> 2);
    }
}

static void pred_angular(uint16_t *dst, int stride, const uint16_t *nb,
                         int nbs, int mode, int c_idx, int bd)
{
    int angle = intra_angle[mode];
    int inv = intra_inv_angle[mode];
    /* ref[] is indexed from -nbs to 2*nbs; bias it. */
    int refbuf[3 * 64 + 2];
    int *ref = refbuf + 64;

    if (mode >= 18) {
        for (int x = 0; x <= nbs; x++) ref[x] = NB_TOP(nb, nbs, x - 1);
        if (angle < 0) {
            int lim = (nbs * angle) >> 5;
            if (lim < -1)
                for (int x = -1; x >= lim; x--)
                    ref[x] = NB_LEFT(nb, nbs, ((x * inv + 128) >> 8) - 1);
        } else {
            for (int x = nbs + 1; x <= 2 * nbs; x++) ref[x] = NB_TOP(nb, nbs, x - 1);
        }
        for (int y = 0; y < nbs; y++) {
            int idx = ((y + 1) * angle) >> 5;
            int fact = ((y + 1) * angle) & 31;
            for (int x = 0; x < nbs; x++) {
                int v;
                if (fact)
                    v = ((32 - fact) * ref[x + idx + 1] + fact * ref[x + idx + 2] + 16) >> 5;
                else
                    v = ref[x + idx + 1];
                dst[y * stride + x] = (uint16_t)v;
            }
        }
        if (mode == 26 && c_idx == 0 && nbs < 32) {
            int corner = NB_CORNER(nb, nbs), t0 = NB_TOP(nb, nbs, 0);
            for (int y = 0; y < nbs; y++)
                dst[y * stride] =
                    (uint16_t)h265_clip_pix(t0 + ((NB_LEFT(nb, nbs, y) - corner) >> 1), bd);
        }
    } else {
        for (int x = 0; x <= nbs; x++) ref[x] = NB_LEFT(nb, nbs, x - 1);
        if (angle < 0) {
            int lim = (nbs * angle) >> 5;
            if (lim < -1)
                for (int x = -1; x >= lim; x--)
                    ref[x] = NB_TOP(nb, nbs, ((x * inv + 128) >> 8) - 1);
        } else {
            for (int x = nbs + 1; x <= 2 * nbs; x++) ref[x] = NB_LEFT(nb, nbs, x - 1);
        }
        for (int x = 0; x < nbs; x++) {
            int idx = ((x + 1) * angle) >> 5;
            int fact = ((x + 1) * angle) & 31;
            for (int y = 0; y < nbs; y++) {
                int v;
                if (fact)
                    v = ((32 - fact) * ref[y + idx + 1] + fact * ref[y + idx + 2] + 16) >> 5;
                else
                    v = ref[y + idx + 1];
                dst[y * stride + x] = (uint16_t)v;
            }
        }
        if (mode == 10 && c_idx == 0 && nbs < 32) {
            int corner = NB_CORNER(nb, nbs), l0 = NB_LEFT(nb, nbs, 0);
            for (int x = 0; x < nbs; x++)
                dst[x] = (uint16_t)h265_clip_pix(l0 + ((NB_TOP(nb, nbs, x) - corner) >> 1), bd);
        }
    }
}

void h265_intra_pred(uint16_t *dst, int stride, const uint16_t *nb,
                     int nbs, int mode, int c_idx, int bd)
{
    if (mode == 0)      pred_planar(dst, stride, nb, nbs);
    else if (mode == 1) pred_dc(dst, stride, nb, nbs, c_idx);
    else                pred_angular(dst, stride, nb, nbs, mode, c_idx, bd);
}

/* The unit test reaches in for the built matrix; nothing else does. */
const int16_t *h265_test_transform_matrix(void)
{
    if (!tables_built) build_tables();
    return &T32[0][0];
}
