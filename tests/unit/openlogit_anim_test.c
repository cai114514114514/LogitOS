#include "openlogit_anim.h"
#include <math.h>
#include <stdio.h>
#define CHECK(c, n)                                                                                \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(c)) {                                                                                \
            printf("FAIL %s\n", n);                                                                \
            failed++;                                                                              \
        } else                                                                                     \
            printf("PASS %s\n", n);                                                                \
    } while (0)
int main(void)
{
    int checks = 0, failed = 0;
    struct ol_keyframe keys[3] = {
        {.offset = 0, .value = {0}}, {.offset = .5, .value = {10}}, {.offset = 1, .value = {20}}};
    struct ol_animation_desc d = {sizeof d, OL_SCALAR, 3, OL_FORWARD, 1, 1000000000ull, 0, keys};
    struct ol_timeline a;
    struct ol_anim_sample s;
    CHECK(!ol_animation_init(&a, &d, 0), "timeline initializes");
    ol_animation_sample(&a, 250000000, 0, &s);
    CHECK(fabs(s.value[0] - 5) < .0001 && s.active, "animation has a real intermediate value");
    ol_animation_sample(&a, 1000000000, 0, &s);
    CHECK(s.value[0] == 20 && s.finished && !s.next_ns,
          "finite animation finishes and stops waking");
    ol_animation_pause(&a, 250000000);
    ol_animation_sample(&a, 800000000, 0, &s);
    CHECK(s.value[0] == 5 && !s.active, "pause freezes local time");
    ol_animation_play(&a, 1000000000);
    ol_animation_sample(&a, 1250000000, 0, &s);
    CHECK(s.value[0] == 10, "resume excludes paused elapsed time");
    ol_animation_seek(&a, 1250000000, 750000000);
    ol_animation_rate(&a, 1250000000, -1);
    ol_animation_sample(&a, 1500000000, 0, &s);
    CHECK(s.value[0] == 10 && s.active, "negative rate plays backwards continuously");
    ol_animation_sample(&a, 2000000000, 0, &s);
    CHECK(s.value[0] == 0 && !s.active, "reverse playback stops at beginning");
    ol_animation_init(&a, &d, 0);
    ol_animation_seek(&a, 1000000000, 1000000000);
    ol_animation_rate(&a, 1000000000, -1);
    ol_animation_sample(&a, 1250000000, 0, &s);
    CHECK(s.value[0] == 15 && s.active, "reverse can leave an exact completed endpoint");
    CHECK(ol_ease256(OL_EASE_OUT_CUBIC,128)==224,"window cubic curve preserves existing midpoint");
    d.direction = OL_ALTERNATE;
    d.iterations = 3;
    ol_animation_init(&a, &d, 0);
    ol_animation_sample(&a, 1750000000, 0, &s);
    CHECK(s.value[0] == 5, "alternate cycle reverses keyframes");
    ol_animation_sample(&a, 99000000000ull, 0, &s);
    CHECK(s.value[0] == 20 && s.finished, "missed frames sample final state without catchup loop");
    d.iterations = 0;
    d.delay_ns = 500000000;
    ol_animation_init(&a, &d, 0);
    ol_animation_sample(&a, 250000000, 0, &s);
    CHECK(s.value[0] == 0 && s.active, "delayed animation holds start");
    ol_animation_sample(&a, 1100000000, 1, &s);
    CHECK(s.finished && !s.active, "reduced motion stops endless effect");
    ol_animation_cancel(&a);
    CHECK(ol_animation_sample(&a, 0, 0, &s) == OL_STATE, "cancelled timeline refuses sampling");
    struct ol_easing e = {.kind = OL_CUBIC_BEZIER, .x1 = .25, .y1 = .1, .x2 = .25, .y2 = 1};
    CHECK(fabs(ol_ease(&e, .5f) - .802403) < .0001, "cubic bezier solves x before evaluating y");
    e = (struct ol_easing){.kind = OL_STEPS, .steps = 4};
    CHECK(ol_ease(&e, .49f) == .25f, "step easing respects interval");
    float q0[4] = {0, 0, 0, 1}, q1[4] = {0, 0, 0, -1}, q[4];
    ol_quat_mix(q, q0, q1, .5f);
    CHECK(q[3] == 1, "quaternion antipodes follow shortest arc");
    struct ol_spring spring;
    float v;
    int active;
    ol_spring_init(&spring, 0, 0, 1, 2, 1, 0, 2000000000ull);
    double t = .2, w = 4 * 3.141592653589793;
    CHECK(fabs(ol_spring_sample(&spring, 200000000, &v, &active) -
               (1 - (1 + w * t) * exp(-w * t))) < .0001,
          "critical spring matches independent analytic reference");
    float before = ol_spring_sample(&spring, 200000000, &v, 0), oldv = v;
    ol_spring_retarget(&spring, 200000000, -1);
    float after = ol_spring_sample(&spring, 200000000, &v, 0);
    CHECK(fabs(before - after) < .0001 && fabs(v - oldv) < .0001,
          "spring retarget preserves position and velocity");
    ol_spring_sample(&spring, 2200000000ull, &v, &active);
    CHECK(!active && v == 0, "spring reaches bounded completion");
    CHECK(ol_transition256(0, 255, 90, 180, OL_EASE_OUT, 0) == 191,
          "legacy transition preserves integer pixel interpolation");
    keys[1].offset = -1;
    CHECK(ol_animation_init(&a, &d, 0) == OL_ARGUMENT, "unordered keyframes rejected");
    printf("OpenLogit animation: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
