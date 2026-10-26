#include "openlogit_motion.h"

#define SUBSTEP_US 4000u
#define EDGE_CLAMP ((long long)1 << 23) /* 24.8 units (32768 px) kept in force math */

void ol_spring256_init(struct ol_spring256 *s, int x24, unsigned stiffness, unsigned damping_q8)
{
    s->x = x24;
    s->v = 0;
    s->target = x24;
    s->stiffness = (unsigned short)(stiffness > 65535 ? 65535 : stiffness);
    s->damping_q8 = (unsigned short)(damping_q8 > 65535 ? 65535 : damping_q8);
    s->xf = s->vf = 0;
    s->t_us = 0;
}

void ol_spring256_retarget(struct ol_spring256 *s, int target24)
{
    s->target = target24;
    s->t_us = 0;
}

static long long iabs(long long a) { return a < 0 ? -a : a; }

static int settled(const struct ol_spring256 *s)
{
    /* Compared in 2^-16 units against 1 unit (1/256 px, 1/256 px/ms). */
    long long e = ((long long)(s->x - s->target) << 16) + s->xf;
    long long v = ((long long)s->v << 16) + s->vf;
    return iabs(e) < 65536 && iabs(v) < 65536;
}

static void snap(struct ol_spring256 *s)
{
    s->x = s->target;
    s->v = 0;
    s->xf = s->vf = 0;
}

int ol_spring256_step(struct ol_spring256 *s, unsigned dt_us)
{
    if (settled(s)) {
        snap(s);
        return 0;
    }
    long long X = ((long long)s->x << 16) + s->xf;
    long long V = ((long long)s->v << 16) + s->vf;
    long long T = (long long)s->target << 16;
    while (dt_us) {
        unsigned d = dt_us > SUBSTEP_US ? SUBSTEP_US : dt_us;
        dt_us -= d;
        long long e = X - T;
        if (e > EDGE_CLAMP << 16)
            e = EDGE_CLAMP << 16;
        if (e < -(EDGE_CLAMP << 16))
            e = -(EDGE_CLAMP << 16);
        /* Units: X in 2^-16 of a 24.8 unit, V the same per ms, d in us.
         * dV = -(k*e/1e6 + c*V/1e3) * d/1e3 with c = damping_q8/256 [1/s]. */
        long long spring = (e * (long long)s->stiffness / 1000) * (long long)d / 1000000;
        long long damp = (V * (long long)s->damping_q8 / 256) * (long long)d / 1000000;
        V -= spring + damp;
        X += V * (long long)d / 1000;
        if (s->t_us < 0xFFFFFFFFu - d)
            s->t_us += d;
        else
            s->t_us = 0xFFFFFFFFu;
    }
    s->x = (int)(X >> 16);
    s->xf = (unsigned short)(X & 0xFFFF);
    s->v = (int)(V >> 16);
    s->vf = (unsigned short)(V & 0xFFFF);
#ifdef OL_SPRING_DURATION_CUTOFF
    if (s->t_us >= 260000u) { /* negative control: fixed-duration stop */
        snap(s);
        return 0;
    }
#endif
    if (settled(s)) {
        snap(s);
        return 0;
    }
    return 1;
}

static unsigned premul(unsigned c, unsigned a) { return (c * a + 127) / 255; }

uint32_t ol_mix_argb_premul(uint32_t a, uint32_t b, int t256)
{
    if (t256 <= 0)
        return a;
    if (t256 >= 256)
        return b;
    unsigned u = (unsigned)t256, w = 256 - u;
    unsigned aa = a >> 24, ab = b >> 24;
    unsigned A = (aa * w + ab * u + 128) >> 8;
    if (!A)
        return 0;
    uint32_t out = A << 24;
    for (int sh = 16; sh >= 0; sh -= 8) {
        unsigned pa = premul((a >> sh) & 255, aa), pb = premul((b >> sh) & 255, ab);
        unsigned P = (pa * w + pb * u + 128) >> 8;
        unsigned c = (P * 255 + A / 2) / A;
        if (c > 255)
            c = 255;
        out |= c << sh;
    }
    return out;
}

static int lerp24(int a, int b, int t)
{
    long long d = (long long)b - a;
    return (int)(a + ((d * t + (d < 0 ? 127 : 128)) >> 8)); /* exact at t = 0, 256 */
}

struct gfx_rect24 ol_rect_lerp(struct gfx_rect24 a, struct gfx_rect24 b, int t256)
{
    if (t256 < 0)
        t256 = 0;
    if (t256 > 256)
        t256 = 256;
    struct gfx_rect24 r = {lerp24(a.x, b.x, t256), lerp24(a.y, b.y, t256), lerp24(a.w, b.w, t256),
                           lerp24(a.h, b.h, t256)};
    return r;
}
