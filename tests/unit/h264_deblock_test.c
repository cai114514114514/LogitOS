/* tests/unit/h264_deblock_test.c -- host unit test for h264_deblock.c.
 *
 * Build (WSL):
 *   gcc -O2 -Wall -Wextra -o /tmp/deblock_test \
 *       tests/unit/h264_deblock_test.c c/lib/video/h264_deblock.c -I c/lib/video
 *
 * Contents:
 *   1. Behavioural spot checks of the Table 8-16 alpha/beta and Table 8-17
 *      tc0 thresholds: edges are constructed so that the filter decision or
 *      the clipped delta pins ONE table entry to its exact spec value.
 *   2. Directed coverage of every bS path (0/1/2/3/4), the bS=4 strong
 *      filter 3-tap and small branches, the tc increment rule, the chroma
 *      tc0+1 rule, the chroma bS=4 simplification, disable_idc == 2 slice
 *      edge skipping (vertical and horizontal) and idc == 1 no-op.
 *   3. A randomized differential test against an INDEPENDENT reference
 *      implementation below (written straight from the spec, per sample
 *      line, with its own tables and control flow) over 2x1 / 1x2 / 2x2 /
 *      3x2 / 1x1 / 3x3 synthetic frames with padded strides; padding is
 *      sentinel-filled to catch out-of-bounds writes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "h264_int.h"

static int n_fail;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL %d: ", __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        n_fail++; \
    } \
} while (0)

/* ------------------------------------------------------------ PRNG ------ */
static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ----------------------------------- spec tables (reference copies) ----- */
/* Table 8-16 alpha / beta and Table 8-17 tc0, transcribed here independently
 * for the reference implementation; the directed tests below pin several of
 * these values through observable filter behaviour so a transcription error
 * shared with the module cannot hide. */
static const int RA[52] = {
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     4, 4, 5, 6, 7, 8, 9, 10, 12, 13, 15, 17, 20, 22, 25, 28,
    32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144,
   162, 182, 203, 226, 255, 255
};
static const int RB[52] = {
     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 6, 6, 7, 7, 8, 8,
     9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
    17, 17, 18, 18
};
static const int RTC[52][4] = {
    {0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},
    {0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},
    {0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,1},
    {0,0,0,1},{0,0,0,1},{0,0,0,1},{0,0,1,1},{0,0,1,1},{0,1,1,1},
    {0,1,1,1},{0,1,1,1},{0,1,1,1},{0,1,1,2},{0,1,1,2},{0,1,1,2},
    {0,1,1,2},{0,1,2,3},{0,1,2,3},{0,2,2,3},{0,2,2,4},{0,2,3,4},
    {0,2,3,4},{0,3,3,5},{0,3,4,6},{0,3,4,6},{0,4,5,7},{0,4,5,8},
    {0,4,6,9},{0,5,7,10},{0,6,8,11},{0,6,8,13},{0,7,10,14},{0,8,11,16},
    {0,9,12,18},{0,10,13,20},{0,11,15,23},{0,13,17,25}
};
/* Table 8-15: chroma QP as a function of clipped qPC. */
static const int RCQP[52] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36,
    37, 37, 37, 38, 38, 38, 39, 39, 39, 39
};

static int rclip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
static int rclip1(int v) { return rclip3(0, 255, v); }
static int riabs(int v) { return v < 0 ? -v : v; }
static int r_intra(int t) { return t == MB_I4x4 || t == MB_I16x16 || t == MB_I_PCM; }

/* ------------------------------------------------- coverage counters ---- */
static long cov_luma_bs[5], cov_chroma_bs[5], cov_skip2, cov_strong3tap,
            cov_strongsmall, cov_tcinc, cov_p1q1;

/* ---------------- independent spec reference implementation ------------- */
/* Boundary strength of ONE 4x4 block edge (spec derivation, evaluated per
 * sample line). bp/bq are the 4x4 block indices (raster 0..15) on the p/q
 * side; for internal edges mp == mq and `boundary` is 0. */
static int ref_bs(const mbinfo_t *mp, const mbinfo_t *mq, int boundary,
                  int bp, int bq)
{
    if (boundary) {
        if (r_intra(mp->type) || r_intra(mq->type))
            return 4;
    } else {
        if (r_intra(mp->type))
            return 3;
    }
    /* nz[] is indexed in Z (coding) order while bp/bq are raster, so the two
     * differ for eight of the sixteen blocks and must be converted. */
    {
        static const uint8_t to_z[16] = {
            0, 1, 4, 5,  2, 3, 6, 7,  8, 9, 12, 13,  10, 11, 14, 15
        };
        if (mp->nz[to_z[bp]] > 0 || mq->nz[to_z[bq]] > 0)
            return 2;
    }
    {
        int rp = ((bp >> 2) >> 1) * 2 + ((bp & 3) >> 1);
        int rq = ((bq >> 2) >> 1) * 2 + ((bq & 3) >> 1);
        /* Spec 8.7.2.1 compares the reference PICTURES, and its note says the
         * index position in the list must not be considered -- weighted
         * prediction deliberately puts one picture at several indices. */
        if (mp->ref_pic[rp] != mq->ref_pic[rq])
            return 1;
        if (riabs(mp->mv[bp][0] - mq->mv[bq][0]) >= 4 ||
            riabs(mp->mv[bp][1] - mq->mv[bq][1]) >= 4)
            return 1;
    }
    return 0;
}

/* Normal (bS < 4) and strong (bS == 4) luma filtering of one sample line
 * across an edge. s addresses q0; `as` is the stride across the edge. */
