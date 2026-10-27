/* logit_elevation.h -- the elevation ladder (design_language.md 3.3).
 *
 * Each level is two analytic SHADOW2 layers drawn in shadow ink, key first,
 * then ambient: {dx, dy, sigma, spread, alpha light / alpha dark}. Geometry is
 * in 24.8 logical points (the SYS_GUI_SHADOW2 convention: blur is a 24.8-pt
 * sigma), so the 1.5 pt sigma of E1 is exact. Sigma never exceeds 30 pt.
 *
 * Freestanding, header-only, `static const` tables only. */
#ifndef LOGIT_UI_ELEVATION_H
#define LOGIT_UI_ELEVATION_H

#include <stdint.h>

#define LOGIT_PT24(pt) ((int32_t)((pt) * 256))
/* Half points, so 1.5 is written LOGIT_PT24_HALF(3) without a float. */
#define LOGIT_PT24_HALF(h) ((int32_t)((h) * 128))

/* Shadow ink (3.1.6): L violet-black, D black. */
#define LOGIT_SHADOW_INK_L 0x140F30u
#define LOGIT_SHADOW_INK_D 0x000000u

enum logit_elev {
    LOGIT_E0,      /* flat content: no layer */
    LOGIT_E1,      /* cards (light), Secondary buttons, thumbs, knobs, toolbar capsules */
    LOGIT_E2,      /* hover-lifted cards, tooltips, dragged rows, focused composer */
    LOGIT_E3,      /* menus, popovers, select popups, notifications, HUD, pending bar */
    LOGIT_E4,      /* unfocused window; any window while dragged or resized */
    LOGIT_E5,      /* focused window at rest */
    LOGIT_E6,      /* Spotlight, About, alerts and sheets, the switcher */
    LOGIT_E_DOCK,  /* the Dock */
    LOGIT_ELEV_COUNT
};

struct logit_shadow_layer {
    int32_t dx, dy, sigma_pt, spread;   /* 24.8 pt */
    uint8_t a_light, a_dark;            /* alpha of the shadow ink */
};
struct logit_elevation {
    uint8_t n;                          /* layers in use: 0 (E0) or 2 */
    struct logit_shadow_layer key, ambient;
};

#define LOGIT__SL(dx, dy, s2, sp, al, ad) \
    { LOGIT_PT24(dx), LOGIT_PT24(dy), LOGIT_PT24_HALF(s2), LOGIT_PT24(sp), (al), (ad) }

static const struct logit_elevation LOGIT_ELEVATION[LOGIT_ELEV_COUNT] = {
    [LOGIT_E0]     = { 0, { 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0 } },
    /*                      dx dy 2σ sp  aL  aD           dx dy  2σ  sp  aL  aD */
    [LOGIT_E1]     = { 2, LOGIT__SL(0, 1,  3, 0, 26,  77), LOGIT__SL(0,  1,  8,  0, 15,  51) },
    [LOGIT_E2]     = { 2, LOGIT__SL(0, 2,  6, 0, 31,  87), LOGIT__SL(0,  6, 24,  0, 20,  61) },
    [LOGIT_E3]     = { 2, LOGIT__SL(0, 4, 12, 0, 41, 102), LOGIT__SL(0, 12, 48,  0, 31,  82) },
    [LOGIT_E4]     = { 2, LOGIT__SL(0, 3, 10, 0, 36, 102), LOGIT__SL(0, 10, 40,  0, 26,  77) },
    [LOGIT_E5]     = { 2, LOGIT__SL(0, 8, 24, 0, 56, 128), LOGIT__SL(0, 20, 60, -4, 41, 102) },
    [LOGIT_E6]     = { 2, LOGIT__SL(0,12, 40, 0, 66, 140), LOGIT__SL(0, 28, 60, -6, 46, 115) },
    [LOGIT_E_DOCK] = { 2, LOGIT__SL(0, 6, 20, 0, 41,  90), LOGIT__SL(0,  2,  8,  0, 20,  50) },
};

#define LOGIT_SHADOW_SIGMA_MAX LOGIT_PT24(30)

static inline const struct logit_elevation *logit_elevation(enum logit_elev e)
{
    return (unsigned)e < LOGIT_ELEV_COUNT ? &LOGIT_ELEVATION[e] : &LOGIT_ELEVATION[LOGIT_E0];
}
/* A layer's ink as ARGB for the mode. */
static inline uint32_t logit_shadow_argb(const struct logit_shadow_layer *l, int dark)
{
    return ((uint32_t)(dark ? l->a_dark : l->a_light) << 24)
         | (dark ? LOGIT_SHADOW_INK_D : LOGIT_SHADOW_INK_L);
}
/* How far a level reaches beyond its rect on each side, 24.8 pt, at
 * 3 sigma + |offset| + spread: the damage halo the aui_damage.h API needs. */
static inline int32_t logit_elevation_reach24(enum logit_elev e)
{
    const struct logit_elevation *v = logit_elevation(e);
    int32_t r = 0;
    for (int i = 0; i < v->n; i++) {
        const struct logit_shadow_layer *l = i ? &v->ambient : &v->key;
        int32_t ox = l->dx < 0 ? -l->dx : l->dx, oy = l->dy < 0 ? -l->dy : l->dy;
        int32_t o = ox > oy ? ox : oy;
        int32_t x = 3 * l->sigma_pt + o + l->spread;
        if (x > r) r = x;
    }
    return r;
}

/* Window outline (3.3): L #141428 @ 31 outside, 1 dev; D #FFFFFF @ 26 inside. */
#define LOGIT_WINDOW_OUTLINE_L 0x1F141428u
#define LOGIT_WINDOW_OUTLINE_D 0x1AFFFFFFu
/* Dark rim on opaque raised surfaces (3.2): a 1 dev inner LINEAR stroke,
 * #FFFFFF @ 38 at the top -> @ 10 at 40 % height -> @ 18 at the bottom.
 * Light mode: a 1 dev OUTER stroke of #141428 @ 20 instead. */
#define LOGIT_DARK_RIM_TOP     0x26FFFFFFu
#define LOGIT_DARK_RIM_MID     0x0AFFFFFFu
#define LOGIT_DARK_RIM_MID_POS 40
#define LOGIT_DARK_RIM_BOTTOM  0x12FFFFFFu
#define LOGIT_LIGHT_OUTLINE    0x14141428u

/* Focus change crossfades (3.3), ms. */
#define LOGIT_ELEV_FOCUS_XFADE_MS 150
#define LOGIT_ELEV_DROP_XFADE_MS  140

#endif /* LOGIT_UI_ELEVATION_H */
