#ifndef OPENLOGIT_ANIM_H
#define OPENLOGIT_ANIM_H
#include "openlogit.h"

/* No clock, allocator or event loop is hidden in this library. All consumers
 * sample the same caller-supplied monotonic instant, including missed frames.
 * The browser keeps cascade/event semantics; AUI keeps widget identity. */
enum ol_curve { OL_LINEAR, OL_EASE_OUT, OL_EASE_INOUT, OL_EASE_IN, OL_CUBIC_BEZIER, OL_STEPS, OL_EASE_OUT_CUBIC };
enum ol_direction { OL_FORWARD, OL_REVERSE, OL_ALTERNATE, OL_ALTERNATE_REVERSE };
enum ol_anim_kind { OL_SCALAR = 1, OL_VEC2 = 2, OL_VEC3 = 3, OL_VEC4 = 4, OL_QUATERNION = 5 };
struct ol_easing {
    int kind;
    float x1, y1, x2, y2;
    unsigned steps;
    int step_start;
};
struct ol_keyframe {
    double offset;
    float value[4];
    struct ol_easing easing;
};
struct ol_animation_desc {
    unsigned size, kind, count, direction, iterations; /* iterations=0: infinite */
    uint64_t duration_ns;
    int64_t delay_ns;
    const struct ol_keyframe *keys; /* immutable, caller-owned until release */
};
struct ol_timeline {
    struct ol_animation_desc desc;
    uint64_t anchor_ns;
    double anchor_position, rate;
    int paused, cancelled;
};
struct ol_anim_sample {
    float value[4];
    double progress;
    int active, finished;
    uint64_t next_ns;
};
float ol_ease(const struct ol_easing *, float progress);
/* Double precision sampling for CSS/WAAPI adapters; raw y can overshoot. */
#include "openlogit_easing.h"
/* Exact 0..256 legacy easing vocabulary, now shared by AUI and WM. */
int ol_ease256(int curve, int progress);
int ol_animation_init(struct ol_timeline *, const struct ol_animation_desc *, uint64_t now);
int ol_animation_sample(const struct ol_timeline *, uint64_t now, int reduced,
                        struct ol_anim_sample *);
int ol_animation_pause(struct ol_timeline *, uint64_t now);
int ol_animation_play(struct ol_timeline *, uint64_t now);
int ol_animation_seek(struct ol_timeline *, uint64_t now, double position_ns);
int ol_animation_rate(struct ol_timeline *, uint64_t now, double rate);
void ol_animation_cancel(struct ol_timeline *);
void ol_quat_mix(float out[4], const float a[4], const float b[4], float t);

/* Analytic damped spring. Re-aiming carries BOTH position and velocity from
 * the current instant. Integration per rendered frame would depend on FPS. */
struct ol_spring {
    float from, velocity, target, omega, damping;
    uint64_t start_ns, duration_ns;
};
int ol_spring_init(struct ol_spring *, float from, float velocity, float target, float frequency_hz,
                   float damping, uint64_t now, uint64_t duration_ns);
float ol_spring_sample(const struct ol_spring *, uint64_t now, float *velocity, int *active);
int ol_spring_retarget(struct ol_spring *, uint64_t now, float target);
/* A pure scalar transition for existing immediate-mode clients. */
int ol_transition256(int from, int to, uint64_t elapsed, uint64_t duration, int curve, int reduced);
#endif