static void ref_luma_line(uint8_t *s, int as, int bs, int qp, int ao, int bo)
{
    int indexA = rclip3(0, 51, qp + ao);
    int indexB = rclip3(0, 51, qp + bo);
    int alpha = RA[indexA], beta = RB[indexB];
    int p0 = s[-as], p1 = s[-2 * as], p2 = s[-3 * as], p3 = s[-4 * as];
    int q0 = s[0],   q1 = s[as],      q2 = s[2 * as],  q3 = s[3 * as];
    int ap = riabs(p2 - p0), aq = riabs(q2 - q0);

    if (alpha == 0 || beta == 0)
        return;
    if (riabs(p0 - q0) >= alpha || riabs(p1 - p0) >= beta ||
        riabs(q1 - q0) >= beta)
        return;

    if (bs < 4) {
        int tc0 = RTC[indexA][bs];
        int tc = tc0, d;
        /* p1 and q1 are only touched for luma and only on small p-side
         * gradients; each also raises tc for the p0/q0 delta */
        if (ap < beta) {
            if (tc0) {
                int t = (p2 + ((p0 + q0 + 1) >> 1) - 2 * p1) >> 1;
                s[-2 * as] = (uint8_t)rclip1(p1 + rclip3(-tc0, tc0, t));
                cov_p1q1++;
            }
            tc++;
            cov_tcinc++;
        }
        if (aq < beta) {
            if (tc0) {
                int t = (q2 + ((p0 + q0 + 1) >> 1) - 2 * q1) >> 1;
                s[as] = (uint8_t)rclip1(q1 + rclip3(-tc0, tc0, t));
            }
            tc++;
        }
        d = rclip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
        s[-as] = (uint8_t)rclip1(p0 + d);
        s[0]   = (uint8_t)rclip1(q0 - d);
    } else {
        if (ap < beta && riabs(p0 - q0) < ((alpha >> 2) + 2)) {
            s[-as]     = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
            s[-2 * as] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
            s[-3 * as] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
            cov_strong3tap++;
        } else {
            s[-as] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            cov_strongsmall++;
        }
        if (aq < beta && riabs(p0 - q0) < ((alpha >> 2) + 2)) {
            s[0]      = (uint8_t)((q2 + 2 * q1 + 2 * q0 + 2 * p0 + p1 + 4) >> 3);
            s[as]     = (uint8_t)((q2 + q1 + q0 + p0 + 2) >> 2);
            s[2 * as] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
        } else {
            s[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

/* Chroma filtering of one sample line (one p/q pair per line). */
static void ref_chroma_line(uint8_t *s, int as, int bs, int qpc, int ao, int bo)
{
    int indexA = rclip3(0, 51, qpc + ao);
    int indexB = rclip3(0, 51, qpc + bo);
    int alpha = RA[indexA], beta = RB[indexB];
    int p0 = s[-as], p1 = s[-2 * as];
    int q0 = s[0],   q1 = s[as];

    if (alpha == 0 || beta == 0)
        return;
    if (riabs(p0 - q0) >= alpha || riabs(p1 - p0) >= beta ||
        riabs(q1 - q0) >= beta)
        return;

    if (bs < 4) {
        int tc = RTC[indexA][bs] + 1;      /* chroma rule: tc = tc0 + 1 */
        int d = rclip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
        s[-as] = (uint8_t)rclip1(p0 + d);
        s[0]   = (uint8_t)rclip1(q0 - d);
    } else {
        /* chroma simplification of the strong filter */
        s[-as] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
        s[0]   = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
    }
}

static int ref_slice_of(const int *sfm, int ns, int addr)
{
    int i, r = 0;
    for (i = 0; i < ns; i++)
        if (sfm[i] <= addr)
            r = i;
    return r;
}

static int ref_chroma_qp(int qp_luma, int off)
{
    return RCQP[rclip3(0, 51, qp_luma + off)];
}

/* Reference frame pass: filters each MB in raster order, vertical edges left
 * to right then horizontal top to bottom; per sample line it derives bS
 * straight from the two adjacent 4x4 blocks. */
static void ref_deblock(uint8_t *y, uint8_t *u, uint8_t *v,
                        int sy, int sc, int mbw, int mbh,
                        const mbinfo_t *mb, const slice_t *sl,
                        const pps_t *pps, const int *sfm, int ns)
{
    int ao = sl->slice_alpha_c0_offset, bo = sl->slice_beta_offset;
    int n_mb = mbw * mbh;
    int cocb = pps->chroma_qp_index_offset;
    int cocr = pps->has_second_chroma_offset ? pps->second_chroma_qp_offset
                                             : pps->chroma_qp_index_offset;
    int skip2 = sl->disable_deblocking_filter_idc == 2 && sfm && ns > 0;

    if (sl->disable_deblocking_filter_idc == 1)
        return;

    for (int mby = 0; mby < mbh; mby++) {
        for (int mbx = 0; mbx < mbw; mbx++) {
            int addr = mby * mbw + mbx;
            const mbinfo_t *mc = mb + addr;
            int qpc = rclip3(0, 51, mc->qp);
            int qcb = ref_chroma_qp(qpc, cocb), qcr = ref_chroma_qp(qpc, cocr);

            /* vertical edges at luma x = 0, 4, 8, 12 */
            for (int e = 0; e < 4; e++) {
                const mbinfo_t *mn = NULL;
                int qp_e, qcb_e, qcr_e, skip = 0;
                if (e == 0) {
                    if (mbx == 0)
                        continue;
                    mn = mc - 1;
                    if (skip2 && ref_slice_of(sfm, ns, addr) !=
                                 ref_slice_of(sfm, ns, addr - 1)) {
                        cov_skip2++;
                        skip = 1;
                    }
                    qp_e = (qpc + rclip3(0, 51, mn->qp) + 1) >> 1;
                    qcb_e = (qcb + ref_chroma_qp(rclip3(0, 51, mn->qp), cocb) + 1) >> 1;
                    qcr_e = (qcr + ref_chroma_qp(rclip3(0, 51, mn->qp), cocr) + 1) >> 1;
                } else {
                    qp_e = qpc; qcb_e = qcb; qcr_e = qcr;
                }
                if (skip)
                    continue;
                for (int l = 0; l < 16; l++) {          /* luma lines */
                    int bs, bp, bq;
                    if (e == 0) { bp = (l >> 2) * 4 + 3; bq = (l >> 2) * 4; }
                    else        { bp = (l >> 2) * 4 + e - 1; bq = (l >> 2) * 4 + e; }
                    bs = ref_bs(e == 0 ? mn : mc, mc, e == 0, bp, bq);
                    cov_luma_bs[bs]++;
                    if (bs)
                        ref_luma_line(y + (size_t)(mby * 16 + l) * sy + mbx * 16 + 4 * e,
                                      1, bs, qp_e, ao, bo);
                }
                if (e == 0 || e == 2) {                 /* chroma lines */
                    for (int l = 0; l < 8; l++) {
                        int lr = (2 * l) >> 2;  /* luma block row at (2x, 2y) */
                        int bs, bp, bq;
                        if (e == 0) { bp = lr * 4 + 3; bq = lr * 4; }
                        else        { bp = lr * 4 + e - 1; bq = lr * 4 + e; }
                        bs = ref_bs(e == 0 ? mn : mc, mc, e == 0, bp, bq);
                        cov_chroma_bs[bs]++;
                        if (!bs)
                            continue;
                        ref_chroma_line(u + (size_t)(mby * 8 + l) * sc + mbx * 8 + 2 * e,
                                        1, bs, qcb_e, ao, bo);
                        ref_chroma_line(v + (size_t)(mby * 8 + l) * sc + mbx * 8 + 2 * e,
                                        1, bs, qcr_e, ao, bo);
                    }
                }
            }

            /* horizontal edges at luma y = 0, 4, 8, 12 */
            for (int e = 0; e < 4; e++) {
                const mbinfo_t *mn = NULL;
                int qp_e, qcb_e, qcr_e, skip = 0;
                if (e == 0) {
                    if (mby == 0)
                        continue;
                    mn = mc - mbw;
                    if (skip2 && ref_slice_of(sfm, ns, addr) !=
                                 ref_slice_of(sfm, ns, addr - mbw)) {
                        cov_skip2++;
                        skip = 1;
                    }
                    qp_e = (qpc + rclip3(0, 51, mn->qp) + 1) >> 1;
                    qcb_e = (qcb + ref_chroma_qp(rclip3(0, 51, mn->qp), cocb) + 1) >> 1;
                    qcr_e = (qcr + ref_chroma_qp(rclip3(0, 51, mn->qp), cocr) + 1) >> 1;
                } else {
                    qp_e = qpc; qcb_e = qcb; qcr_e = qcr;
                }
                if (skip)
                    continue;
                for (int l = 0; l < 16; l++) {
                    int bc = l >> 2;        /* 4x4 block column of this sample */
                    int bs, bp, bq;
                    if (e == 0) { bp = 12 + bc; bq = bc; }
                    else        { bp = (e - 1) * 4 + bc; bq = e * 4 + bc; }
                    bs = ref_bs(e == 0 ? mn : mc, mc, e == 0, bp, bq);
                    if (bs)
                        ref_luma_line(y + (size_t)(mby * 16 + 4 * e) * sy + mbx * 16 + l,
                                      sy, bs, qp_e, ao, bo);
                }
                if (e == 0 || e == 2) {
                    for (int l = 0; l < 8; l++) {
                        int lc = (2 * l) >> 2;
                        int bs, bp, bq;
                        if (e == 0) { bp = 12 + lc; bq = lc; }
                        else        { bp = (e - 1) * 4 + lc; bq = e * 4 + lc; }
                        bs = ref_bs(e == 0 ? mn : mc, mc, e == 0, bp, bq);
                        if (!bs)
                            continue;
                        ref_chroma_line(u + (size_t)(mby * 8 + 2 * e) * sc + mbx * 8 + l,
                                        sc, bs, qcb_e, ao, bo);
                        ref_chroma_line(v + (size_t)(mby * 8 + 2 * e) * sc + mbx * 8 + l,
                                        sc, bs, qcr_e, ao, bo);
                    }
                }
            }
        }
    }
    (void)n_mb;
}

/* ------------------------------------------------------ frame helpers --- */
#define PAD_SENT 0xAB
typedef struct {
    int mbw, mbh, sy, sc;
    size_t ysz, csz;
    uint8_t *y[2], *u[2], *v[2];   /* [0] module, [1] reference */
    mbinfo_t *mb;
    slice_t sl;
    pps_t pps;
    sps_t sps;
} frame_t;

static void frame_alloc(frame_t *f, int mbw, int mbh, int pad_y, int pad_c)
{
    memset(f, 0, sizeof *f);
    f->mbw = mbw; f->mbh = mbh;
    f->sy = mbw * 16 + pad_y;
    f->sc = mbw * 8 + pad_c;
    f->ysz = (size_t)f->sy * (mbh * 16 + 8);
    f->csz = (size_t)f->sc * (mbh * 8 + 8);
    for (int b = 0; b < 2; b++) {
        f->y[b] = malloc(f->ysz);
        f->u[b] = malloc(f->csz);
        f->v[b] = malloc(f->csz);
        if (!f->y[b] || !f->u[b] || !f->v[b]) { fprintf(stderr, "oom\n"); exit(1); }
        memset(f->y[b], PAD_SENT, f->ysz);
        memset(f->u[b], PAD_SENT, f->csz);
        memset(f->v[b], PAD_SENT, f->csz);
    }
    f->mb = calloc((size_t)mbw * mbh, sizeof(mbinfo_t));
    if (!f->mb) { fprintf(stderr, "oom\n"); exit(1); }
    f->sl.disable_deblocking_filter_idc = 0;
}

static void frame_free(frame_t *f)
{
    for (int b = 0; b < 2; b++) { free(f->y[b]); free(f->u[b]); free(f->v[b]); }
    free(f->mb);
}

static void set_y(frame_t *f, int b, int x, int yy, int val)
{
    f->y[b][(size_t)yy * f->sy + x] = (uint8_t)val;
}
static int get_y(frame_t *f, int b, int x, int yy)
{
    return f->y[b][(size_t)yy * f->sy + x];
}
static void set_c(uint8_t *pl, int sc, int x, int yy, int val)
{
    pl[(size_t)yy * sc + x] = (uint8_t)val;
}
static int get_c(uint8_t *pl, int sc, int x, int yy)
{
    return pl[(size_t)yy * sc + x];
}

static void fill_plane_y(frame_t *f, int b, int val)
{
    for (int r = 0; r < f->mbh * 16; r++)
        for (int x = 0; x < f->mbw * 16; x++)
            set_y(f, b, x, r, val);
}
static void fill_plane_c(uint8_t *pl, int sc, int w, int h, int val)
{
    for (int r = 0; r < h; r++)
        for (int x = 0; x < w; x++)
            set_c(pl, sc, x, r, val);
}

/* every padding byte must still be the sentinel */
static int check_sentinel(frame_t *f, int b)
{
    int bad = 0;
    for (int r = 0; r < f->mbh * 16 + 8; r++)
        for (int x = 0; x < f->sy; x++)
            if (r >= f->mbh * 16 || x >= f->mbw * 16)
                if (f->y[b][(size_t)r * f->sy + x] != PAD_SENT) bad++;
    for (int p = 0; p < 2; p++) {
        uint8_t *pl = p ? f->v[b] : f->u[b];
        for (int r = 0; r < f->mbh * 8 + 8; r++)
            for (int x = 0; x < f->sc; x++)
                if (r >= f->mbh * 8 || x >= f->mbw * 8)
                    if (pl[(size_t)r * f->sc + x] != PAD_SENT) bad++;
    }
    return bad == 0;
}

static void run_module(frame_t *f, const int *sfm, int ns)
{
    h264_deblock_frame(f->y[0], f->u[0], f->v[0], f->sy, f->sc, f->mbw, f->mbh,
                       f->mb, &f->sl, &f->sps, &f->pps, sfm, ns);
}

/* set one vertical boundary line (luma x = 15|16) of a 2x1 frame, row r,
 * to the given p3 p2 p1 p0 | q0 q1 q2 q3 */
static void set_vline(frame_t *f, int b, int r,
                      int p3, int p2, int p1, int p0,
                      int q0, int q1, int q2, int q3)
{
    set_y(f, b, 12, r, p3); set_y(f, b, 13, r, p2);
    set_y(f, b, 14, r, p1); set_y(f, b, 15, r, p0);
    set_y(f, b, 16, r, q0); set_y(f, b, 17, r, q1);
    set_y(f, b, 18, r, q2); set_y(f, b, 19, r, q3);
}

/* ------------------------------------------------------- directed tests - */
/* Pin Table 8-16 alpha[26] = 15 and beta[26] = 6 behaviourally: a bS = 4
 * boundary edge (right MB intra) is filtered iff |p0-q0| < alpha and the
 * beta conditions hold. */
static void test_alpha_beta_pins(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    for (int i = 0; i < 2; i++) {
        f.mb[i].type = i ? MB_I4x4 : MB_P_L0;   /* right intra -> bS 4 */
        f.mb[i].qp = 26;
    }
    /* line 0: |p0-q0| = 14 < alpha(15) -> filtered, small branch:
     * p0' = (2*48 + 50 + 66 + 2) >> 2 = 53, q0' = (2*66 + 64 + 48 + 2) >> 2 = 61 */
    set_vline(&f, 0, 0, 46, 46, 48, 50, 64, 66, 68, 70);
    /* line 1: |p0-q0| = 15 = alpha -> untouched */
    set_vline(&f, 0, 1, 46, 46, 48, 50, 65, 66, 68, 70);
    /* line 2: |p1-p0| = 5 < beta(6) -> filtered */
    set_vline(&f, 0, 2, 40, 42, 45, 50, 64, 66, 68, 70);
    /* line 3: |p1-p0| = 6 = beta -> untouched */
    set_vline(&f, 0, 3, 40, 42, 44, 50, 64, 66, 68, 70);
    /* line 4: |q1-q0| = 6 = beta -> untouched */
    set_vline(&f, 0, 4, 46, 46, 48, 50, 64, 70, 72, 74);
    run_module(&f, NULL, 0);
    CHECK(get_y(&f, 0, 15, 0) == 53 && get_y(&f, 0, 16, 0) == 61,
          "alpha pin line0: got %d/%d want 53/61", get_y(&f, 0, 15, 0), get_y(&f, 0, 16, 0));
    CHECK(get_y(&f, 0, 15, 1) == 50 && get_y(&f, 0, 16, 1) == 65,
          "alpha pin line1 (|p0-q0|==alpha must not filter): got %d/%d",
          get_y(&f, 0, 15, 1), get_y(&f, 0, 16, 1));
    CHECK(get_y(&f, 0, 15, 2) != 50, "beta pin line2 (|p1-p0|=5<6 must filter)");
    CHECK(get_y(&f, 0, 15, 3) == 50, "beta pin line3 (|p1-p0|==beta must not filter)");
    CHECK(get_y(&f, 0, 16, 4) == 64, "beta pin line4 (|q1-q0|==beta must not filter)");
    CHECK(check_sentinel(&f, 0), "alpha/beta: padding overwritten");
    frame_free(&f);
}

/* Pin Table 8-17 tc0 entries through the saturated p0 delta of a bS = 2
 * boundary edge (right MB carries nonzero coefficients on its left column). */
static void test_tc0_pins(void)
{
    struct { int qp, tc0, p0, q0, p1, q1, p2, q2, want_p0, want_q0; } cases[4] = {
        /* delta_raw fully saturates tc; ap/aq >= beta so tc stays tc0 */
        { 23, 1, 100, 105,  99, 106,  95, 111, 101, 104 },
        { 33, 2, 100, 110,  99, 111,  90, 121, 102, 108 },
        { 46, 10, 50, 150,  45, 155,  30, 170,  60, 140 },
        { 51, 17, 100, 220, 95, 225,  80, 240, 117, 203 },
    };
    for (int c = 0; c < 4; c++) {
        frame_t f;
        frame_alloc(&f, 2, 1, 8, 8);
        fill_plane_y(&f, 0, 128);
        for (int i = 0; i < 2; i++) {
            f.mb[i].type = MB_P_L0;
            f.mb[i].qp = (int8_t)cases[c].qp;
        }
        f.mb[1].nz[0] = 5;                    /* right MB, left column -> bS 2 */
        set_vline(&f, 0, 0, cases[c].p2, cases[c].p2, cases[c].p1, cases[c].p0,
                  cases[c].q0, cases[c].q1, cases[c].q2, cases[c].q2);
        run_module(&f, NULL, 0);
        CHECK(get_y(&f, 0, 15, 0) == cases[c].want_p0 &&
              get_y(&f, 0, 16, 0) == cases[c].want_q0,
              "tc0[%d][2]=%d pin: got %d/%d want %d/%d",
              cases[c].qp, cases[c].tc0, get_y(&f, 0, 15, 0), get_y(&f, 0, 16, 0),
              cases[c].want_p0, cases[c].want_q0);
        CHECK(check_sentinel(&f, 0), "tc0: padding overwritten");
        frame_free(&f);
    }
    (void)cases;
}

/* bS = 1 path: inter/inter boundary, no coefficients, mv differs by one
 * whole luma pixel (4 quarter-pel) in x. */
static void test_bs1_mv(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    for (int i = 0; i < 2; i++) { f.mb[i].type = MB_P_L0; f.mb[i].qp = 40; }
    /* left MB mv (0,0), right MB mv (4,0) on all blocks -> |dmvx| = 4 */
    for (int k = 0; k < 16; k++) { f.mb[1].mv[k][0] = 4; }
    /* qp 40: alpha 50, beta 11, tc0(40,1) = 4.
     * delta_raw = (80 + (98-122) + 4)>>3 = 7 -> clipped to 4 */
    set_vline(&f, 0, 0, 85, 85, 98, 100, 120, 122, 135, 135);
    run_module(&f, NULL, 0);
    CHECK(get_y(&f, 0, 15, 0) == 104 && get_y(&f, 0, 16, 0) == 116,
          "bS=1: got %d/%d want 104/116", get_y(&f, 0, 15, 0), get_y(&f, 0, 16, 0));
    CHECK(check_sentinel(&f, 0), "bS=1: padding overwritten");
    frame_free(&f);
}

/* bS = 1 also via differing reference PICTURES (mv identical). */
static void test_bs1_ref(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    for (int i = 0; i < 2; i++) { f.mb[i].type = MB_P_L0; f.mb[i].qp = 40; }
    for (int r = 0; r < 4; r++) { f.mb[0].ref_pic[r] = 0; f.mb[1].ref_pic[r] = 1; }
    set_vline(&f, 0, 0, 85, 85, 98, 100, 120, 122, 135, 135);
    run_module(&f, NULL, 0);
    CHECK(get_y(&f, 0, 15, 0) == 104 && get_y(&f, 0, 16, 0) == 116,
          "bS=1(ref): got %d/%d want 104/116", get_y(&f, 0, 15, 0), get_y(&f, 0, 16, 0));
    frame_free(&f);
}

/* The mirror of the above, and the reason ref_pic exists: two DIFFERENT
 * ref_idx values that resolve to the SAME picture must NOT raise bS. This is
 * exactly what weighted prediction produces -- one picture entered into the
 * list several times so each slot can carry its own weights -- and comparing
 * indices instead of pictures filters edges the spec leaves alone. */
static void test_bs0_same_pic_different_idx(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    for (int i = 0; i < 2; i++) { f.mb[i].type = MB_P_L0; f.mb[i].qp = 40; }
    for (int r = 0; r < 4; r++) {
        f.mb[0].ref_idx[r] = 0; f.mb[0].ref_pic[r] = 7;
        f.mb[1].ref_idx[r] = 2; f.mb[1].ref_pic[r] = 7;
    }
    set_vline(&f, 0, 0, 85, 85, 98, 100, 120, 122, 135, 135);
    run_module(&f, NULL, 0);
    CHECK(get_y(&f, 0, 15, 0) == 100 && get_y(&f, 0, 16, 0) == 120,
          "same picture at two indices must not filter: got %d/%d want 100/120",
          get_y(&f, 0, 15, 0), get_y(&f, 0, 16, 0));
    frame_free(&f);
}

/* bS = 0 path: inter/inter, same ref, same mv, no coefficients -> the whole
 * frame must come out bit-identical. */
static void test_bs0_noop(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    for (int r = 0; r < 16; r++)
        for (int x = 0; x < 32; x++)
            set_y(&f, 0, x, r, (x * 7 + r * 13) & 0xFF);
    fill_plane_c(f.u[0], f.sc, 16, 8, 90);
    fill_plane_c(f.v[0], f.sc, 16, 8, 160);
    for (int i = 0; i < 2; i++) { f.mb[i].type = MB_P_L0; f.mb[i].qp = 38; }
    memcpy(f.y[1], f.y[0], f.ysz);
    memcpy(f.u[1], f.u[0], f.csz);
    memcpy(f.v[1], f.v[0], f.csz);
    run_module(&f, NULL, 0);
    CHECK(memcmp(f.y[0], f.y[1], f.ysz) == 0 &&
          memcmp(f.u[0], f.u[1], f.csz) == 0 &&
          memcmp(f.v[0], f.v[1], f.csz) == 0,
          "bS=0: frame modified");
    frame_free(&f);
}

/* bS = 3 path: internal vertical edge (x = 4) of a single intra MB, with
 * the p1/q1 filtering and tc increments exercised (qp 30: alpha 32, beta 8,
 * tc0(30,3) = 2). The edge at x = 4 has p3..p0 at x = 0..3 and q0..q3 at
 * x = 4..7. Hand-computed expectations:
 *   avg = (104+114+1)>>1 = 109
 *   p1' = 102 + clip3(-2,2, ((100+109)>>1) - 102 = 2) = 104   (x = 2)
 *   q1' = 116 + clip3(-2,2, ((118+109)>>1) - 116 = -3) = 114  (x = 5)
 *   tc = 2 + 1 + 1 = 4
 *   delta = clip3(-4,4, (40 + (102-116) + 4)>>3 = 3) = 3
 *   p0' = 107 (x = 3), q0' = 111 (x = 4) */
static void test_bs3_internal(void)
{
    frame_t f;
    frame_alloc(&f, 1, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    f.mb[0].type = MB_I4x4;
    f.mb[0].qp = 30;
    for (int r = 0; r < 16; r++) {
        set_y(&f, 0, 0, r, 96);  set_y(&f, 0, 1, r, 100);
        set_y(&f, 0, 2, r, 102); set_y(&f, 0, 3, r, 104);
        set_y(&f, 0, 4, r, 114); set_y(&f, 0, 5, r, 116);
        set_y(&f, 0, 6, r, 118); set_y(&f, 0, 7, r, 118);
    }
    run_module(&f, NULL, 0);
    CHECK(get_y(&f, 0, 2, 0) == 104, "bS=3 p1': got %d want 104", get_y(&f, 0, 2, 0));
    CHECK(get_y(&f, 0, 3, 0) == 107, "bS=3 p0': got %d want 107", get_y(&f, 0, 3, 0));
    CHECK(get_y(&f, 0, 4, 0) == 111, "bS=3 q0': got %d want 111", get_y(&f, 0, 4, 0));
    CHECK(get_y(&f, 0, 5, 0) == 114, "bS=3 q1': got %d want 114", get_y(&f, 0, 5, 0));
    frame_free(&f);
}

/* bS = 4 strong filter, 3-tap branch on both sides (qp 40: alpha 50,
 * beta 13, (alpha>>2)+2 = 14). The edge is x = 15|16, so p2 is x = 13 and
 * q2 is x = 18. Hand-computed:
 *   p2' = (180+285+98+100+110+4)>>3 = 97     p1' = 405>>2 = 101
 *   p0' = (95+196+200+220+112+4)>>3 = 103
 *   q0' = (115+224+220+200+98+4)>>3 = 107    q1' = 439>>2 = 109
 *   q2' = (240+345+112+110+100+4)>>3 = 113
 * q2 (x = 18) is then filtered AGAIN as p1 of MB1's internal edge at x = 20
 * (bS = 3, intra MB): ap = |109-120| = 11 < beta 13, tc0(40,3) = 5,
 * avg = (120+128+1)>>1 = 124, ((109+124)>>1)-113 = 3 -> x18 = 113+3 = 116. */
static void test_bs4_strong(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    f.mb[0].type = MB_P_L0; f.mb[0].qp = 40;
    f.mb[1].type = MB_I16x16; f.mb[1].qp = 40;
    set_vline(&f, 0, 0, 90, 95, 98, 100, 110, 112, 115, 120);
    run_module(&f, NULL, 0);
    CHECK(get_y(&f, 0, 13, 0) == 97 && get_y(&f, 0, 14, 0) == 101 &&
          get_y(&f, 0, 15, 0) == 103,
          "bS=4 3-tap p side: got %d/%d/%d want 97/101/103",
          get_y(&f, 0, 13, 0), get_y(&f, 0, 14, 0), get_y(&f, 0, 15, 0));
    CHECK(get_y(&f, 0, 16, 0) == 107 && get_y(&f, 0, 17, 0) == 109 &&
          get_y(&f, 0, 18, 0) == 116,
          "bS=4 3-tap q side: got %d/%d/%d want 107/109/116",
          get_y(&f, 0, 16, 0), get_y(&f, 0, 17, 0), get_y(&f, 0, 18, 0));
    CHECK(check_sentinel(&f, 0), "bS=4: padding overwritten");
    frame_free(&f);
}

/* tc increment rule: same bS = 2 setup as the tc0 pin at qp 46 but with
 * ap < beta, which filters p1 AND raises tc from 10 to 11. */
static void test_tc_increment(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    for (int i = 0; i < 2; i++) { f.mb[i].type = MB_P_L0; f.mb[i].qp = 46; }
    f.mb[1].nz[0] = 5;
    /* p2 = 60 -> ap = |60-50| = 10 < beta(16): p1 filtered AND tc incremented */
    set_vline(&f, 0, 0, 60, 60, 45, 50, 150, 155, 170, 170);
    run_module(&f, NULL, 0);
    /* avg = 100; p1' = 45 + clip3(-10,10, ((60+100)>>1) - 45 = 35) = 55
     * tc = 11; delta = clip3(-11,11, 36) = 11 -> p0' = 61, q0' = 139 */
    CHECK(get_y(&f, 0, 14, 0) == 55, "tc++ p1': got %d want 55", get_y(&f, 0, 14, 0));
    CHECK(get_y(&f, 0, 15, 0) == 61 && get_y(&f, 0, 16, 0) == 139,
          "tc++ p0/q0: got %d/%d want 61/139", get_y(&f, 0, 15, 0), get_y(&f, 0, 16, 0));
    frame_free(&f);
}

/* Chroma normal filter at the MB boundary (bS = 2 via nz on the right MB),
 * pinning the chroma tc = tc0 + 1 rule. Both MBs qp 30, offsets 0:
 * qpc = RCQP[30] = 29 per side -> avg 29; alpha(29) = 20, beta(29) = 7,
 * tc = tc0(29,2) + 1 = 2. delta_raw = (24 + (99-107) + 4)>>3 = 2. */
static void test_chroma_normal(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_c(f.u[0], f.sc, 16, 8, 128);
    fill_plane_c(f.v[0], f.sc, 16, 8, 128);
    for (int i = 0; i < 2; i++) { f.mb[i].type = MB_P_L0; f.mb[i].qp = 30; }
    f.mb[1].nz[0] = 3;
    for (int r = 0; r < 8; r++) {
        set_c(f.u[0], f.sc, 6, r, 97);  set_c(f.u[0], f.sc, 7, r, 100);
        set_c(f.u[0], f.sc, 8, r, 106); set_c(f.u[0], f.sc, 9, r, 107);
    }
    run_module(&f, NULL, 0);
    CHECK(get_c(f.u[0], f.sc, 7, 0) == 102 && get_c(f.u[0], f.sc, 8, 0) == 104,
          "chroma bS=2: got %d/%d want 102/104",
          get_c(f.u[0], f.sc, 7, 0), get_c(f.u[0], f.sc, 8, 0));
    frame_free(&f);
}

/* Chroma strong filter at an intra boundary: p0' = (2p1+p0+q1+2)>>2,
 * q0' = (2q1+q0+p1+2)>>2 on every one of the 8 chroma lines. */
static void test_chroma_strong(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_c(f.u[0], f.sc, 16, 8, 128);
    for (int i = 0; i < 2; i++) { f.mb[i].qp = 30; }
    f.mb[0].type = MB_P_L0;
    f.mb[1].type = MB_I4x4;
    for (int r = 0; r < 8; r++) {
        set_c(f.u[0], f.sc, 6, r, 98);  set_c(f.u[0], f.sc, 7, r, 100);
        set_c(f.u[0], f.sc, 8, r, 110); set_c(f.u[0], f.sc, 9, r, 112);
    }
    run_module(&f, NULL, 0);
    for (int r = 0; r < 8; r++)
        CHECK(get_c(f.u[0], f.sc, 7, r) == 102 && get_c(f.u[0], f.sc, 8, r) == 108,
              "chroma bS=4 row %d: got %d/%d want 102/108",
              r, get_c(f.u[0], f.sc, 7, r), get_c(f.u[0], f.sc, 8, r));
    frame_free(&f);
}

/* disable_deblocking_filter_idc == 2: the edge between two slices is left
 * alone while internal edges of the same MBs are still filtered. Vertical
 * (2x1) variant; the right MB is intra so the boundary would be bS = 4. */
static void test_idc2_vertical(void)
{
    frame_t f;
    int sfm[2] = { 0, 1 };
    frame_alloc(&f, 2, 1, 8, 8);
    fill_plane_y(&f, 0, 128);
    f.mb[0].type = MB_P_L0; f.mb[0].qp = 40;
    f.mb[1].type = MB_I4x4; f.mb[1].qp = 40;
    f.sl.disable_deblocking_filter_idc = 2;
    /* boundary line that WOULD be strongly filtered with idc == 0 */
    set_vline(&f, 0, 0, 90, 95, 98, 100, 110, 112, 115, 120);
    /* internal edge of the intra MB at x = 20 (bS = 3), set up on rows 1..15
     * so row 0 keeps the boundary line intact; only x = 13..16 are
     * exclusively touched by the boundary edge, so those columns prove the
     * slice-edge skip */
    for (int r = 1; r < 16; r++) {
        set_y(&f, 0, 17, r, 96);  set_y(&f, 0, 18, r, 98);
        set_y(&f, 0, 19, r, 100); set_y(&f, 0, 20, r, 104);
        set_y(&f, 0, 21, r, 106); set_y(&f, 0, 22, r, 108);
    }
    memcpy(f.y[1], f.y[0], f.ysz);
    run_module(&f, sfm, 2);
    CHECK(get_y(&f, 0, 13, 0) == 95 && get_y(&f, 0, 14, 0) == 98 &&
          get_y(&f, 0, 15, 0) == 100 && get_y(&f, 0, 16, 0) == 110,
          "idc=2: slice boundary filtered");
    CHECK(get_y(&f, 0, 19, 1) != 100, "idc=2: internal edge NOT filtered");
    /* sanity: with idc == 0 the same boundary IS filtered */
    memcpy(f.y[0], f.y[1], f.ysz);
    f.sl.disable_deblocking_filter_idc = 0;
    run_module(&f, sfm, 2);
    CHECK(get_y(&f, 0, 15, 0) != 100, "idc=0 control: boundary not filtered");
    frame_free(&f);
}

/* idc == 2, horizontal (1x2) variant. */
static void test_idc2_horizontal(void)
{
    frame_t f;
    int sfm[2] = { 0, 1 };
    frame_alloc(&f, 1, 2, 8, 8);
    fill_plane_y(&f, 0, 128);
    f.mb[0].type = MB_P_L0; f.mb[0].qp = 40;
    f.mb[1].type = MB_I4x4; f.mb[1].qp = 40;
    f.sl.disable_deblocking_filter_idc = 2;
    /* horizontal boundary line at y = 15|16, column 0 */
    set_y(&f, 0, 0, 12, 90);  set_y(&f, 0, 0, 13, 95);
    set_y(&f, 0, 0, 14, 98);  set_y(&f, 0, 0, 15, 100);
    set_y(&f, 0, 0, 16, 110); set_y(&f, 0, 0, 17, 112);
    set_y(&f, 0, 0, 18, 115); set_y(&f, 0, 0, 19, 120);
    run_module(&f, sfm, 2);
    CHECK(get_y(&f, 0, 0, 15) == 100 && get_y(&f, 0, 0, 16) == 110,
          "idc=2 horizontal: slice boundary filtered");
    memcpy(f.y[1], f.y[0], f.ysz);
    f.sl.disable_deblocking_filter_idc = 0;
    run_module(&f, sfm, 2);
    CHECK(get_y(&f, 0, 0, 15) != 100, "idc=0 horizontal control: not filtered");
    frame_free(&f);
}

/* idc == 1: complete no-op even with heavy edges. */
static void test_idc1_noop(void)
{
    frame_t f;
    frame_alloc(&f, 2, 1, 8, 8);
    for (int r = 0; r < 16; r++)
        for (int x = 0; x < 32; x++)
            set_y(&f, 0, x, r, x < 16 ? 30 : 220);
    f.mb[0].type = MB_P_L0; f.mb[0].qp = 51;
    f.mb[1].type = MB_I4x4; f.mb[1].qp = 51;
    f.sl.disable_deblocking_filter_idc = 1;
    memcpy(f.y[1], f.y[0], f.ysz);
    run_module(&f, NULL, 0);
    CHECK(memcmp(f.y[0], f.y[1], f.ysz) == 0, "idc=1: frame modified");
    frame_free(&f);
}

/* Horizontal strong-filter path with the small-edge branch on the p side
 * only (aq >= beta): q side uses the simplified 1-sample filter while the
 * p side is 3-tap. Hand values checked against the reference instead. */
static void test_bs4_horizontal(void)
{
    frame_t f;
    frame_alloc(&f, 1, 2, 8, 8);
    fill_plane_y(&f, 0, 128);
    f.mb[0].type = MB_P_L0; f.mb[0].qp = 40;
    f.mb[1].type = MB_I16x16; f.mb[1].qp = 40;
    for (int x = 0; x < 16; x++) {
        set_y(&f, 0, x, 12, 90);  set_y(&f, 0, x, 13, 95);
        set_y(&f, 0, x, 14, 98);  set_y(&f, 0, x, 15, 100);
        set_y(&f, 0, x, 16, 110); set_y(&f, 0, x, 17, 112);
        set_y(&f, 0, x, 18, 115); set_y(&f, 0, x, 19, 120);
    }
    memcpy(f.y[1], f.y[0], f.ysz);
    memcpy(f.u[1], f.u[0], f.csz);
    memcpy(f.v[1], f.v[0], f.csz);
    run_module(&f, NULL, 0);
    ref_deblock(f.y[1], f.u[1], f.v[1], f.sy, f.sc, f.mbw, f.mbh,
                f.mb, &f.sl, &f.pps, NULL, 0);
    CHECK(memcmp(f.y[0], f.y[1], f.ysz) == 0, "horizontal bS=4 mismatch vs reference");
    CHECK(get_y(&f, 0, 0, 15) == 103, "horizontal bS=4 p0': got %d want 103",
          get_y(&f, 0, 0, 15));
    frame_free(&f);
}

/* --------------------------------------------- randomized differential -- */
static void random_mb(frame_t *f, int idx, uint32_t flavor)
{
    mbinfo_t *m = &f->mb[idx];
    static const int types[] = { MB_I4x4, MB_I4x4, MB_I16x16, MB_P_L0,
                                 MB_P_L0, MB_P_L0, MB_P_SKIP };
    m->type = (uint8_t)types[rnd() % 7];
    m->qp = (int8_t)(rnd() % 52);
    m->cbp = (uint8_t)rnd();
    for (int k = 0; k < 24; k++) {
        uint32_t r = rnd() % 100;
        m->nz[k] = (uint8_t)(r < 55 ? 0 : 1 + rnd() % 16);
    }
    if (m->type == MB_I16x16)
        m->nz_i16dc = (uint8_t)(rnd() % 17);
    /* Draw the index and the picture INDEPENDENTLY. They are correlated in a
     * real stream but not equal, and drawing them together would let a
     * decoder that compares indices pass this test by accident. */
    for (int r = 0; r < 4; r++) {
        m->ref_idx[r] = (uint8_t)(rnd() % 3);
        m->ref_pic[r] = (uint8_t)(rnd() % 3);
    }
    for (int k = 0; k < 16; k++) {
        m->mv[k][0] = (int16_t)((int)(rnd() % 25) - 12);
        m->mv[k][1] = (int16_t)((int)(rnd() % 25) - 12);
        if ((flavor & 1) && k > 0) {        /* clustered mvs: more bS 0/1 */
            m->mv[k][0] = m->mv[0][0] + (int16_t)(rnd() % 3);
            m->mv[k][1] = m->mv[0][1] + (int16_t)(rnd() % 3);
        }
    }
}

static void random_pixels(frame_t *f, int b)
{
    uint32_t smooth = rnd() % 3;
    for (int i = 0; i < f->mbw * f->mbh; i++) {
        int bx = (i % f->mbw) * 16, by = (i / f->mbw) * 16;
        int base = 20 + (int)(rnd() % 200);
        for (int r = 0; r < 16; r++)
            for (int x = 0; x < 16; x++) {
                int v;
                if (smooth == 0)            /* smooth: edges often in range */
                    v = base + (int)(rnd() % 25) - 12;
                else if (smooth == 1)       /* steps + noise */
                    v = base + (x >= 8 ? 30 : 0) + (int)(rnd() % 9) - 4;
                else                        /* raw noise */
                    v = (int)(rnd() & 0xFF);
                set_y(f, b, bx + x, by + r, rclip3(0, 255, v));
            }
        int cbx = (i % f->mbw) * 8, cby = (i / f->mbw) * 8;
        int cbase = 20 + (int)(rnd() % 200);
        for (int r = 0; r < 8; r++)
            for (int x = 0; x < 8; x++) {
                int v = smooth == 2 ? (int)(rnd() & 0xFF)
                                    : cbase + (int)(rnd() % 25) - 12;
                set_c(f->u[b], f->sc, cbx + x, cby + r, rclip3(0, 255, v));
                set_c(f->v[b], f->sc, cbx + x, cby + r, rclip3(0, 255, v));
            }
    }
}

static void test_differential(int trials)
{
    static const int shapes[6][2] = { {2, 1}, {1, 2}, {2, 2},
                                      {3, 2}, {1, 1}, {3, 3} };
    int sfm_buf[16];
    for (int t = 0; t < trials; t++) {
        frame_t f;
        int sh = (int)(rnd() % 6);
        int mbw = shapes[sh][0], mbh = shapes[sh][1];
        int n_mb = mbw * mbh;
        uint32_t flavor = rnd();
        int ns = 1, idc2 = 0;

        frame_alloc(&f, mbw, mbh, 5, 3);    /* odd paddings on purpose */
        for (int i = 0; i < n_mb; i++)
            random_mb(&f, i, flavor);
        random_pixels(&f, 0);

        f.sl.slice_alpha_c0_offset = ((int)(rnd() % 13) - 6) * 2;
        f.sl.slice_beta_offset = ((int)(rnd() % 13) - 6) * 2;
        f.pps.chroma_qp_index_offset = (int)(rnd() % 25) - 12;
        f.pps.has_second_chroma_offset = (int)(rnd() & 1);
        f.pps.second_chroma_qp_offset = (int)(rnd() % 25) - 12;

        sfm_buf[0] = 0;
        if ((rnd() % 3) == 0 && n_mb > 1) {  /* multi-slice + idc 2 */
            idc2 = 1;
            for (int i = 1; i < n_mb && ns < 16; i++)
                if (rnd() & 1)
                    sfm_buf[ns++] = i;
        }
        f.sl.disable_deblocking_filter_idc = idc2 ? 2 : 0;

        memcpy(f.y[1], f.y[0], f.ysz);
        memcpy(f.u[1], f.u[0], f.csz);
        memcpy(f.v[1], f.v[0], f.csz);

        run_module(&f, sfm_buf, ns);
        ref_deblock(f.y[1], f.u[1], f.v[1], f.sy, f.sc, mbw, mbh,
                    f.mb, &f.sl, &f.pps, sfm_buf, ns);

        if (memcmp(f.y[0], f.y[1], f.ysz) != 0 ||
            memcmp(f.u[0], f.u[1], f.csz) != 0 ||
            memcmp(f.v[0], f.v[1], f.csz) != 0) {
            n_fail++;
            printf("FAIL differential trial %d (shape %dx%d idc2=%d ns=%d)\n",
                   t, mbw, mbh, idc2, ns);
            for (int r = 0; r < mbh * 16; r++)
                for (int x = 0; x < mbw * 16; x++)
                    if (get_y(&f, 0, x, r) != get_y(&f, 1, x, r)) {
                        printf("  first y diff at (%d,%d): mod %d ref %d\n",
                               x, r, get_y(&f, 0, x, r), get_y(&f, 1, x, r));
                        goto dumped;
                    }
            dumped:
            for (int i = 0; i < n_mb; i++) {
                mbinfo_t *m = &f.mb[i];
                printf("  MB%d type=%d qp=%d ref=[%d %d %d %d] nz=",
                       i, m->type, m->qp, m->ref_idx[0], m->ref_idx[1],
                       m->ref_idx[2], m->ref_idx[3]);
                for (int k = 0; k < 16; k++)
                    if (m->nz[k]) printf("%d:%d ", k, m->nz[k]);
                printf(" mv0=(%d,%d) mv5=(%d,%d) mv15=(%d,%d)\n",
                       m->mv[0][0], m->mv[0][1], m->mv[5][0], m->mv[5][1],
                       m->mv[15][0], m->mv[15][1]);
            }
            printf("  sfm =");
            for (int i = 0; i < ns; i++) printf(" %d", sfm_buf[i]);
            printf(" a_off=%d b_off=%d\n", f.sl.slice_alpha_c0_offset,
                   f.sl.slice_beta_offset);
            frame_free(&f);
            return;
        }
        if (!check_sentinel(&f, 0)) {
            n_fail++;
            printf("FAIL differential trial %d: padding overwritten\n", t);
            frame_free(&f);
            return;
        }
        frame_free(&f);
    }
}

int main(void)
{
    test_alpha_beta_pins();
    test_tc0_pins();
    test_bs1_mv();
    test_bs1_ref();
    test_bs0_same_pic_different_idx();
    test_bs0_noop();
    test_bs3_internal();
    test_bs4_strong();
    test_tc_increment();
    test_chroma_normal();
    test_chroma_strong();
    test_idc2_vertical();
    test_idc2_horizontal();
    test_idc1_noop();
    test_bs4_horizontal();

    test_differential(4000);

    printf("coverage: luma bS0..4 = %ld %ld %ld %ld %ld, chroma bS1..4 = %ld %ld %ld %ld, "
           "idc2-skips = %ld, strong3tap = %ld, strongsmall = %ld, tcinc = %ld\n",
           cov_luma_bs[0], cov_luma_bs[1], cov_luma_bs[2], cov_luma_bs[3],
           cov_luma_bs[4], cov_chroma_bs[1], cov_chroma_bs[2], cov_chroma_bs[3],
           cov_chroma_bs[4], cov_skip2, cov_strong3tap, cov_strongsmall, cov_tcinc);
    if (cov_luma_bs[1] == 0 || cov_luma_bs[2] == 0 || cov_luma_bs[3] == 0 ||
        cov_luma_bs[4] == 0 || cov_chroma_bs[1] == 0 || cov_chroma_bs[2] == 0 ||
        cov_chroma_bs[3] == 0 || cov_chroma_bs[4] == 0 || cov_skip2 == 0 ||
        cov_strong3tap == 0 || cov_strongsmall == 0 || cov_tcinc == 0) {
        printf("FAIL: insufficient path coverage\n");
        n_fail++;
    }

    if (n_fail) {
        printf("H264-DEBLOCK-FAIL %d failures\n", n_fail);
        return 1;
    }
    printf("H264-DEBLOCK-OK all tests passed\n");
    return 0;
}
