#ifndef LOGIT_WM_SHADOW_H
#define LOGIT_WM_SHADOW_H
/* THE WM'S DROP-SHADOW PRESETS AND EXTENT RULE. Pure static inline: no fb.c,
 * no gfx, so host tests include it directly.
 *
 * Every WM surface casts two ol_display_shadow_ex layers: a KEY shadow (offset
 * down, small sigma: the contact shadow that says "this is above that") and an
 * AMBIENT one (no offset, larger sigma, low alpha: the soft halo). Values are
 * logical points at 100%; the caller scales by the UI percentage. shadow_ex has
 * no sigma ceiling (the legacy ol_display_shadow clamps the mask tile to 72 px),
 * so the 200% window shadow is the 100% one scaled by 2, not a tightened one.
 *
 * wm_shadow_extent() is the ONE place the footprint is defined: the renderer
 * paints with the presets and every damage box in wm.c grows by this, so the
 * two cannot disagree (the "dark ghost of the old position" bug). */

enum wm_shadow_kind {
    WMSH_WIN_FOCUSED = 0,
    WMSH_WIN_UNFOCUSED,
    WMSH_MENU,
    WMSH_OVERLAY,       /* overlay panel and the window switcher */
    WMSH_DOCK,
    WMSH_NKIND
};

struct wm_shadow_layer {
    short dy_pt;            /* offset down, points */
    short sigma_pt;         /* gaussian sigma, points */
    short spread_pt;        /* shape growth, points */
    unsigned char a_dark;   /* peak alpha on a dark UI */
    unsigned char a_light;  /* peak alpha on a light UI */
};

#define WMSH_NLAYER 2       /* [0] key, [1] ambient */

static const struct wm_shadow_layer wm_shadow_preset[WMSH_NKIND][WMSH_NLAYER] = {
    /* focused window: deep and low */
    { { 14, 9, 0, 120, 70 }, { 4, 16, 0, 70, 40 } },
    /* unfocused: sits lower, the whole depth cue */
    { {  3, 4, 0,  62, 28 }, { 1,  8, 0, 40, 18 } },
    /* menu dropdown */
    { {  6, 5, 0, 110, 50 }, { 2, 10, 0, 50, 24 } },
    /* overlay panel, switcher */
    { { 10, 9, 0, 125, 65 }, { 4, 18, 0, 55, 30 } },
    /* dock (flat alpha in both themes) */
    { {  6, 5, 0,  42, 42 }, { 2, 10, 0, 20, 20 } },
};

/* shadow_ex's own reach is 3 sigma at full alpha plus 2 px of rounding
 * (reach_px in display_shadow_ex.c); WM_SHADOW_PAD is that rounding. */
#define WM_SHADOW_PAD 2

static inline int wm_shadow_px(int pt, int pct) { return pt * pct / 100; }

/* Pixels the shadow of `kind` reaches past the caster's box on every side
 * (L, T, R, B): 3 sigma + |offset| + spread, taken over both layers. */
static inline void wm_shadow_extent(int kind, int pct, int out[4])
{
    int e = 0;
    if (kind < 0 || kind >= WMSH_NKIND) kind = WMSH_WIN_FOCUSED;
    for (int i = 0; i < WMSH_NLAYER; i++) {
        const struct wm_shadow_layer *l = &wm_shadow_preset[kind][i];
#ifdef WM_SHADOW_EXTENT_NEGCTL
        int v = (3 * l->sigma_pt * pct + 99) / 100;           /* blur only */
#else
        int dy = l->dy_pt < 0 ? -l->dy_pt : l->dy_pt;
        int sp = l->spread_pt > 0 ? l->spread_pt : 0;
        /* ceil each term in device px: whole points scale up fractionally */
        int v = (3 * l->sigma_pt * pct + 99) / 100 + (dy * pct + 99) / 100
              + (sp * pct + 99) / 100 + WM_SHADOW_PAD;
#endif
        if (v > e) e = v;
    }
    out[0] = out[1] = out[2] = out[3] = e;
}

/* One layer in the 24.8 device units shadow_ex takes. `scale256` is the
 * thumbnail scale (256 = full size, the animated path passes sr). */
static inline void wm_shadow_dev(const struct wm_shadow_layer *l, int pct, int scale256,
                                 int *dy24, int *sig24, int *spr24)
{
    long long k = (long long)pct * scale256;           /* /100/256 later */
    *dy24  = (int)((long long)l->dy_pt * 256 * k / (100 * 256));
    *sig24 = (int)((long long)l->sigma_pt * 256 * k / (100 * 256));
    *spr24 = (int)((long long)l->spread_pt * 256 * k / (100 * 256));
    if (*sig24 < 64) *sig24 = 64;
}
#endif
