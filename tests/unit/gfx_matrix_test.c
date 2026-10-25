/* gfx_m_invert / gfx_m_scale_of on SHRINKING matrices.
 *
 * The determinant used to be truncated to 16.16 before it became the divisor.
 * A 1% uniform scale has det = 6.55 in those units, kept as 6, so the inverse
 * was 9% too large (a 4000 px photo drawn at 40 px sampled x = 4367), and below
 * a 1/256 scale the determinant was 0 and a valid matrix was refused. Reference
 * is the closed form in double. -DGFX_M_TRUNC_DET restores the old arithmetic
 * and every assertion below must then FAIL. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "gfx.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    /* Scales are exact in 16.16 so the only error under test is the inverse. */
    static const int s16[] = { 65536, 32768, 6554, 3277, 1311, 655, 328, 262, 131 };
    for (unsigned i = 0; i < sizeof s16 / sizeof *s16; i++) {
        struct gfx_matrix m, inv;
        gfx_m_set(&m, s16[i], 0, 0, s16[i], 0, 0);
        double scale = s16[i] / 65536.0;
        if (!gfx_m_invert(&inv, &m)) { CHECK(0, "scale %.4f refused as singular", scale); continue; }
        int u, v;
        gfx_m_apply(&inv, 40 * 256, 25 * 256, &u, &v);
        double want_u = 40 / scale, want_v = 25 / scale;
        /* 0.1%: the 16.16 quantisation of the inverse itself, not the divisor. */
        CHECK(fabs(u / 256.0 - want_u) <= want_u * 0.001 + 0.05,
              "scale %.4f: x %.2f want %.2f", scale, u / 256.0, want_u);
        CHECK(fabs(v / 256.0 - want_v) <= want_v * 0.001 + 0.05,
              "scale %.4f: y %.2f want %.2f", scale, v / 256.0, want_v);
        double got = gfx_m_scale_of(&m) / 65536.0;
        CHECK(fabs(got - scale) <= scale * 0.001 + 1.0 / 65536,
              "scale_of(%.4f) = %.5f", scale, got);
    }

    /* A flip plus rotation still round-trips at a small scale. */
    struct gfx_matrix m, inv;
    gfx_m_identity(&m);
    gfx_m_translate(&m, 300 * 256, -120 * 256);
    gfx_m_rotate(&m, 30 * 256);
    gfx_m_scale(&m, -655, 655);
    CHECK(gfx_m_invert(&inv, &m), "rotated+flipped 1%% matrix refused");
    int a, b, c, d;
    gfx_m_apply(&m, 800 * 256, 600 * 256, &a, &b);
    gfx_m_apply(&inv, a, b, &c, &d);
    CHECK(abs(c - 800 * 256) <= 2 * 256 && abs(d - 600 * 256) <= 2 * 256,
          "round trip landed at %.2f,%.2f", c / 256.0, d / 256.0);

    /* A matrix whose inverse cannot be expressed is refused, never wrapped. */
    gfx_m_set(&m, 1, 0, 0, 1, 0, 0);
    CHECK(!gfx_m_invert(&inv, &m), "inverse of a 1/65536 scale should be refused");

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
