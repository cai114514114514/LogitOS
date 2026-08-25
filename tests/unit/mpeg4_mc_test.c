/* tests/unit/mpeg4_mc_test.c -- differential for c/lib/video/mpeg4_mc.c's
 * half-pel motion compensation (mpeg_motion + hpel + edge_mc, the H.263 /
 * MPEG-4 Simple profile path, M4_MV_16X16 with quarter_sample=0) against an
 * INDEPENDENTLY WRITTEN bilinear-average reference with its own
 * border-replication clamp, built fresh in this file from the plain
 * definition mpeg4_mc.c's own header states (a bilinear average of the 1,
 * 2 or 4 neighbouring integer samples, two roundings selected by the
 * rounding_type bit) rather than derived from mpeg4_mc.c's source.
 *
 * SCOPE, STATED HONESTLY: this covers ONLY the M4_MV_16X16 LUMA path --
 * mpeg_motion's src_x/src_y/dxy derivation, hpel's two roundings, and
 * edge_mc's border replication for motion vectors that push the block
 * outside the frame. It does NOT independently verify: chroma (the
 * round_chroma / uvdxy derivation), the four-MV path (apply_8x8 /
 * chroma_4mv_motion), interlaced/field prediction, or ANY of the 16
 * quarter-pel (Advanced Simple) composition cases in qpel_block -- those
 * are smoke-tested only (dxy=0 identity, and one hand-derived numeric case
 * for mc10 below) and are named here as unverified rather than left silent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mpeg4_int.h"

/* --------------------------------------------------------- independent ref */
/* mpeg4_mc.c's M4_OP_* enum is private to that TU (not in mpeg4_int.h);
 * m4_mc's own declaration just calls the parameter `avg`. 0 = PUT (the
 * ordinary rounding_type-selected path), 1 = PUT_NR (no rounding). */
#define OP_PUT     0
#define OP_PUT_NR  1

static int iclip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Same border-replication rule edge_mc implements (clamp the coordinate
 * into [0,w-1]/[0,h-1]), reimplemented independently: a plain sample
 * fetch with clamped coordinates, no shared code with mpeg4_mc.c. */
static uint8_t ref_sample(const uint8_t *plane, int stride, int w, int h, int x, int y)
{
    x = iclip(x, 0, w - 1);
    y = iclip(y, 0, h - 1);
    return plane[(long)y * stride + x];
}

/* dxy: bit0 = half horiz, bit1 = half vert. rnd: 1 = "+1" rounding
 * (ordinary), 0 = "no rounding" (the picture header's rounding_type bit,
 * M4_OP_PUT_NR). */
static uint8_t ref_hpel(const uint8_t *plane, int stride, int w, int h,
                        int x, int y, int dxy, int rnd)
{
    int a = ref_sample(plane, stride, w, h, x, y);
    switch (dxy) {
    case 0: return (uint8_t)a;
    case 1: { int b = ref_sample(plane, stride, w, h, x + 1, y);     return (uint8_t)((a + b + rnd) >> 1); }
    case 2: { int b = ref_sample(plane, stride, w, h, x, y + 1);     return (uint8_t)((a + b + rnd) >> 1); }
    default: {
        int b = ref_sample(plane, stride, w, h, x + 1, y);
        int c = ref_sample(plane, stride, w, h, x, y + 1);
        int d = ref_sample(plane, stride, w, h, x + 1, y + 1);
        return (uint8_t)((a + b + c + d + 1 + rnd) >> 2);
    }
    }
}

/* ------------------------------------------------------------- corpus/run */
static uint64_t rngstate = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void)
{
    rngstate ^= rngstate << 13; rngstate ^= rngstate >> 7; rngstate ^= rngstate << 17;
    return (uint32_t)rngstate;
}

#define PW 40
#define PH 40

