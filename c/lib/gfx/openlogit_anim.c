#include "openlogit_anim.h"

static int finite(double x)
{
    return x == x && x <= 1.7976931348623157e308 && x >= -1.7976931348623157e308;
}
static float root(float x)
{
    if (x <= 0)
        return 0;
    float a = x > 1 ? x : 1;
    for (int i = 0; i < 32; i++)
        a = .5f * (a + x / a);
    return a;
}
static float sine(float radians)
{
    return gfx_sin((int)(radians * 14667.719555f)) / 65536.0f;
}
static float cosine(float radians)
{
    return gfx_cos((int)(radians * 14667.719555f)) / 65536.0f;
}
/* Range reduction keeps the Taylor series near zero. This is shared kernel/
 * user code, so importing the host libm would break the freestanding link. */
static float decay(float x)
{
    if (x >= 80)
        return 0;
    if (x <= 0)
        return 1;
    int n = 0;
    while (x > .5f) {
        x *= .5f;
        n++;
    }
    float sum = 1, term = 1;
    for (int i = 1; i <= 12; i++) {
        term *= -x / i;
        sum += term;
    }
    while (n--)
        sum *= sum;
    return sum;
}
static float bez(float a, float b, float t)
{
    float s = 1 - t;
    return 3 * s * s * t * a + 3 * s * t * t * b + t * t * t;
}
float ol_ease(const struct ol_easing *e, float t)
{
    if (t <= 0)
        return e && e->kind == OL_STEPS && e->step_start && e->steps ? 1.0f / e->steps : 0;
    if (t >= 1)
        return 1;
    if (!e)
        return t;
    switch (e->kind) {
    case OL_EASE_OUT:
        return t * (2 - t);
    case OL_EASE_OUT_CUBIC:
        return 1 - (1-t)*(1-t)*(1-t);
    case OL_EASE_IN:
        return t * t;
    case OL_EASE_INOUT:
        return t < .5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
    case OL_CUBIC_BEZIER: {
        float lo = 0, hi = 1;
        for (int i = 0; i < 24; i++) {
            float m = (lo + hi) * .5f;
            if (bez(e->x1, e->x2, m) < t)
                lo = m;
            else
                hi = m;
        }
        return bez(e->y1, e->y2, (lo + hi) * .5f);
    }
    case OL_STEPS:
        if (e->steps) {
            unsigned n = (unsigned)(t * e->steps) + (e->step_start != 0);
            return n > e->steps ? 1 : (float)n / e->steps;
        }
        return t;
    default:
        return t;
    }
}
int ol_ease256(int curve, int t)
{
    if (t < 0)
        t = 0;
    if (t > 256)
        t = 256;
    if (curve == OL_EASE_OUT)
        return gfx_ease_out(t);
    if (curve == OL_EASE_INOUT)
        return gfx_ease_inout(t);
    if (curve == OL_EASE_OUT_CUBIC) {
        int inv = 256 - t;
        return 256 - inv * inv * inv / (256 * 256);
    }
    if (curve == OL_EASE_IN)
        return t * t / 256;
    return t;
}
int ol_transition256(int from, int to, uint64_t elapsed, uint64_t duration, int curve, int reduced)
{
    if (reduced || !duration || elapsed >= duration)
        return to;
    int t = (int)((double)elapsed * 256 / (double)duration);
    return from + (int)((long long)(to - (long long)from) * ol_ease256(curve, t) / 256);
}
static int easing_ok(const struct ol_easing *e)
{
    if (e->kind < OL_LINEAR || e->kind > OL_EASE_OUT_CUBIC)
        return 0;
    if (e->kind == OL_STEPS)
        return e->steps > 0 && e->steps <= 65536;
    return e->kind != OL_CUBIC_BEZIER ||
           (finite(e->x1) && finite(e->x2) && finite(e->y1) && finite(e->y2) && e->x1 >= 0 &&
            e->x1 <= 1 && e->x2 >= 0 && e->x2 <= 1);
}
int ol_animation_init(struct ol_timeline *a, const struct ol_animation_desc *d, uint64_t now)
{
    if (!a || !d || d->size < sizeof *d || !d->keys || d->count < 2 || d->count > 4096 ||
        d->kind < OL_SCALAR || d->kind > OL_QUATERNION || d->direction > OL_ALTERNATE_REVERSE ||
        d->duration_ns > 0x1ffffffffffffful)
        return OL_ARGUMENT;
    if (d->keys[0].offset != 0 || d->keys[d->count - 1].offset != 1)
        return OL_ARGUMENT;
    for (unsigned i = 0; i < d->count; i++) {
        const struct ol_keyframe *k = &d->keys[i];
        if (!finite(k->offset) || k->offset < 0 || k->offset > 1 ||
            (i && k->offset < d->keys[i - 1].offset) || !easing_ok(&k->easing))
            return OL_ARGUMENT;
        for (unsigned j = 0; j < (d->kind == OL_QUATERNION ? 4 : d->kind); j++)
            if (!finite(k->value[j]))
                return OL_ARGUMENT;
        if (d->kind == OL_QUATERNION) {
            float n = 0;
            for (int j = 0; j < 4; j++)
                n += k->value[j] * k->value[j];
            if (!finite(n) || n < 1e-12f)
                return OL_ARGUMENT;
        }
    }
    *a = (struct ol_timeline){.desc = *d, .anchor_ns = now, .rate = 1};
    return OL_OK;
}
static double position(const struct ol_timeline *a, uint64_t now)
{
    double elapsed =
        now >= a->anchor_ns ? (double)(now - a->anchor_ns) : -(double)(a->anchor_ns - now);
    return a->anchor_position + (a->paused ? 0 : elapsed * a->rate);
}
void ol_quat_mix(float out[4], const float a[4], const float b[4], float t)
{
    float dot = 0, n = 0;
    for (int i = 0; i < 4; i++)
        dot += a[i] * b[i];
    for (int i = 0; i < 4; i++) {
        out[i] = a[i] + ((dot < 0 ? -b[i] : b[i]) - a[i]) * t;
        n += out[i] * out[i];
    }
    n = root(n);
    if (n < 1e-12f) {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
        return;
    }
    for (int i = 0; i < 4; i++)
        out[i] /= n;
}
int ol_animation_sample(const struct ol_timeline *a, uint64_t now, int reduced,
                        struct ol_anim_sample *out)
{
    if (!a || !out || !a->desc.keys)
        return OL_ARGUMENT;
    const struct ol_animation_desc *d = &a->desc;
    *out = (struct ol_anim_sample){0};
    if (a->cancelled)
        return OL_STATE;
    double local = position(a, now) - (double)d->delay_ns;
    if (!finite(local) || local > 9e18 || local < -9e18)
        return OL_LIMIT;
    double dur = (double)d->duration_ns, total = dur * d->iterations, p = 0, cycle = 0;
    int end = reduced || !d->duration_ns || (d->iterations && local >= total);
    if (end) {
        cycle = d->iterations ? d->iterations - 1 : 0;
        p = 1;
        out->finished = reduced || !d->duration_ns || a->rate >= 0;
    } else if (local >= 0) {
        cycle = (double)(uint64_t)(local / dur);
        p = (local - cycle * dur) / dur;
    }
    if (d->direction == OL_REVERSE || d->direction == OL_ALTERNATE_REVERSE)
        p = 1 - p;
    if ((d->direction == OL_ALTERNATE || d->direction == OL_ALTERNATE_REVERSE) &&
        ((uint64_t)cycle & 1))
        p = 1 - p;
#ifdef OPENLOGIT_ANIM_FROZEN
    p = 0; /* Same intermediate-frame assertion must fail. */
#endif
    out->progress = p;
    unsigned left = 0;
    while (left + 1 < d->count && d->keys[left + 1].offset <= p)
        left++;
    unsigned right = left + 1 < d->count ? left + 1 : left;
    const struct ol_keyframe *x = &d->keys[left], *y = &d->keys[right];
    float t = y->offset > x->offset ? (float)((p - x->offset) / (y->offset - x->offset)) : 0;
    t = ol_ease(&x->easing, t);
    if (d->kind == OL_QUATERNION)
        ol_quat_mix(out->value, x->value, y->value, t);
    else
        for (unsigned j = 0; j < d->kind; j++)
            out->value[j] = x->value[j] + (y->value[j] - x->value[j]) * t;
    out->active = !out->finished && !a->paused && a->rate != 0;
    if (a->rate < 0 && position(a, now) <= 0) {
        out->active = 0;
        out->finished = 1;
    }
    if (out->active) {
        uint64_t step = 16666667;
        if (local < 0 && a->rate > 0) {
            double wait = -local / a->rate;
            if (wait < step)
                step = (uint64_t)wait;
        }
        out->next_ns = now > ~0ull - step ? ~0ull : now + step;
    }
    return OL_OK;
}
int ol_animation_seek(struct ol_timeline *a, uint64_t now, double pos)
{
    if (!a || !finite(pos) || pos < -9e15 || pos > 9e15)
        return OL_ARGUMENT;
    a->anchor_position = pos;
    a->anchor_ns = now;
    return OL_OK;
}
int ol_animation_pause(struct ol_timeline *a, uint64_t now)
{
    if (!a)
        return OL_ARGUMENT;
    int r = ol_animation_seek(a, now, position(a, now));
    if (!r)
        a->paused = 1;
    return r;
}
int ol_animation_play(struct ol_timeline *a, uint64_t now)
{
    if (!a || a->cancelled)
        return OL_STATE;
    if (a->paused) {
        a->anchor_ns = now;
        a->paused = 0;
    }
    return OL_OK;
}
int ol_animation_rate(struct ol_timeline *a, uint64_t now, double rate)
{
    if (!a || !finite(rate) || rate < -1024 || rate > 1024)
        return OL_ARGUMENT;
    int r = ol_animation_seek(a, now, position(a, now));
    if (!r)
        a->rate = rate;
    return r;
}
void ol_animation_cancel(struct ol_timeline *a)
{
    if (a) {
        a->cancelled = 1;
        a->paused = 1;
    }
}
int ol_spring_init(struct ol_spring *s, float from, float velocity, float target, float hz,
                   float damping, uint64_t now, uint64_t duration)
{
    if (!s || !finite(from) || !finite(velocity) || !finite(target) || !finite(hz) ||
        !finite(damping) || hz <= 0 || hz > 100 || damping <= 0 || damping > 4 || !duration ||
        duration > 60000000000ull)
        return OL_ARGUMENT;
    *s = (struct ol_spring){from, velocity, target, hz * 6.283185307f, damping, now, duration};
    return OL_OK;
}
float ol_spring_sample(const struct ol_spring *s, uint64_t now, float *velocity, int *active)
{
    if (velocity)
        *velocity = 0;
    if (active)
        *active = 0;
    if (!s)
        return 0;
    uint64_t elapsed = now > s->start_ns ? now - s->start_ns : 0;
    if (elapsed >= s->duration_ns)
        return s->target;
    if (active)
        *active = 1;
    float t = (float)((double)elapsed / 1e9), w = s->omega, z = s->damping, x = s->from - s->target,
          v = s->velocity, y, dy;
    if (z > .9999f && z < 1.0001f) {
        float b = v + w * x, e = decay(w * t);
        y = (x + b * t) * e;
        dy = (b - w * (x + b * t)) * e;
    } else if (z < 1) {
        float d = w * root(1 - z * z), b = (v + z * w * x) / d, e = decay(z * w * t),
              sn = sine(d * t), cs = cosine(d * t);
        y = e * (x * cs + b * sn);
        dy = -z * w * y + e * d * (-x * sn + b * cs);
    } else {
        float q = root(z * z - 1), r1 = -w * (z - q), r2 = -w * (z + q),
              a = (v - r2 * x) / (r1 - r2), b = x - a, e1 = decay(-r1 * t), e2 = decay(-r2 * t);
        y = a * e1 + b * e2;
        dy = r1 * a * e1 + r2 * b * e2;
    }
    if (velocity)
        *velocity = dy;
    return s->target + y;
}
int ol_spring_retarget(struct ol_spring *s, uint64_t now, float target)
{
    if (!s || !finite(target))
        return OL_ARGUMENT;
    float v, x = ol_spring_sample(s, now, &v, 0);
    s->from = x;
    s->velocity = v;
    s->target = target;
    s->start_ns = now;
    return OL_OK;
}

