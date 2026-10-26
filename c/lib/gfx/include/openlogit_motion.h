#ifndef OPENLOGIT_MOTION_H
#define OPENLOGIT_MOTION_H
#include "openlogit.h"

/* Integer motion helpers. All state is caller-owned; no clock, allocator or
 * float is used, so the kernel and ring-3 apps share one implementation.
 *
 * Spring model (units): x, v, target are 24.8 px (v is 24.8 px per ms).
 *   x'' = -stiffness * (x - target) - (damping_q8 / 256) * x'
 * with stiffness = omega^2 in 1/s^2 and damping_q8 = c in 1/s, Q8. Critical
 * damping is damping_q8 = 2 * 256 * sqrt(stiffness). x and v carry 16 extra
 * fractional bits privately (xf, vf) so slow springs do not stall on
 * truncation; treat those and t_us as private. */
struct ol_spring256 {
    int x, v, target;
    unsigned short stiffness, damping_q8;
    unsigned short xf, vf; /* private: sub-unit remainders, 2^-16 */
    unsigned t_us;         /* private: time since init/retarget (saturating) */
};
void ol_spring256_init(struct ol_spring256 *, int x24, unsigned stiffness, unsigned damping_q8);
/* Advances by dt_us in substeps of at most 4 ms. Returns 1 while moving and
 * 0 once |x-target| < 1/256 and |v| < 1/256 px/ms, where x snaps to target
 * and v to 0. */
int ol_spring256_step(struct ol_spring256 *, unsigned dt_us);
/* Keeps position and velocity. */
void ol_spring256_retarget(struct ol_spring256 *, int target24);

/* Straight ARGB in and out; interpolated in premultiplied space, rounded,
 * and unpremultiplied again, so transparent->opaque does not pass through
 * a darkened band. t256 is clamped to 0..256; the endpoints are exact. */
uint32_t ol_mix_argb_premul(uint32_t a, uint32_t b, int t256);

struct gfx_rect24 { int x, y, w, h; };
struct gfx_rect24 ol_rect_lerp(struct gfx_rect24 a, struct gfx_rect24 b, int t256);
#endif
