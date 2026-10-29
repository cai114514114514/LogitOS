/* tests/unit/h264_pred_test.c -- host unit tests for h264_pred.c.
 *
 * Every expected value below is hand-computed from ITU-T H.264 (2023):
 * dequant (8.5.10), 4x4 IDCT (8.5.12.1, columns first then rows, r=(g+32)>>6),
 * luma DC 4x4 inverse Hadamard (8.5.11 transform part, unnormalised),
 * chroma DC 2x2 inverse Hadamard (8.5.13 transform part),
 * intra 4x4 (8.3.1), 16x16 (8.3.3), chroma 8x8 (8.3.4).
 *
 * Build: gcc -O2 -Wall -Wextra -o /tmp/t tests/unit/h264_pred_test.c \
 *            c/lib/video/h264_pred.c -I c/lib/video
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "h264_int.h"

/* LevelScale4x4 for the FLAT default scaling list, by qP%6 and raster
 * position -- what h264_nal.c precomputes for a stream that carries no
 * scaling matrix. Passing it here keeps every expectation in this file the
 * one the old hardcoded "coefficient * v << qP/6" arithmetic produced. */
static const int flat_v[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 },
    { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 }
};
static const int *flat_ls(int qp)
{
    static int ls[16];
    int m = ((qp % 6) + 6) % 6;
    for (int r = 0; r < 16; r++) {
        int i = r >> 2, j = r & 3;
        int k = ((i & 1) == 0 && (j & 1) == 0) ? 0 : (((i & 1) && (j & 1)) ? 1 : 2);
        ls[r] = 16 * flat_v[m][k];
    }
    return ls;
}


static int fails, checks;
#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static int eq_block(const uint8_t *got, int stride, const uint8_t *want, int n)
{
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (got[y * stride + x] != want[y * n + x]) return 0;
    return 1;
}

/* ------------------------------------------------------------ dequant+idct */
/* Vector A: DC-only, qp=0.  coef[0]=1 -> d00 = 1*V(0,0,0)=10 (qbits 0).
 * The 1-D inverse kernel applied to (10,0,0,0) yields (10,10,10,10), so
 * g = 10 everywhere and r = (10+32)>>6 = 42>>6 = 0: dst unchanged. */
static void test_idct_dc_qp0(void)
{
    int coef[16] = { 0 };
    uint8_t dst[4 * 4];
    coef[0] = 1;
    memset(dst, 100, sizeof dst);
    h264_dequant_idct_add(coef, 0, flat_ls(0), dst, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == 100, "idct_dc_qp0[%d] = %d want 100", i, dst[i]);
}

/* Vector B: DC-only, qp=51 (boundary).  m=3 -> V(0,0)=14, qbits=8:
 * d00 = 1*14<<8 = 3584.  r = (3584+32)>>6 = 3616>>6 = 56.  pred 40 -> 96. */
static void test_idct_dc_qp51(void)
{
    int coef[16] = { 0 };
    uint8_t dst[4 * 4];
    coef[0] = 1;
    memset(dst, 40, sizeof dst);
    h264_dequant_idct_add(coef, 51, flat_ls(51), dst, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == 96, "idct_dc_qp51[%d] = %d want 96", i, dst[i]);
}

/* Vector C: qp=26 (m=2, qbits=4), DC-only coef[0]=3:
 * d00 = 3*13<<4 = 624, r = (624+32)>>6 = 656>>6 = 10, pred 10 -> 20. */
static void test_idct_dc_qp26(void)
{
    int coef[16] = { 0 };
    uint8_t dst[4 * 4];
    coef[0] = 3;
    memset(dst, 10, sizeof dst);
    h264_dequant_idct_add(coef, 26, flat_ls(26), dst, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == 20, "idct_dc_qp26[%d] = %d want 20", i, dst[i]);
}

/* Vector D (mixed, qp=0): zigzag coef = [10, 10, -11, 20, 0...].
 * zigzag->raster: c(0,0)=10, c(0,1)=10, c(1,0)=-11, c(2,0)=20.
 * qp=0 scale: V(even,even)=10, V(mixed)=13 ->
 * d(0,0)=100, d(0,1)=130, d(1,0)=-143, d(2,0)=200.
 * Column pass, col0: e=(300,-100,(-143>>1)-0=-72,-143)  [arith shift!]
 *   f = (157,-172,-28,443); col1 -> (130,130,130,130); col2/3 = 0.
 * Row pass rows (f_i,130,0,0): e2 = 130>>1 = 65, e3 = 130:
 *   row0: g=(287,222,92,27)      r=(4,3,1,0)
 *   row1: g=(-42,-107,-237,-302) r=(-1,-2,-4,-5)
 *   row2: g=(102,37,-93,-158)    r=(2,1,-1,-2)
 *   row3: g=(573,508,378,313)    r=(9,8,6,5)
 * pred 50 -> dst rows: 54 53 51 50 / 49 48 46 45 / 52 51 49 48 / 59 58 56 55 */
