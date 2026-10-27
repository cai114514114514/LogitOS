/* wm_glass.c -- the chrome's materials.
 *
 * Every piece of WM chrome glass goes through here: wm_glass_panel maps a
 * material kind (enum wm_mat) to today's exact tint and call, timed into the
 * frame's glass counters; wm_glass_regions lists the glass panels on screen
 * for dmg_expand and the band splitter. The app's legacy SYS_GUI_GLASS is
 * here too, so no other wm*.c file calls fb_liquid_glass.
 *
 * Split out of wm.c with no change in behaviour (ui-wm-split); see
 * wm_internal.h for what the files share. */
#include "wm_internal.h"

/* Exact phase attribution for the rare full frame.  Timing the compositor's
 * glass calls here avoids fb.c's app-surface calls and therefore answers the
 * question being asked: live-backdrop glass during this frame, not every use
 * of the drawing API in the machine.  `glass_px` is work pixels (one panel
 * drawn twice counts twice), deliberately: that is the quantity proportional
 * to CPU cost.  The base and present pixel counts remain W*H for a full frame.
 *
 * The trace is printed only after wm_render drops graphics_lock.  Printing it
 * inside wm_render_locked would add serial latency to the very hold time this
 * instrument exists to measure, a silent self-measurement failure. */
uint64_t wm_frame_glass_ns, wm_frame_glass_px;

/* The frosted menu bar + dock now blur the live backdrop per-frame via
 * fb_blur_rect (a fast separable box blur, see fb.c) instead of a baked-once
 * O(r^2) box blur -- so windows slid under them read as true frosted glass.
 * Each call is timed into the frame's glass counters above. */
static void profiled_glass(int x, int y, int w, int h, int radius,
                           uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    uint64_t t = time_mono_ns();
    fb_liquid_glass(x, y, w, h, radius, tr, tg, tb, ta);
    __atomic_fetch_add(&wm_frame_glass_ns, time_mono_ns() - t, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wm_frame_glass_px, (uint64_t)w * (uint64_t)h, __ATOMIC_RELAXED);
}

static void profiled_glass_cut(int x, int y, int w, int h, int radius,
                               uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta,
                               unsigned cut)
{
    uint64_t t = time_mono_ns();
    fb_liquid_glass_cut(x, y, w, h, radius, tr, tg, tb, ta, cut);
    __atomic_fetch_add(&wm_frame_glass_ns, time_mono_ns() - t, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wm_frame_glass_px, (uint64_t)w * (uint64_t)h, __ATOMIC_RELAXED);
}

/* ---- the chrome's materials, by KIND ----------------------------------------
 * Every piece of WM chrome glass names what it IS, and this table says what
 * that kind is drawn with -- today's exact tint pairs and today's exact call
 * (fb_liquid_glass_cut for the two cut slabs, fb_liquid_glass for the floating
 * panels). One place to restyle a material (design 3.2), instead of six
 * painters that each carry their own pair of literals. `alpha` < 0 is the
 * kind's own tint alpha; the window titlebar passes its focus-dependent one.
 * `cut` is honoured only by the kinds drawn with the cut call. */
static const struct wm_mat_style {
    uint8_t light[4], dark[4];   /* r, g, b, a */
    uint8_t cut_call;
} wm_mat_style[WM_MAT_COUNT] = {
    [WM_MAT_MENUBAR]  = { { 255, 255, 255, 110 }, { 24, 24, 32, 150 }, 1 },
    [WM_MAT_MENU]     = { { 250, 250, 255, 190 }, { 30, 30, 38, 165 }, 0 },
    [WM_MAT_DOCK]     = { { 200, 202, 216,  92 }, { 26, 26, 34, 104 }, 0 },
    [WM_MAT_OVERLAY]  = { { 250, 250, 255, 210 }, { 28, 28, 36, 195 }, 0 },
    [WM_MAT_SWITCHER] = { { 250, 250, 255, 210 }, { 28, 28, 36, 195 }, 0 },
    [WM_MAT_TITLE]    = { { 250, 250, 255,   0 }, { 30, 30, 40,   0 }, 1 },
};

void wm_glass_panel(enum wm_mat kind, int x, int y, int w, int h, int r_pt,
                           unsigned cut, int alpha)
{
    const struct wm_mat_style *m = &wm_mat_style[kind];
    const uint8_t *t = g_ui_dark ? m->dark : m->light;
    uint8_t a = alpha >= 0 ? (uint8_t)alpha : t[3];
    if (m->cut_call) profiled_glass_cut(x, y, w, h, S(r_pt), t[0], t[1], t[2], a, cut);
    else             profiled_glass(x, y, w, h, S(r_pt), t[0], t[1], t[2], a);
}

/* SYS_GUI_GLASS (the legacy app call): glass over an app's OWN canvas, which
 * the caller has already targeted. Not chrome, so not profiled: the frame's
 * glass counters measure the compositor's live-backdrop glass only. Here so
 * that every fb_liquid_glass call under c/kernel/gui/wm*.c is in this file. */
void wm_glass_app(int x, int y, int w, int h, int radius,
                         uint8_t tr, uint8_t tg, uint8_t tb, uint8_t ta)
{
    fb_liquid_glass(x, y, w, h, radius, tr, tg, tb, ta);
}

/* ---- where the chrome glass IS ----------------------------------------------
 * Every chrome panel that frosts the live backdrop this frame, as its panel
 * box and corner radius (both device px), in the order dmg_expand and the band
 * splitter have always visited them: the menu bar, the dock slab, the open
 * dropdown, the About/power panel, the switcher, then each window's titlebar
 * back to front. Each surface's own file says where its panel is; this is the
 * ONE list both consumers read, so a new glass surface is protected from a
 * damage cut and from a band cut by adding one provider here. */
int wm_glass_regions(struct wm_glass_region *out, int max)
{
    int n = 0;
    n += wm_menubar_glass_rect(out + n, max - n);
    n += wm_dock_glass_rect(out + n, max - n);
    n += wm_menu_glass_rect(out + n, max - n);
    n += wm_overlay_glass_rect(out + n, max - n);
    n += wm_chrome_glass_rects(out + n, max - n);
    return n;
}

/* Idle work for the glass caches (design P9's C1/C2), none yet. */
int wm_glass_tick(uint64_t now) { (void)now; return 0; }
