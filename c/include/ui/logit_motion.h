/* logit_motion.h -- motion tokens (design_language.md 3.8, prerequisite P6).
 *
 * Springs are {k, c_q8} for ol_spring256_init(s, x24, k, c_q8): k = omega^2
 * (1/s^2) and c_q8 = 2 * zeta * omega * 256, the convention of today's
 * AUI_SPRING_STIFF 900 / DAMP 7680. Timed tokens name a duration and a curve
 * from `enum ol_curve` (c/lib/gfx/include/openlogit_anim.h). The curve values
 * are mirrored here as integers so this header stays freestanding (that
 * header pulls in gfx.h and double-precision easing); ui_tokens_test.c
 * asserts every mirror equals its enumerator. No curve outside the enum is
 * invented: STANDARD and EMPHASIZED are OL_CUBIC_BEZIER with control points.
 *
 * Header-only, `static const` tables only, no float. */
#ifndef LOGIT_UI_MOTION_H
#define LOGIT_UI_MOTION_H

#include <stdint.h>

/* -------------------------------------------------------------- springs */

enum logit_spring {
    LOGIT_SPRING_SNAPPY,   /* ω 38 ζ .86, ~120 ms, 0.4 %: knobs, thumbs, pills, hover pill slide */
    LOGIT_SPRING_SMOOTH,   /* ω 26 ζ 1.0, ~230 ms, 0: popovers, menus, reflow, panels, sheets */
    LOGIT_SPRING_BOUNCY,   /* ω 28 ζ .62, ~300 ms, 8 %: window open, Dock, notifications, pops */
    LOGIT_SPRING_GENTLE,   /* ω 18 ζ .95, ~350 ms, 0: zoom, minimise, Exposé, thumbnail zoom */
    LOGIT_SPRING_TICK,     /* ω 60 ζ .45, ~150 ms, ~20 % of a step: the Clock second hand */
    LOGIT_SPRING_COUNT
};
struct logit_spring_preset {
    uint16_t k;            /* stiffness, omega^2 */
    uint16_t c_q8;         /* damping, 2 zeta omega in Q8 */
    uint16_t settle_ms;    /* design's nominal settle time */
};
static const struct logit_spring_preset LOGIT_SPRINGS[LOGIT_SPRING_COUNT] = {
    [LOGIT_SPRING_SNAPPY] = { 1444, 16732, 120 },
    [LOGIT_SPRING_SMOOTH] = {  676, 13312, 230 },
    [LOGIT_SPRING_BOUNCY] = {  784,  8888, 300 },
    [LOGIT_SPRING_GENTLE] = {  324,  8755, 350 },
    [LOGIT_SPRING_TICK]   = { 3600, 13824, 150 },
};
#define LOGIT_SPRING_DEFAULT LOGIT_SPRING_SNAPPY   /* replaces ω 30 / ζ 0.5 in aui */

/* A spring has settled when |x - target| < 0.25 pt and |v| < 0.5 pt/s; it
 * then snaps to the target and stops requesting frames. 24.8 units. */
#define LOGIT_SETTLE_D24     64     /* 0.25 pt */
#define LOGIT_SETTLE_V24_S  128     /* 0.5 pt per second */
static inline int logit_spring_settled(int32_t d24, int32_t v24_per_s)
{
    return (d24 < 0 ? -d24 : d24) < LOGIT_SETTLE_D24
        && (v24_per_s < 0 ? -v24_per_s : v24_per_s) < LOGIT_SETTLE_V24_S;
}

/* ------------------------------------------------------- timed curves */

/* Mirrors of enum ol_curve (openlogit_anim.h); asserted equal in the test. */
#define LOGIT_CURVE_LINEAR       0   /* OL_LINEAR */
#define LOGIT_CURVE_CUBIC_BEZIER 4   /* OL_CUBIC_BEZIER */
#define LOGIT_CURVE_IN_CUBIC     7   /* OL_EASE_IN_CUBIC */

/* Cubic-bezier control points in 16.16, layout-compatible with struct
 * ol_bezier {x1, y1, x2, y2}. */
struct logit_bezier { int32_t x1, y1, x2, y2; };
#define LOGIT_BEZIER_STANDARD   { 13107, 0, 0, 65536 }        /* (.2, 0, 0, 1) */
#define LOGIT_BEZIER_EMPHASIZED { 3277, 45875, 6554, 65536 }  /* (.05, .7, .1, 1) */

struct logit_timed {
    uint16_t ms;
    uint8_t curve;              /* LOGIT_CURVE_* == enum ol_curve */
    struct logit_bezier bez;    /* read when curve == LOGIT_CURVE_CUBIC_BEZIER */
};
enum logit_timed_token {
    LOGIT_T_HOVER_IN,    /*  90 LINEAR (premultiplied colour tween) */
    LOGIT_T_HOVER_OUT,   /* 140 LINEAR */
    LOGIT_T_FADE_IN,     /* 180 STANDARD: menus, popovers, tooltips, sheets in */
    LOGIT_T_FADE_OUT,    /* 120 IN_CUBIC: everything leaving */
    LOGIT_T_REVEAL,      /* 420 EMPHASIZED: theme reveal, Greeter unlock */
    LOGIT_T_PRESS,       /*  60 down (LINEAR); the release rides `snappy` */
    LOGIT_T_COUNT
};
static const struct logit_timed LOGIT_TIMED[LOGIT_T_COUNT] = {
    [LOGIT_T_HOVER_IN]  = {  90, LOGIT_CURVE_LINEAR,       { 0, 0, 0, 0 } },
    [LOGIT_T_HOVER_OUT] = { 140, LOGIT_CURVE_LINEAR,       { 0, 0, 0, 0 } },
    [LOGIT_T_FADE_IN]   = { 180, LOGIT_CURVE_CUBIC_BEZIER, LOGIT_BEZIER_STANDARD },
    [LOGIT_T_FADE_OUT]  = { 120, LOGIT_CURVE_IN_CUBIC,     { 0, 0, 0, 0 } },
    [LOGIT_T_REVEAL]    = { 420, LOGIT_CURVE_CUBIC_BEZIER, LOGIT_BEZIER_EMPHASIZED },
    [LOGIT_T_PRESS]     = {  60, LOGIT_CURVE_LINEAR,       { 0, 0, 0, 0 } },
};
#define T_HOVER_IN  90
#define T_HOVER_OUT 140
#define T_FADE_IN   180
#define T_FADE_OUT  120
#define T_REVEAL    420
#define T_PRESS     60

/* Press scales in Q8 (3.8): 0.97 on buttons, 0.92 on Dock tiles. */
#define LOGIT_PRESS_SCALE_Q8      248
#define LOGIT_PRESS_SCALE_DOCK_Q8 236

/* Reduce Motion (3.8): springs snap; window open/close become this fade;
 * the theme switch is instant; no Dock magnify or bounce; spinners turn. */
#define LOGIT_RM_FADE_MS 120

#endif /* LOGIT_UI_MOTION_H */