static void test_idct_mixed(void)
{
    int coef[16] = { 0 };
    uint8_t dst[4 * 5];                     /* stride 5: catch stride bugs */
    static const uint8_t want[16] = {
        54, 53, 51, 50,
        49, 48, 46, 45,
        52, 51, 49, 48,
        59, 58, 56, 55
    };
    coef[0] = 10; coef[1] = 10; coef[2] = -11; coef[3] = 20;
    memset(dst, 50, sizeof dst);
    h264_dequant_idct_add(coef, 0, flat_ls(0), dst, 5);
    CHECK(eq_block(dst, 5, want, 4), "idct_mixed block mismatch");
    /* untouched column 4 must still hold the prediction */
    for (int y = 0; y < 4; y++)
        CHECK(dst[y * 5 + 4] == 50, "idct_mixed stride spill at row %d", y);
}

/* Vector E: negative residual clipping.  coef[0] = -100, qp=0: d00 = -1000,
 * r = (-1000+32)>>6 = -968>>6 = -16 (floor), pred 10 -> -6 -> clip 0. */
static void test_idct_clip_low(void)
{
    int coef[16] = { 0 };
    uint8_t dst[4 * 4];
    coef[0] = -100;
    memset(dst, 10, sizeof dst);
    h264_dequant_idct_add(coef, 0, flat_ls(0), dst, 4);
    for (int i = 0; i < 16; i++)
        CHECK(dst[i] == 0, "idct_clip_low[%d] = %d want 0", i, dst[i]);
}

/* ------------------------------------------------------ DC 16 hadamard */
/* identity input -> 4*identity (W*W^T = 4I for the unnormalised kernel) */
static void test_dc16_identity(void)
{
    int dc[16] = { 0 };
    dc[0] = 1; dc[5] = 1; dc[10] = 1; dc[15] = 1;
    h264_dc16_transform(dc);
    for (int i = 0; i < 16; i++) {
        int want = (i == 0 || i == 5 || i == 10 || i == 15) ? 4 : 0;
        CHECK(dc[i] == want, "dc16_identity[%d] = %d want %d", i, dc[i], want);
    }
}

/* single row input [1,2,3,4,0...]: column pass spreads each column
 * uniformly, row pass turns (1,2,3,4) into W*(1,2,3,4) = (10,-4,0,-2);
 * every output row equals that. */
static void test_dc16_row(void)
{
    int dc[16] = { 0 };
    static const int wantrow[4] = { 10, -4, 0, -2 };
    dc[0] = 1; dc[1] = 2; dc[2] = 3; dc[3] = 4;
    h264_dc16_transform(dc);
    for (int i = 0; i < 16; i++)
        CHECK(dc[i] == wantrow[i & 3], "dc16_row[%d] = %d want %d",
              i, dc[i], wantrow[i & 3]);
}

/* impulse at raster (1,1) (index 5): Y[i][j] = W[i][1]*W[j][1], W[.][1] =
 * (1,1,-1,-1) -> rows (1,1,-1,-1),(1,1,-1,-1),(-1,-1,1,1),(-1,-1,1,1) */
static void test_dc16_impulse(void)
{
    int dc[16] = { 0 };
    static const int want[16] = {
        1, 1, -1, -1,
        1, 1, -1, -1,
        -1, -1, 1, 1,
        -1, -1, 1, 1
    };
    dc[5] = 1;
    h264_dc16_transform(dc);
    for (int i = 0; i < 16; i++)
        CHECK(dc[i] == want[i], "dc16_impulse[%d] = %d want %d", i, dc[i], want[i]);
}

