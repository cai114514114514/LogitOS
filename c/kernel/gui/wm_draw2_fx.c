/* SYS_GUI_SHADOW2 and SYS_GUI_GLASS2 (GUI ABI v2).
 *
 *   wm2_shadow2  -> ol_display_shadow_ex       (rounded-rect gaussian box shadow)
 *   wm2_glass2   -> ol_display_liquid_glass_ex (glass over the window's OWN content)
 *
 * Both draw on e->d inside whatever clip and group levels wm_draw2_state has
 * open (the surface clip is part of the display); they never push their own.
 * No statics: the shadow uses the display's OL_DS_SHADOW slot, the glass its
 * own per-display buffer, so both are per context.
 *
 * SCALE. The rect goes through wm2_rect24 (LOGIT_RECT_SNAP honoured) and is
 * then rounded edge-to-edge to whole device pixels, the units the display
 * calls take. Radii, offset, spread and the 24.8 sigma go through wm2_pt24, so
 * sigma scales by S(pct); radii are rounded to the nearest device pixel.
 *
 * STATUS. OL_ARGUMENT: wrong size, copy fault, negative extent, radius or
 * sigma out of range (sigma <= 64 pt), undefined flag/enum bits, non-zero
 * reserved fields; nothing is drawn. OL_UNSUPPORTED: glass2 with four
 * different corner radii (the material has one radius). A zero-area rect
 * draws nothing and returns OL_OK. Reduce transparency is the display flag,
 * handled by ol_display_liquid_glass_ex (solid panel).
 *
 * -DWM_DRAW2_FX_NO_SCALE (negative control) passes sigma unscaled. */
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "openlogit_display_shadow.h"
#include "openlogit_glass_material.h"

const unsigned wm2_fx_info = LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_GLASS2;

#define FX_COORD_MAX (32768 * 256)     /* +-32768 pt, 24.8 */
#define FX_SIGMA_MAX (64 * 256)        /* 64 pt */
#define FX_GLASS_CUT_ALL 15u

static int fx_in(int v, int lo, int hi) { return v >= lo && v <= hi; }

static int px_round(int v24)           /* 24.8 -> nearest whole pixel */
{
    return wm2_sat(wm2_floordiv((long long)v24 + 128, 256));
}

/* Device rect in whole pixels from a 24.8 point rect; edges rounded, so
 * abutting rects stay abutting. Returns 0 for an empty one. */
static int dev_rect(const struct wm2_env *e, int x, int y, int w, int h, unsigned flags,
                    int *ox, int *oy, int *ow, int *oh)
{
    struct wm2_rect24 r = wm2_rect24(x, y, w, h, e->scale_pct, (flags & LOGIT_RECT_SNAP) != 0);
    int x0 = px_round(r.x), y0 = px_round(r.y);
    int x1 = px_round(wm2_sat((long long)r.x + r.w)), y1 = px_round(wm2_sat((long long)r.y + r.h));
    *ox = x0; *oy = y0; *ow = wm2_sat((long long)x1 - x0); *oh = wm2_sat((long long)y1 - y0);
    return *ow > 0 && *oh > 0;
}

static int rect_ok(int x, int y, int w, int h)
{
    return fx_in(x, -FX_COORD_MAX, FX_COORD_MAX) && fx_in(y, -FX_COORD_MAX, FX_COORD_MAX) &&
           fx_in(w, 0, FX_COORD_MAX) && fx_in(h, 0, FX_COORD_MAX);
}

static int radii_ok(const int *r)
{
    for (int i = 0; i < 4; i++)
        if (!fx_in(r[i], 0, FX_COORD_MAX)) return 0;
    return 1;
}

static void dev_radii(const struct wm2_env *e, const int *r, int *out)
{
    for (int i = 0; i < 4; i++) out[i] = px_round(wm2_pt24(r[i], e->scale_pct));
}

long wm2_shadow2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_shadow2 s;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &s, a, sizeof s);
    if (rc != OL_OK) return rc;
    if (!e->d) return OL_STATE;
    if (s.flags & ~(LOGIT_RECT_SNAP | LOGIT_SHADOW2_INSET)) return OL_ARGUMENT;
    if (!rect_ok(s.x, s.y, s.w, s.h) || !radii_ok(s.radius)) return OL_ARGUMENT;
    if (!fx_in(s.blur, 0, FX_SIGMA_MAX)) return OL_ARGUMENT;
    if (!fx_in(s.dx, -FX_COORD_MAX, FX_COORD_MAX) || !fx_in(s.dy, -FX_COORD_MAX, FX_COORD_MAX) ||
        !fx_in(s.spread, -FX_COORD_MAX, FX_COORD_MAX))
        return OL_ARGUMENT;
    if (e->scale_pct <= 0) return OL_STATE;

    int x, y, w, h, rad[4];
    if (!dev_rect(e, s.x, s.y, s.w, s.h, s.flags, &x, &y, &w, &h)) return OL_OK;
    dev_radii(e, s.radius, rad);
#ifdef WM_DRAW2_FX_NO_SCALE
    int sigma = s.blur;
#else
    int sigma = wm2_pt24(s.blur, e->scale_pct);
#endif
    return ol_display_shadow_ex(e->d, x, y, w, h, rad,
                                wm2_pt24(s.dx, e->scale_pct), wm2_pt24(s.dy, e->scale_pct),
                                sigma, wm2_pt24(s.spread, e->scale_pct),
                                s.argb & 0xFFFFFFu, (int)(s.argb >> 24),
                                (s.flags & LOGIT_SHADOW2_INSET) != 0);
}

long wm2_glass2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_glass2 g;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &g, a, sizeof g);
    if (rc != OL_OK) return rc;
    if (!e->d) return OL_STATE;
    if (g.flags & ~LOGIT_RECT_SNAP) return OL_ARGUMENT;
    if (g.reserved[0] || g.reserved[1]) return OL_ARGUMENT;
    if (!rect_ok(g.x, g.y, g.w, g.h) || !radii_ok(g.radius)) return OL_ARGUMENT;
    if (g.cut > FX_GLASS_CUT_ALL) return OL_ARGUMENT;
    if (!fx_in(g.opacity, 0, 255) || !fx_in(g.tone, 0, 255)) return OL_ARGUMENT;
    if (!fx_in(g.blur_level, OL_GLASS_BLUR_REGULAR, OL_GLASS_BLUR_ULTRA) ||
        !fx_in(g.rim, OL_GLASS_RIM_DIRECTIONAL, OL_GLASS_RIM_SYMMETRIC))
        return OL_ARGUMENT;
    if (g.radius[1] != g.radius[0] || g.radius[2] != g.radius[0] || g.radius[3] != g.radius[0])
        return OL_UNSUPPORTED;
    if (e->scale_pct <= 0) return OL_STATE;

    int x, y, w, h, rad[4];
    if (!dev_rect(e, g.x, g.y, g.w, g.h, g.flags, &x, &y, &w, &h)) return OL_OK;
    dev_radii(e, g.radius, rad);
    struct ol_glass_material m = {0};
    m.tint = g.tint_argb & 0xFFFFFFu;
    m.opacity = (uint8_t)g.opacity;
    m.tone = (uint8_t)g.tone;
    m.blur = (uint8_t)g.blur_level;
    m.rim = (uint8_t)g.rim;
    m.fg_rgb = g.fg_rgb & 0xFFFFFFu;
    ol_display_liquid_glass_ex(e->d, x, y, w, h, rad[0], g.cut, &m);
    return OL_OK;
}
