/* Host test for the integer motion helpers (anim-motion-fixed). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "openlogit_motion.h"

static int fails;
#define CHECK(name, cond)                                         \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s (line %d)\n", name, __LINE__);        \
            fails++;                                              \
        }                                                         \
    } while (0)

/* Analytic x(t)-target for e0 = 1, v0 = 0, w^2 = k, c = 2*zeta*w. */
static double analytic(double k, double c, double t)
{
    double w = sqrt(k), z = c / (2 * w);
    if (fabs(z - 1) < 1e-9)
        return (1 + w * t) * exp(-w * t);
    if (z < 1) {
        double wd = w * sqrt(1 - z * z);
        return exp(-z * w * t) * (cos(wd * t) + z * w / wd * sin(wd * t));
    }
    double s = w * sqrt(z * z - 1), r1 = -z * w + s, r2 = -z * w - s;
    return (r2 * exp(r1 * t) - r1 * exp(r2 * t)) / (r2 - r1);
}

struct cfg { const char *name; unsigned k, dq; };

static void run(const struct cfg *c)
{
    const int start = 0, target = 100 * 256, DT = 500; /* 0.5 ms */
    struct ol_spring256 s;
    ol_spring256_init(&s, start, c->k, c->dq);
    ol_spring256_retarget(&s, target);
    double kk = c->k, cc = c->dq / 256.0, maxerr = 0;
    /* analytic settle time (|e|<1/256 px and |v|<1/256 px/ms) */
    double ta = 0;
    for (double t = 0; t < 60; t += 0.0005) {
        double e = 100 * fabs(analytic(kk, cc, t));
        double v = 100 * fabs((analytic(kk, cc, t + 1e-4) - analytic(kk, cc, t)) / 1e-4) / 1000;
        if (e >= 1 / 256.0 || v >= 1 / 256.0)
            ta = t;
    }
    int moving = 1, steps = 0, prev = s.x;
    double t = 0;
    while (steps < 200000) {
        prev = s.x;
        moving = ol_spring256_step(&s, DT);
        steps++;
        t += DT / 1e6;
        double want = target - 100 * 256.0 * analytic(kk, cc, t);
        if (moving) {
            double err = fabs(s.x - want) / 256.0;
            if (err > maxerr)
                maxerr = err;
        } else
            break;
    }
    printf("%s: analytic settle %.3fs, int settle %.3fs, max err %.4f px\n", c->name, ta, t, maxerr);
    CHECK("terminates", !moving);
    CHECK("error", maxerr <= 0.5);
    CHECK("snap", s.x == target && s.v == 0 && abs(prev - target) <= 2);
    if (c->k <= 25)
        CHECK("slow-spring-analytic>260ms", ta > 0.26);
}

static void retarget(void)
{
    struct ol_spring256 s;
    ol_spring256_init(&s, 0, 400, 9 * 256);
    ol_spring256_retarget(&s, 100 * 256);
    for (int i = 0; i < 100; i++)
        ol_spring256_step(&s, 1000);
    int v = s.v, x = s.x;
    ol_spring256_retarget(&s, -50 * 256);
    CHECK("retarget-keeps-state", s.v == v && s.x == x);
    ol_spring256_step(&s, 500);
    printf("retarget: v %d -> %d\n", v, s.v);
    CHECK("retarget-velocity-continuous", abs(s.v - v) <= abs(v) / 20 + 8 && v > 0);
    for (int i = 0; i < 100000 && ol_spring256_step(&s, 1000); i++)
        ;
    CHECK("retarget-settles", s.x == -50 * 256);
    ol_spring256_init(&s, 77, 400, 1000);
    CHECK("rest-returns-0", ol_spring256_step(&s, 1000) == 0 && s.x == 77);
}

static void colour(void)
{
    uint32_t m = ol_mix_argb_premul(0xFFFF0000u, 0x00000000u, 128);
    int A = m >> 24, R = (m >> 16) & 255, G = (m >> 8) & 255, B = m & 255;
    printf("mix: %08x\n", m);
    CHECK("mix-alpha", abs(A - 128) <= 1);
    CHECK("mix-rgb", abs(R - 255) <= 1 && G <= 1 && B <= 1);
    CHECK("mix-ends", ol_mix_argb_premul(0x12345678u, 0x9abcdef0u, 0) == 0x12345678u &&
                          ol_mix_argb_premul(0x12345678u, 0x9abcdef0u, 256) == 0x9abcdef0u);
    CHECK("mix-opaque", ol_mix_argb_premul(0xFF000000u, 0xFFFFFFFFu, 128) == 0xFF808080u);
}

static void rect(void)
{
    struct gfx_rect24 a = {-1000, 5, 70000, 300}, b = {2000, -77, 12, 99999};
    struct gfx_rect24 r0 = ol_rect_lerp(a, b, 0), r1 = ol_rect_lerp(a, b, 256);
    CHECK("rect-ends", r0.x == a.x && r0.y == a.y && r0.w == a.w && r0.h == a.h && r1.x == b.x &&
                           r1.y == b.y && r1.w == b.w && r1.h == b.h);
    struct gfx_rect24 h = ol_rect_lerp(a, b, 128);
    CHECK("rect-mid", h.x == 500 && h.w == 35006);
}

int main(void)
{
    const struct cfg cs[] = {
        {"under  (w=15 z=0.3)", 225, (unsigned)(2 * 0.3 * 15 * 256)},
        {"under  (w=8 z=0.6)", 64, (unsigned)(2 * 0.6 * 8 * 256)},
        {"critical (w=12)", 144, 2 * 12 * 256},
        {"over   (w=15 z=2)", 225, (unsigned)(2 * 2.0 * 15 * 256)},
        {"soft over (w=5 z=1.5)", 25, (unsigned)(2 * 1.5 * 5 * 256)},
        {"soft over (w=3 z=3)", 9, (unsigned)(2 * 3.0 * 3 * 256)},
    };
    for (unsigned i = 0; i < sizeof cs / sizeof *cs; i++)
        run(&cs[i]);
    retarget();
    colour();
    rect();
    if (fails) {
        printf("%d check(s) failed\n", fails);
        return 1;
    }
    printf("ok\n");
    return 0;
}