/* ------------------------------------------------------ chroma DC 2x2 */
static void test_dcdcm(void)
{
    int dc[4] = { 1, 2, 3, 4 };
    /* f00=1+2+3+4=10, f01=1-2+3-4=-2, f10=1+2-3-4=-4, f11=1-2-3+4=0 */
    static const int want[4] = { 10, -2, -4, 0 };
    h264_dcdcm_transform(dc);
    for (int i = 0; i < 4; i++)
        CHECK(dc[i] == want[i], "dcdcm[%d] = %d want %d", i, dc[i], want[i]);
}

/* ------------------------------------------------------------ intra 4x4 */
/* common neighbours: top=[10,20,30,40], topright=[50,60,70,80],
 * left=[90,100,110,120], topleft=5. */
static const uint8_t T4[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
static const uint8_t L4[4] = { 90, 100, 110, 120 };
#define TL4 5

static void run4(int mode, int al, int at, int atr, int atl,
                 const uint8_t *want, const char *name)
{
    uint8_t dst[4 * 4];
    memset(dst, 0xAA, sizeof dst);
    h264_intra4x4(dst, 4, mode, T4, L4, TL4, al, at, atr, atl);
    CHECK(eq_block(dst, 4, want, 4), "%s mismatch", name);
}

static void test_intra4x4(void)
{
    static const uint8_t v_vert[16] = {   /* mode 0 */
        10, 20, 30, 40, 10, 20, 30, 40, 10, 20, 30, 40, 10, 20, 30, 40
    };
    static const uint8_t v_horz[16] = {   /* mode 1 */
        90, 90, 90, 90, 100, 100, 100, 100,
        110, 110, 110, 110, 120, 120, 120, 120
    };
    static const uint8_t v_dc[16] = {     /* mode 2: (100+420+4)>>3 = 65 */
        65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65, 65
    };
    static const uint8_t v_dc_top[16] = { /* top only: (100+2)>>2 = 25 */
        25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25
    };
    static const uint8_t v_dc_left[16] = {/* left only: (420+2)>>2 = 105 */
        105, 105, 105, 105, 105, 105, 105, 105,
        105, 105, 105, 105, 105, 105, 105, 105
    };
    static const uint8_t v_dc_none[16] = {/* neither: 128 */
        128, 128, 128, 128, 128, 128, 128, 128,
        128, 128, 128, 128, 128, 128, 128, 128
    };
    /* mode 3 down-left with top-right: (t[k]+2t[k+1]+t[k+2]+2)>>2,
     * (3,3) = (t6+3t7+2)>>2 = 78 */
    static const uint8_t v_dl[16] = {
        20, 30, 40, 50,
        30, 40, 50, 60,
        40, 50, 60, 70,
        50, 60, 70, 78
    };
    /* mode 3 without top-right: t[4..7] = t[3] = 40 */
    static const uint8_t v_dl_notr[16] = {
        20, 30, 38, 40,
        30, 38, 40, 40,
        38, 40, 40, 40,
        40, 40, 40, 40
    };
    /* mode 4 down-right (tl=5) */
    static const uint8_t v_dr[16] = {
        28, 11, 20, 30,
        71, 28, 11, 20,
        100, 71, 28, 11,
        110, 100, 71, 28
    };
    /* mode 5 vertical-right */
    static const uint8_t v_vr[16] = {
        8, 15, 25, 35,
        28, 11, 20, 30,
        71, 8, 15, 25,
        100, 28, 11, 20
    };
    /* mode 6 horizontal-down */
    static const uint8_t v_hd[16] = {
        48, 28, 11, 20,
        95, 71, 48, 28,
        105, 100, 95, 71,
        115, 110, 105, 100
    };
    /* mode 7 vertical-left with top-right. 8.3.1.2.8 indexes p[x + (y>>1)],
     * NOT p[x + y]: (3,3) reads p[4],p[5],p[6] = 50,60,70 and gives
     * (50 + 2*60 + 70 + 2) >> 2 = 60. Using x+y would land on p[6],p[7],p[8]
     * and give 78 -- that is the Diagonal_Down_Left index pattern, and this
     * expectation used to carry it. */
    static const uint8_t v_vl[16] = {
        15, 25, 35, 45,
        20, 30, 40, 50,
        25, 35, 45, 55,
        30, 40, 50, 60
    };
    /* mode 7 without top-right: t[4..7]=40 */
    static const uint8_t v_vl_notr[16] = {
        15, 25, 35, 40,
        20, 30, 38, 40,
        25, 35, 40, 40,
        30, 38, 40, 40
    };
    /* mode 8 horizontal-up; z=5 -> (l2+3l3+2)>>2 = 118, z>5 -> l3 = 120 */
    static const uint8_t v_hu[16] = {
        95, 100, 105, 110,
        105, 110, 115, 118,
        115, 118, 120, 120,
        120, 120, 120, 120
    };

    run4(0, 1, 1, 1, 1, v_vert, "4x4 vertical");
    run4(1, 1, 1, 1, 1, v_horz, "4x4 horizontal");
    run4(2, 1, 1, 1, 1, v_dc, "4x4 dc");
    run4(2, 0, 1, 0, 0, v_dc_top, "4x4 dc top-only");
    run4(2, 1, 0, 0, 0, v_dc_left, "4x4 dc left-only");
    run4(2, 0, 0, 0, 0, v_dc_none, "4x4 dc none");
    run4(3, 1, 1, 1, 1, v_dl, "4x4 down-left");
    run4(3, 1, 1, 0, 1, v_dl_notr, "4x4 down-left no-topright");
    run4(4, 1, 1, 1, 1, v_dr, "4x4 down-right");
    run4(5, 1, 1, 1, 1, v_vr, "4x4 vertical-right");
    run4(6, 1, 1, 1, 1, v_hd, "4x4 horizontal-down");
    run4(7, 1, 1, 1, 1, v_vl, "4x4 vertical-left");
    run4(7, 1, 1, 0, 1, v_vl_notr, "4x4 vertical-left no-topright");
    run4(8, 1, 1, 1, 1, v_hu, "4x4 horizontal-up");

    /* illegal mode w.r.t. availability must fall back to DC, not crash:
     * vertical with no top -> DC mean of left = 105 */
    run4(0, 1, 0, 0, 0, v_dc_left, "4x4 illegal-mode fallback");
}

/* ----------------------------------------------------------- intra 16x16 */
/* gradient neighbours: top[i] = 2i, left[y] = 4y, topleft = 0.
 * H = sum (x+1)*(top[8+x]-top[6-x]), x=7 term uses top[15]-tl:
 *   x<7: 4+4x -> 4,16,36,64,100,144,196 ; x=7: 8*30 = 240.  H = 800.
 * b = (5*800+32)>>6 = 4032>>6 = 63.
 * V: y<7: 8+8y -> 8,32,72,128,200,288,392 ; y=7: 8*60 = 480.  V = 1600.
 * c = (5*1600+32)>>6 = 8032>>6 = 125.
 * a = 16*(60+30) = 1440.  pred = clip((1440 + 63(x-7) + 125(y-7) + 16)>>5). */
static void test_intra16x16(void)
{
    uint8_t top[16], left[16], dst[16 * 17];
    for (int i = 0; i < 16; i++) { top[i] = (uint8_t)(2 * i); left[i] = (uint8_t)(4 * i); }

    /* vertical / horizontal */
    memset(dst, 0, sizeof dst);
    h264_intra16x16(dst, 17, 0, top, left, 0, 1, 1);
    int ok = 1;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (dst[y * 17 + x] != top[x]) ok = 0;
    CHECK(ok, "16x16 vertical mismatch");
    h264_intra16x16(dst, 17, 1, top, left, 0, 1, 1);
    ok = 1;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (dst[y * 17 + x] != left[y]) ok = 0;
    CHECK(ok, "16x16 horizontal mismatch");

    /* DC: sum top = 240, sum left = 480 -> (720+16)>>5 = 23 */
    h264_intra16x16(dst, 17, 2, top, left, 0, 1, 1);
    ok = 1;
    for (int i = 0; i < 16 * 16; i++) if (dst[(i / 16) * 17 + i % 16] != 23) ok = 0;
    CHECK(ok, "16x16 dc mismatch");
    /* DC top-only: (240+8)>>4 = 15 */
    h264_intra16x16(dst, 17, 2, top, left, 0, 0, 1);
    CHECK(dst[0] == 15 && dst[15 * 17 + 15] == 15, "16x16 dc top-only");
    /* DC none: 128 */
    h264_intra16x16(dst, 17, 2, top, left, 0, 0, 0);
    CHECK(dst[0] == 128, "16x16 dc none");

    /* plane: hand-computed anchor cells */
    h264_intra16x16(dst, 17, 3, top, left, 0, 1, 1);
    struct { int x, y, v; } anchors[] = {
        { 0, 0, 4 }, { 7, 7, 45 }, { 15, 15, 92 }, { 15, 0, 33 },
        { 0, 15, 62 }, { 8, 8, 51 }
    };
    for (unsigned k = 0; k < sizeof anchors / sizeof anchors[0]; k++)
        CHECK(dst[anchors[k].y * 17 + anchors[k].x] == anchors[k].v,
              "16x16 plane(%d,%d) = %d want %d", anchors[k].x, anchors[k].y,
              dst[anchors[k].y * 17 + anchors[k].x], anchors[k].v);

    /* plane clipping: top[i]=16i (<=240), left[y]=16y, tl=0:
     * H=V: x<7: 32(x+1)^2 -> 32,128,288,512,800,1152,1568; x=7: 8*240=1920.
     * H = 6400, b = (5*6400+32)>>6 = 32032>>6 = 500 = c.  a = 16*480 = 7680.
     * (15,15): (7680+4000+4000+16)>>5 = 15696>>5 = 490 -> clip 255.
     * (0,0): (7680-3500-3500+16)>>5 = 696>>5 = 21. */
    for (int i = 0; i < 16; i++) { top[i] = (uint8_t)(16 * i); left[i] = (uint8_t)(16 * i); }
    h264_intra16x16(dst, 17, 3, top, left, 0, 1, 1);
    CHECK(dst[0] == 21, "16x16 plane clip lo (0,0) = %d want 21", dst[0]);
    CHECK(dst[15 * 17 + 15] == 255, "16x16 plane clip hi = %d want 255",
          dst[15 * 17 + 15]);
    CHECK(dst[15 * 17] == 255, "16x16 plane clip (0,15) = %d want 255",
          dst[15 * 17]);

    /* illegal: plane without top -> DC fallback (left only) */
    for (int i = 0; i < 16; i++) left[i] = (uint8_t)(4 * i);
    h264_intra16x16(dst, 17, 3, top, left, 0, 1, 0);
    /* left-only DC: sum left = 480 -> (480+8)>>4 = 30 */
    CHECK(dst[0] == 30 && dst[15 * 17 + 15] == 30, "16x16 illegal plane fallback");
}

/* ------------------------------------------------------------ intra chroma */
/* top[i] = i (0..7), left[y] = 2y, topleft = 0. */
static void test_intra_chroma(void)
{
    uint8_t top[8], left[8], dst[8 * 9];
    for (int i = 0; i < 8; i++) { top[i] = (uint8_t)i; left[i] = (uint8_t)(2 * i); }

    /* DC quadrants, both edges available. 8.3.4.1 does NOT average both edges
     * everywhere: only the two quadrants on the diagonal do. The other two use
     * the edge they touch ALONE, even when both are available.
     *   TL: (sum t0..3=6 + sum l0..3=12 + 4)>>3 = 22>>3 = 2
     *   TR: top only  -> (sum t4..7=22 + 2)>>2 = 24>>2 = 6
     *   BL: left only -> (sum l4..7=44 + 2)>>2 = 46>>2 = 11
     *   BR: (22 + 44 + 4)>>3 = 70>>3 = 8
     * This expectation used to average both edges in all four, which is what
     * the decoder did too -- the test agreed with the bug. Decoding real
     * streams settled it: with the quadrant rules applied, chroma matches
     * ffmpeg byte for byte over all 60 frames of i-only-160x120. */
    static const uint8_t vdc[64] = {
        2,2,2,2, 6,6,6,6,  2,2,2,2, 6,6,6,6,  2,2,2,2, 6,6,6,6,  2,2,2,2, 6,6,6,6,
        11,11,11,11, 8,8,8,8,  11,11,11,11, 8,8,8,8,
        11,11,11,11, 8,8,8,8,  11,11,11,11, 8,8,8,8
    };
    memset(dst, 0, sizeof dst);
    h264_intra_chroma(dst, 9, 0, top, left, 0, 1, 1);
    CHECK(eq_block(dst, 9, vdc, 8), "chroma dc quadrants mismatch");

    /* DC top-only: TL=BL=(6+2)>>2=2, TR=BR=(22+2)>>2=6 */
    h264_intra_chroma(dst, 9, 0, top, left, 0, 0, 1);
    CHECK(dst[0] == 2 && dst[7] == 6 && dst[4 * 9] == 2 && dst[4 * 9 + 7] == 6,
          "chroma dc top-only");
    /* DC none: 128 */
    h264_intra_chroma(dst, 9, 0, top, left, 0, 0, 0);
    CHECK(dst[0] == 128 && dst[7 * 9 + 7] == 128, "chroma dc none");

    /* horizontal / vertical */
    h264_intra_chroma(dst, 9, 1, top, left, 0, 1, 1);
    int ok = 1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (dst[y * 9 + x] != left[y]) ok = 0;
    CHECK(ok, "chroma horizontal mismatch");
    h264_intra_chroma(dst, 9, 2, top, left, 0, 1, 1);
    ok = 1;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            if (dst[y * 9 + x] != top[x]) ok = 0;
    CHECK(ok, "chroma vertical mismatch");

    /* plane: H: x<3: 2(x+1)^2 -> 2,8,18; x=3: 4*(top[7]-tl)=28. H = 56.
     * b = (17*56+16)>>5 = 968>>5 = 30.
     * V: y<3: 4(y+1)^2 -> 4,16,36; y=3: 4*(left[7]-tl)=56. V = 112.
     * c = (17*112+16)>>5 = 1920>>5 = 60.  a = 16*(14+7) = 336.
     * pred = clip((336 + 30(x-3) + 60(y-3) + 16)>>5). */
    h264_intra_chroma(dst, 9, 3, top, left, 0, 1, 1);
    struct { int x, y, v; } anchors[] = {
        { 0, 0, 2 }, { 3, 3, 11 }, { 7, 7, 22 }, { 7, 0, 9 },
        { 0, 7, 15 }, { 1, 1, 5 }
    };
    for (unsigned k = 0; k < sizeof anchors / sizeof anchors[0]; k++)
        CHECK(dst[anchors[k].y * 9 + anchors[k].x] == anchors[k].v,
              "chroma plane(%d,%d) = %d want %d", anchors[k].x, anchors[k].y,
              dst[anchors[k].y * 9 + anchors[k].x], anchors[k].v);

    /* illegal: vertical without top -> DC fallback */
    h264_intra_chroma(dst, 9, 2, top, left, 0, 1, 0);
    /* left-only quadrants: TL=TR=(12+2)>>2=3, BL=BR=(44+2)>>2=11 */
    CHECK(dst[0] == 3 && dst[7] == 3 && dst[4 * 9] == 11 && dst[4 * 9 + 7] == 11,
          "chroma illegal vertical fallback");
}

/* -------------------------------------------------- robustness (no crash) */
/* adversarial input must never crash nor produce UB: out-of-range qp,
 * extreme coefficient values, extreme hadamard inputs, bogus modes. */
static void test_robust(void)
{
    int coef[16], dc[16], dc4[4];
    uint8_t dst[16 * 17];
    uint8_t nb[16];

    for (int i = 0; i < 16; i++) { coef[i] = (i & 1) ? 0x7FFFFFFF : -0x7FFFFFFF; }
    h264_dequant_idct_add(coef, -5, flat_ls(-5), dst, 17);
    h264_dequant_idct_add(coef, 100, flat_ls(100), dst, 17);
    h264_dequant_idct_add(coef, 51, flat_ls(51), dst, 17);
    for (int i = 0; i < 16; i++) dc[i] = 0x7FFFFFFF - i;
    h264_dc16_transform(dc);
    for (int i = 0; i < 4; i++) dc4[i] = -0x7FFFFFFF;
    h264_dcdcm_transform(dc4);
    memset(nb, 200, sizeof nb);
    for (int m = -3; m < 12; m++) {
        h264_intra4x4(dst, 17, m, nb, nb, 77, 0, 0, 0, 0);
        h264_intra4x4(dst, 17, m, nb, nb, 77, 1, 1, 0, 0);
        h264_intra16x16(dst, 17, m, nb, nb, 77, 0, 0);
        h264_intra_chroma(dst, 17, m, nb, nb, 77, 0, 1);
    }
    CHECK(1, "robustness: survived adversarial calls");
}

int main(void)
{
    test_idct_dc_qp0();
    test_idct_dc_qp51();
    test_idct_dc_qp26();
    test_idct_mixed();
    test_idct_clip_low();
    test_dc16_identity();
    test_dc16_row();
    test_dc16_impulse();
    test_dcdcm();
    test_intra4x4();
    test_intra16x16();
    test_intra_chroma();
    test_robust();
    printf("%s: %d checks, %d failures\n",
           fails ? "H264-PRED-FAIL" : "H264-PRED-OK", checks, fails);
    return fails ? 1 : 0;
}
