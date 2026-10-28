/* SYS_GUI_ICON2 (GUI ABI v2): one symbol of the kernel's vector set
 * (icons.h; GICON_* in c/apps/logit.h) at 24.8-point placement and a stroke
 * weight, drawn into the targeted window surface through icon_draw_w -- the
 * same rasteriser and the same 64-entry (id, device px, weight) LRU the
 * legacy SYS_GUI_ICON uses, so a symbol drawn both ways is one cache entry.
 *
 * UNITS. x, y (top-left) and px are 24.8 logical points (16 pt = 16 << 8),
 * converted with the v2 floor rule (wm2_scale.h), px at least one device
 * pixel. That is the rule TEXT_RUN2 uses, so a symbol and the label beside it
 * land on the same device rows at 125 % and 150 %. px is re-rasterised at the
 * device size (icons are 0..100-unit paths), never scaled from a bitmap.
 *
 * BOUNDS. id in [0, ICON_COUNT) (LOGIT_ICON_APP, 65535, is outside: an app
 * icon needs its name and stays on SYS_GUI_ICON); device px 1..512, the
 * legacy case's cap (icon_draw allocates px * px); weight 0 (= 2) or 1..3;
 * flags 0. Anything else is OL_ARGUMENT and draws nothing.
 *
 * COLOUR. A raw app value: only RGB is kept (& 0xFFFFFF), the backdrop
 * marker rule of wm_draw2.h -- fb_blit_glyph rebuilds each pixel from
 * channels anyway, so the high byte could not reach the surface, but the
 * contract is stated where the value enters.
 *
 * LOCKING. Runs with graphics_lock held and wm_lock dropped (a pixel call,
 * is_draw_call); icon_draw_w takes icon_lock (a spinlock) around its cache,
 * fill and blit and calls nothing that sleeps. No state of its own. */
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "icons.h"

const unsigned wm2_icon2_info = LOGIT_GUI2_F_ICON2;

#define ICON2_DEV_MAX 512

static inline int icon2_dev(int v24, int pct)
{
    return wm2_sat(wm2_floordiv((long long)v24 * pct, 25600));
}

long wm2_icon2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_icon2 ic;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &ic, a, sizeof ic);
    if (rc != OL_OK) return rc;
    if (ic.flags) return OL_ARGUMENT;
    if (ic.id < 0 || ic.id >= ICON_COUNT) return OL_ARGUMENT;
    int weight = ic.weight ? ic.weight : 2;
    if (weight < 1 || weight > 3) return OL_ARGUMENT;
    if (ic.px <= 0) return OL_ARGUMENT;
    int px = icon2_dev(ic.px, e->scale_pct);
    if (px < 1) px = 1;
    if (px > ICON2_DEV_MAX) return OL_ARGUMENT;
    int x = icon2_dev(ic.x, e->scale_pct), y = icon2_dev(ic.y, e->scale_pct);
    icon_draw_w(ic.id, x, y, px, ic.color & 0xFFFFFFu, weight);
    return OL_OK;
}