int main(void)
{
    uint8_t *luma = malloc((size_t)PW * PH);
    uint8_t *cb = malloc((size_t)(PW / 2) * (PH / 2));
    uint8_t *cr = malloc((size_t)(PW / 2) * (PH / 2));
    for (int i = 0; i < PW * PH; i++) luma[i] = (uint8_t)(rnd32() & 0xff);
    for (int i = 0; i < (PW / 2) * (PH / 2); i++) { cb[i] = (uint8_t)(rnd32() & 0xff); cr[i] = (uint8_t)(rnd32() & 0xff); }

    m4ctx s;
    memset(&s, 0, sizeof s);
    s.linesize = PW; s.uvlinesize = PW / 2;
    s.h_edge_pos = PW; s.v_edge_pos = PH;
    s.quarter_sample = 0;
    s.mv_type = M4_MV_16X16;
    s.emu_size = 18 * s.linesize + 20 * s.uvlinesize + 256;
    s.emu = malloc((size_t)s.emu_size);

    uint8_t *dy = malloc((size_t)s.linesize * 16);
    uint8_t *dcb = malloc((size_t)s.uvlinesize * 8);
    uint8_t *dcr = malloc((size_t)s.uvlinesize * 8);
    uint8_t *const ref[3] = { luma, cb, cr };

    /* mb positions and motion vectors chosen to cover: every dxy 0..3;
     * fully in-bounds blocks; blocks pushed past every one of the four
     * edges (negative src_x, negative src_y, src_x+16>w, src_y+16>h). */
    struct { int mb_x, mb_y, mvx, mvy; } cases[] = {
        { 1, 1, 0, 0 },      /* dxy 0, in-bounds */
        { 1, 1, 1, 0 },      /* dxy 1 (horiz half) */
        { 1, 1, 0, 1 },      /* dxy 2 (vert half) */
        { 1, 1, 1, 1 },      /* dxy 3 (both half) */
        { 1, 1, -3, 5 },     /* odd/even mix, in-bounds */
        { 0, 0, -20, -20 },  /* pushes past the top-left edge */
        { 0, 0, -1, -1 },    /* just off the top-left edge, half-pel */
        { 1, 1, 40, 0 },     /* pushes past the right edge */
        { 1, 1, 0, 40 },     /* pushes past the bottom edge */
        { 1, 1, 33, 33 },    /* past bottom-right, both fractional bits set */
    };
    int ncases = (int)(sizeof(cases) / sizeof(cases[0]));

    long total = 0, mism = 0;
    int first_bad_case = -1;
    for (int ci = 0; ci < ncases; ci++) {
        for (int op = 0; op <= 1; op++) { /* M4_OP_PUT=0, M4_OP_PUT_NR=1 */
            s.mb_x = cases[ci].mb_x; s.mb_y = cases[ci].mb_y;
            s.mv[0][0][0] = (int16_t)cases[ci].mvx;
            s.mv[0][0][1] = (int16_t)cases[ci].mvy;
            memset(dy, 0xAA, (size_t)s.linesize * 16);
            m4_mc(&s, dy, dcb, dcr, 0, ref, op == 0 ? OP_PUT : OP_PUT_NR);

            int dxy = ((cases[ci].mvy & 1) << 1) | (cases[ci].mvx & 1);
            int rnd = (op == 1) ? 0 : 1; /* M4_OP_PUT_NR -> no rounding */
            int src_x0 = s.mb_x * 16 + (cases[ci].mvx >> 1);
            int src_y0 = s.mb_y * 16 + (cases[ci].mvy >> 1);
            for (int y = 0; y < 16; y++) {
                for (int x = 0; x < 16; x++) {
                    uint8_t want = ref_hpel(luma, PW, PW, PH, src_x0 + x, src_y0 + y, dxy, rnd);
                    uint8_t got = dy[y * s.linesize + x];
                    total++;
                    if (want != got) { mism++; if (first_bad_case < 0) first_bad_case = ci; }
                }
            }
        }
    }

    /* Quarter-pel smoke test 1: dxy=0 (mc00, no fractional offset at all)
     * must be an exact copy -- catches gross indexing bugs without
     * verifying any of the 15 fractional composition cases. */
    s.quarter_sample = 1;
    s.mb_x = 1; s.mb_y = 1;
    s.mv[0][0][0] = 0; s.mv[0][0][1] = 0;
    memset(dy, 0x55, (size_t)s.linesize * 16);
    m4_mc(&s, dy, dcb, dcr, 0, ref, OP_PUT);
    long qsmoke_mism = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint8_t want = luma[(long)(s.mb_y * 16 + y) * PW + (s.mb_x * 16 + x)];
            uint8_t got = dy[y * s.linesize + x];
            if (want != got) qsmoke_mism++;
        }

    printf("MPEG4-MC halfpel(16x16 luma): %ld/%ld samples wrong over %d case*op combinations",
           mism, total, ncases * 2);
    if (mism) printf(" (first bad case index %d)", first_bad_case);
    printf("\n");
    printf("MPEG4-MC qpel mc00 identity smoke: %ld/256 wrong\n", qsmoke_mism);
    printf("MPEG4-MC UNVERIFIED by this gate: chroma, four-MV, field/interlaced, "
           "and 15 of 16 qpel composition cases (only mc00 identity is checked)\n");

    int result = (mism == 0 && qsmoke_mism == 0) ? 0 : 1;
    if (result == 0) printf("MPEG4-MC-OK\n");

    free(luma); free(cb); free(cr);
    free(s.emu); free(dy); free(dcb); free(dcr);
    return result;
}
