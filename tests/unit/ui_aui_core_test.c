/* ui_aui_core_test.c -- the 「澄」 paint API, theme roles and scroll query of
 * the real ring-3 aui.c (make test-ui-aui-core-api; design P4, 3.2, 3.3).
 *
 *   material   every logit_mat x light/dark is ONE SYS_GUI_GLASS2 with the
 *              table's tier, tint (rgb) and opacity (its alpha), cut, four
 *              equal radii, SYMMETRIC rim, default tone; no PATH
 *                                     FAIL material / FAIL material-rim
 *   rt         ui.reduce_transparency, or a kernel without GLASS2: 0 GLASS2
 *              and ONE PATH whose paint is the material's RT colour
 *                                                            FAIL material-rt
 *   backdrop   ONE SYS_GUI_BACKDROP: id, tier, tint, fill_rgb = the RT solid;
 *              RT: 0 BACKDROP, 1 PATH, and the region registered earlier is
 *              cleared; aui_backdrop_clear(-1) clears all     FAIL backdrop
 *   image      ONE SYS_GUI_BLIT2 per filter (NEAREST, BILINEAR, BOX), the
 *              whole image into dst; without BLIT2 the legacy SYS_GUI_BLIT
 *                                                               FAIL image
 *   elev       E1..E6 and E-Dock: exactly 2 SHADOW2 each (key, ambient) with
 *              the table's dx/dy/sigma/spread and shadow-ink ARGB per mode;
 *              E0 draws nothing                                  FAIL elev
 *   halo       at 100/125/150/200 %: the frame's flush covers each material
 *              tier's reach (THIN 3, REGULAR 6, THICK 12, ULTRA 20 dev px) and
 *              E5's (offset + 3 sigma + spread) around the rect, and is not
 *              more than 2 pt wider                              FAIL halo
 *   roles      aui_tok() and the appended aui_t roles are the mode's table
 *              with accent roles derived from the current aui_t.accent; they
 *              follow aui_set_dark and aui_set_accent; legacy fields keep
 *              their values                                FAIL theme-roles
 *   scroll     aui_scroll_offset(id) answers container id's displayed offset,
 *              last frame's before it reopens, 0 for an unknown id
 *                                                        FAIL scroll-offset
 *
 * THE CONTROL (-DUI_AUI_CORE_NEGCTL) makes aui ask for the DIRECTIONAL rim:
 * the material (and backdrop) rim checks must report 'FAIL material-rim' and
 * nothing else (every other check still runs and passes). Exits 1 on any
 * failure. */
#include "ui_aui_core_hook.h"

static const char *const MAT_NAME[LOGIT_MAT_COUNT] = {
    "menubar", "menu", "dock", "spotlight", "about", "switcher", "sidebar",
    "popover", "hud-dark", "pending", "greeter", "regular", "title",
};

static void setup(int pct, int dark, unsigned info)
{
    W.pct = pct; W.dark = dark; W.info = info;
    aui_set_size(1000, 800);
}

/* The radius aui_material must pick for rect (w, h) and r = -1. */
static int table_radius(const struct logit_material *m, int w, int h)
{
    int r = m->radius_pt == LOGIT_R_CAPSULE ? (w < h ? w : h) / 2 : m->radius_pt > 0 ? m->radius_pt : 0;
    int lim = (w < h ? w : h) / 2;
    return r > lim ? lim : r;
}

static struct aui_rect g_rc;
static int g_mat, g_id, g_filter, g_elev;
static void draw_material(void *p) { (void)p; aui_material(g_rc, -1, (enum logit_mat)g_mat); }
static void draw_backdrop(void *p) { (void)p; aui_backdrop(g_id, g_rc, -1, (enum logit_mat)g_mat); }
static void draw_elev(void *p) { (void)p; aui_elev(g_rc, 12, (enum logit_elev)g_elev); }

/* ---------------------------------------------------------------- material */
static void case_material(int dark)
{
    setup(100, dark, LOGIT_GUI2_F_GLASS2 | LOGIT_GUI2_F_PATH | LOGIT_GUI2_F_SHADOW2);
    g_rc = aui_r(40, 40, 240, 96);
    core_frame(0, 0);
    for (g_mat = 0; g_mat < LOGIT_MAT_COUNT; g_mat++) {
        const struct logit_material *m = logit_material((enum logit_mat)g_mat);
        unsigned tint = dark ? m->tint_d : m->tint_l;
        int rr = table_radius(m, g_rc.w, g_rc.h) * 256;
        const char *mode = dark ? "dark" : "light";
        ch_reset();
        core_frame(draw_material, 0);
        if (W.nglass != 1 || W.npath != 0) { FAIL("material %s %s: %d GLASS2 %d PATH, want 1 and 0", MAT_NAME[g_mat], mode, W.nglass, W.npath); continue; }
        const struct logit_glass2 *g = &W.glass[0];
        if (g->rim != OL_GLASS_RIM_SYMMETRIC)
            FAIL("material-rim %s %s: rim %d, want SYMMETRIC (%d)", MAT_NAME[g_mat], mode, g->rim, OL_GLASS_RIM_SYMMETRIC);
        if (g->blur_level != m->tier || g->tint_argb != (tint & 0xFFFFFFu) || g->opacity != (int)(tint >> 24) ||
            g->cut != m->cut || g->tone != 0)
            FAIL("material %s %s: tier %d tint %06x op %d cut %u tone %d, want %d %06x %d %u 0", MAT_NAME[g_mat], mode,
                 g->blur_level, g->tint_argb, g->opacity, g->cut, g->tone, m->tier, tint & 0xFFFFFFu, (int)(tint >> 24), (unsigned)m->cut);
        if (g->radius[0] != rr || g->radius[1] != rr || g->radius[2] != rr || g->radius[3] != rr)
            FAIL("material %s %s: radii %d %d %d %d, want four x %d", MAT_NAME[g_mat], mode,
                 g->radius[0], g->radius[1], g->radius[2], g->radius[3], rr);
        if (g->x != 40 * 256 || g->y != 40 * 256 || g->w != 240 * 256 || g->h != 96 * 256)
            FAIL("material %s %s: rect %d %d %d %d (24.8)", MAT_NAME[g_mat], mode, g->x, g->y, g->w, g->h);
    }
    if (!bad) printf("ok   material %s: %d materials, one SYMMETRIC GLASS2 each\n", dark ? "dark" : "light", LOGIT_MAT_COUNT);
}

/* RT: arg bit0 = dark, bit1 = no GLASS2 capability instead of the setting */
static void case_rt(int arg)
{
    int dark = arg & 1, nocap = arg & 2;
    setup(100, dark, (nocap ? 0 : LOGIT_GUI2_F_GLASS2) | LOGIT_GUI2_F_PATH);
    W.reduce_t = !nocap;
    g_rc = aui_r(40, 40, 240, 96);
    core_frame(0, 0);
    for (g_mat = 0; g_mat < LOGIT_MAT_COUNT; g_mat++) {
        const struct logit_material *m = logit_material((enum logit_mat)g_mat);
        unsigned rt = dark ? m->rt_d : m->rt_l;
        ch_reset();
        core_frame(draw_material, 0);
        if (W.nglass != 0 || W.npath != 1)
            FAIL("material-rt %s %s%s: %d GLASS2 %d PATH, want 0 and 1", MAT_NAME[g_mat], dark ? "dark" : "light",
                 nocap ? " (no GLASS2)" : "", W.nglass, W.npath);
        else if (W.paint[0].argb != rt || W.paint[0].kind != LOGIT_PAINT2_SOLID)
            FAIL("material-rt %s: paint %08x, want the RT solid %08x", MAT_NAME[g_mat], W.paint[0].argb, rt);
    }
    if (!bad) printf("ok   material-rt %s%s: one RT-solid PATH, no GLASS2\n", dark ? "dark" : "light", nocap ? " without GLASS2" : "");
}

/* ---------------------------------------------------------------- backdrop */
static void case_backdrop(int dark)
{
    const struct logit_material *m = logit_material(LOGIT_MAT_SIDEBAR);
    unsigned tint = dark ? m->tint_d : m->tint_l, rt = dark ? m->rt_d : m->rt_l;
    setup(100, dark, LOGIT_GUI2_F_BACKDROP | LOGIT_GUI2_F_GLASS2 | LOGIT_GUI2_F_PATH);
    g_rc = aui_r(8, 8, 220, 400); g_mat = LOGIT_MAT_SIDEBAR;
    core_frame(0, 0);
    for (g_id = 0; g_id < LOGIT_BACKDROP_MAX; g_id += 7) {
        ch_reset();
        core_frame(draw_backdrop, 0);
        if (W.nbd != 1 || W.nglass != 0 || W.npath != 0) { FAIL("backdrop id %d: %d BACKDROP %d GLASS2 %d PATH, want 1 0 0", g_id, W.nbd, W.nglass, W.npath); continue; }
        const struct logit_backdrop2 *b = &W.bd[0];
        if (b->rim != OL_GLASS_RIM_SYMMETRIC)
            FAIL("material-rim backdrop %s: rim %d, want SYMMETRIC", dark ? "dark" : "light", b->rim);
        if (b->id != g_id || b->fill_rgb != (rt & 0xFFFFFFu) || b->tint_argb != (tint & 0xFFFFFFu) ||
            b->opacity != (int)(tint >> 24) || b->blur_level != m->tier || b->flags != LOGIT_RECT_SNAP)
            FAIL("backdrop id %d: id %d fill %06x tint %06x op %d tier %d flags %u, want %d %06x %06x %d %d SNAP",
                 g_id, b->id, b->fill_rgb, b->tint_argb, b->opacity, b->blur_level, b->flags,
                 g_id, rt & 0xFFFFFFu, tint & 0xFFFFFFu, (int)(tint >> 24), m->tier);
        if (b->radius[0] != 12 * 256 || b->radius[3] != 12 * 256 || b->w != 220 * 256 || b->h != 400 * 256)
            FAIL("backdrop id %d: radius %d rect %dx%d (24.8)", g_id, b->radius[0], b->w, b->h);
    }
    /* id 7 is registered now; RT turns it into the solid and clears it */
    g_id = 7;
    W.reduce_t = 1;
    ch_reset();
    core_frame(draw_backdrop, 0);
    if (W.nbd != 0 || W.npath != 1 || W.nbdclr != 1 || W.bdclr[0] != 7)
        FAIL("backdrop rt: %d BACKDROP %d PATH %d CLEAR (id %d), want 0 1 1 (7)", W.nbd, W.npath, W.nbdclr, W.nbdclr ? W.bdclr[0] : -9);
    else if (W.paint[0].argb != rt)
        FAIL("backdrop rt: paint %08x, want %08x", W.paint[0].argb, rt);
    ch_reset();
    core_frame(draw_backdrop, 0);
    if (W.nbdclr != 0) FAIL("backdrop rt: a second solid frame cleared again (%d)", W.nbdclr);
    ch_reset();
    aui_backdrop_clear(-1);
    if (W.nbdclr != 1 || W.bdclr[0] != -1) FAIL("backdrop clear(-1): %d calls", W.nbdclr);
    /* out of range ids draw nothing */
    ch_reset();
    g_id = LOGIT_BACKDROP_MAX;
    core_frame(draw_backdrop, 0);
    if (W.nbd || W.npath) FAIL("backdrop id 8 drew (%d BACKDROP %d PATH)", W.nbd, W.npath);
    if (!bad) printf("ok   backdrop %s: id/fill/tint per call, RT solid + clear\n", dark ? "dark" : "light");
}

/* ------------------------------------------------------------------- image */
static unsigned char g_img[16 * 8 * 4];
static void draw_image(void *p) { (void)p; aui_image(g_img, 16, 8, g_rc, g_filter); }
static void case_image(int nocap)
{
    static const int filters[3] = { LOGIT_FILTER_NEAREST, LOGIT_FILTER_BILINEAR, LOGIT_FILTER_BOX };
    setup(100, 0, nocap ? 0 : LOGIT_GUI2_F_BLIT2);
    for (int i = 0; i < (int)sizeof g_img; i++) g_img[i] = (unsigned char)(i * 7);
    g_rc = aui_r(30, 20, 64, 32);
    core_frame(0, 0);
    for (int f = 0; f < 3; f++) {
        g_filter = filters[f];
        ch_reset();
        core_frame(draw_image, 0);
        if (nocap) {
            if (W.nblit2 != 0 || W.nblit != 1 || W.blit[0].x != 30 || W.blit[0].w != 64)
                FAIL("image (no BLIT2): %d BLIT2 %d BLIT, want 0 and 1 at the dst rect", W.nblit2, W.nblit);
            continue;
        }
        if (W.nblit2 != 1 || W.nblit != 0) { FAIL("image filter %d: %d BLIT2 %d BLIT, want 1 and 0", g_filter, W.nblit2, W.nblit); continue; }
        const struct logit_blit2 *b = &W.blit2[0];
        if (b->filter != g_filter || b->rgba != g_img || b->sw != 16 || b->sh != 8 || b->sstride != 64 ||
            b->src_x || b->src_y || b->src_w != 16 || b->src_h != 8 || b->opacity != 255 ||
            b->x != 30 * 256 || b->y != 20 * 256 || b->w != 64 * 256 || b->h != 32 * 256)
            FAIL("image filter %d: filter %d src %d,%d %dx%d of %dx%d dst %d %d %d %d op %d", g_filter, b->filter,
                 b->src_x, b->src_y, b->src_w, b->src_h, b->sw, b->sh, b->x, b->y, b->w, b->h, b->opacity);
    }
    if (!bad) printf("ok   image%s\n", nocap ? ": gui_blit fallback without BLIT2" : ": one BLIT2 per filter x3");
}

/* -------------------------------------------------------------------- elev */
static void case_elev(int dark)
{
    setup(100, dark, LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_PATH);
    g_rc = aui_r(300, 200, 200, 120);
    core_frame(0, 0);
    for (g_elev = 0; g_elev < LOGIT_ELEV_COUNT; g_elev++) {
        const struct logit_elevation *v = logit_elevation((enum logit_elev)g_elev);
        ch_reset();
        core_frame(draw_elev, 0);
        int want = g_elev == LOGIT_E0 ? 0 : 2;
        if (W.nshadow != want) { FAIL("elev E%d: %d SHADOW2, want %d", g_elev, W.nshadow, want); continue; }
        for (int k = 0; k < want; k++) {
            const struct logit_shadow_layer *l = k ? &v->ambient : &v->key;
            const struct logit_shadow2 *s = &W.shadow[k];
            unsigned argb = ((unsigned)(dark ? l->a_dark : l->a_light) << 24) | (dark ? LOGIT_SHADOW_INK_D : LOGIT_SHADOW_INK_L);
            if (s->dx != l->dx || s->dy != l->dy || s->blur != l->sigma_pt || s->spread != l->spread ||
                s->argb != argb || (s->flags & LOGIT_SHADOW2_INSET) ||
                s->radius[0] != 12 * 256 || s->x != 300 * 256 || s->w != 200 * 256)
                FAIL("elev E%d %s layer %d: dx %d dy %d sigma %d spread %d argb %08x, want %d %d %d %d %08x",
                     g_elev, k ? "ambient" : "key", k, s->dx, s->dy, s->blur, s->spread, s->argb,
                     l->dx, l->dy, l->sigma_pt, l->spread, argb);
        }
    }
    /* the E5 numbers, spelled out (design 3.3): {0,8,12,0} / {0,20,30,-4} */
    g_elev = LOGIT_E5;
    ch_reset();
    core_frame(draw_elev, 0);
    if (W.nshadow != 2 || W.shadow[0].dy != 8 * 256 || W.shadow[0].blur != 12 * 256 ||
        W.shadow[1].dy != 20 * 256 || W.shadow[1].blur != 30 * 256 || W.shadow[1].spread != -4 * 256 ||
        (W.shadow[0].argb >> 24) != (dark ? 128u : 56u) || (W.shadow[1].argb >> 24) != (dark ? 102u : 41u))
        FAIL("elev E5 %s: not {0,8,12,0,%d} + {0,20,30,-4,%d}", dark ? "dark" : "light", dark ? 128 : 56, dark ? 102 : 41);
    if (!bad) printf("ok   elev %s: E1..E6 + E-Dock two SHADOW2 each, E0 none\n", dark ? "dark" : "light");
}

/* -------------------------------------------------------------------- halo */
/* Does the union of the frame's flushes cover the rect grown by (l,t,r,b)
 * device px, at most `slack` points wider on each side? */
static void check_cover(const char *what, int pct, struct aui_rect rc, int l, int t, int r, int b, int slack)
{
    int x0, y0, x1, y1;
    if (!core_flush_union(&x0, &y0, &x1, &y1)) { FAIL("halo %s @%d: no FLUSH_RECT (%d full flushes)", what, pct, W.nfull); return; }
    int need_x0 = core_dev(rc.x, pct) - l, need_y0 = core_dev(rc.y, pct) - t;
    int need_x1 = core_dev(rc.x + rc.w, pct) + r, need_y1 = core_dev(rc.y + rc.h, pct) + b;
    if (core_dev(x0, pct) > need_x0 || core_dev(y0, pct) > need_y0 ||
        core_dev(x1, pct) < need_x1 || core_dev(y1, pct) < need_y1)
        FAIL("halo %s @%d: flush %d,%d..%d,%d pt does not cover %d,%d..%d,%d dev", what, pct, x0, y0, x1, y1,
             need_x0, need_y0, need_x1, need_y1);
    /* tight: within the reach converted to points, + slack */
    int lp = (l * 100 + pct - 1) / pct + slack, tp = (t * 100 + pct - 1) / pct + slack;
    int rp = (r * 100 + pct - 1) / pct + slack, bp = (b * 100 + pct - 1) / pct + slack;
    if (x0 < rc.x - lp || y0 < rc.y - tp || x1 > rc.x + rc.w + rp || y1 > rc.y + rc.h + bp)
        FAIL("halo %s @%d: flush %d,%d..%d,%d pt is wider than the reach (+%d pt)", what, pct, x0, y0, x1, y1, slack);
}

static void case_halo(int pct)
{
    static const struct { int mat, tier, reach; const char *name; } tiers[4] = {
        { LOGIT_MAT_HUD_DARK, LOGIT_TIER_THIN, 3, "THIN" },
        { LOGIT_MAT_MENUBAR, LOGIT_TIER_REGULAR, 6, "REGULAR" },
        { LOGIT_MAT_MENU, LOGIT_TIER_THICK, 12, "THICK" },
        { LOGIT_MAT_ABOUT, LOGIT_TIER_ULTRA, 20, "ULTRA" },
    };
    setup(pct, 0, LOGIT_GUI2_F_GLASS2 | LOGIT_GUI2_F_PATH | LOGIT_GUI2_F_SHADOW2);
    g_rc = aui_r(300, 250, 200, 90);
    for (int i = 0; i < 4; i++) {
        if (logit_material((enum logit_mat)tiers[i].mat)->tier != tiers[i].tier) FAIL("halo: table tier of %s moved", tiers[i].name);
        g_mat = tiers[i].mat;
        ch_reset(); core_frame(0, 0);                 /* empty frame: the diff baseline */
        ch_reset(); core_frame(draw_material, 0);
        check_cover(tiers[i].name, pct, g_rc, tiers[i].reach, tiers[i].reach, tiers[i].reach, tiers[i].reach, 2);
        ch_reset(); core_frame(0, 0);                 /* and its removal flushes the same */
        check_cover(tiers[i].name, pct, g_rc, tiers[i].reach, tiers[i].reach, tiers[i].reach, tiers[i].reach, 2);
    }
    /* E5: offset + 3 sigma + spread, per side, the larger of its two layers */
    {
        const struct logit_elevation *v = logit_elevation(LOGIT_E5);
        int side = 0, top = 0, bot = 0;
        for (int k = 0; k < 2; k++) {
            const struct logit_shadow_layer *l = k ? &v->ambient : &v->key;
            int s3 = 3 * l->sigma_pt + (l->spread > 0 ? l->spread : 0);       /* 24.8 pt */
            int sd = (int)(((long long)s3 * pct + 25599) / 25600);           /* dev px, up */
            int dy = (int)(((long long)l->dy * pct + 25599) / 25600);
            if (sd > side) side = sd;
            if (sd > top) top = sd;
            if (sd + dy > bot) bot = sd + dy;
        }
        g_elev = LOGIT_E5;
        ch_reset(); core_frame(0, 0);
        ch_reset(); core_frame(draw_elev, 0);
        /* ad_halo_shadow adds 2 px of the kernel's own rounding: slack 2 pt +
         * those 2 px; it may not be tighter than the design's reach */
        check_cover("E5", pct, g_rc, side, top, side, bot, 4);
    }
    if (!bad) printf("ok   halo @%d%%: THIN 3 REGULAR 6 THICK 12 ULTRA 20 dev px and E5 covered, tight\n", pct);
}

/* ------------------------------------------------------------------- roles */
static int roles_match(const char *when, unsigned accent, int dark)
{
    struct logit_tokens want;
    const struct logit_tokens *t = logit_tok(dark), *got = aui_tok();
    memcpy(&want, t, sizeof want);
    logit_tokens_derive(accent, dark, &want);
    int ok = 1;
    if (memcmp(&want, got, sizeof want)) { FAIL("theme-roles %s: aui_tok() is not the %s table with derive(%06x)", when, dark ? "dark" : "light", accent); ok = 0; }
#define R(f) if (aui_t.f != want.f) { FAIL("theme-roles %s: aui_t." #f " %08x, want %08x", when, aui_t.f, want.f); ok = 0; }
    R(bg_window) R(bg_content) R(bg_card) R(bg_sidebar) R(bg_inspector) R(bg_toolbar) R(bg_raised)
    R(bg_sunken) R(bg_stage) R(fill_control) R(fill_hover) R(fill_pressed) R(fill_zebra) R(fill_inactive_sel)
    R(label_primary) R(label_secondary) R(label_tertiary) R(label_quaternary) R(label_on_accent)
    R(sep_hairline) R(sep_opaque) R(sep_strong) R(accent_hover) R(accent_pressed) R(accent_ink)
    R(accent_tint) R(accent_focus_ring) R(accent_sel_text) R(success_ink) R(warning_ink) R(error_ink)
    R(destructive_fill) R(shadow_ink)
#undef R
    return ok;
}
static void case_roles(int unused)
{
    (void)unused;
    setup(100, 0, 0);
    core_frame(0, 0);
    roles_match("light", aui_t.accent, 0);
    /* design 3.1.1 / 3.1.4 (ui-aui-theme): the legacy fields come from logit_tokens,
     * bg.window L #F4F4F8 and the default accent 靛 #5E5CE6 */
    if (aui_t.bg != 0xF4F4F8u || aui_t.accent != 0x5E5CE6u || aui_t.surface != 0xFFFFFFu)
        FAIL("theme-roles: legacy light fields moved (bg %06x accent %06x)", aui_t.bg, aui_t.accent);
    aui_set_dark(1);
    roles_match("set_dark", aui_t.accent, 1);
    if (aui_t.bg != 0x1E1E24u || aui_t.accent != 0x6563EEu)        /* bg.window D, 靛 D */
        FAIL("theme-roles: legacy dark fields moved (bg %06x accent %06x)", aui_t.bg, aui_t.accent);
    aui_set_accent(0x1F6FEBu);                     /* 蓝: the tuned swatch */
    roles_match("set_accent dark", 0x1F6FEBu, 1);
    if (aui_tok()->accent != 0x1F6FEBu || aui_tok()->accent_ink != 0x79AEFFu)
        FAIL("theme-roles: 蓝 dark accent %06x ink %06x, want 1f6feb 79aeff", aui_tok()->accent, aui_tok()->accent_ink);
    aui_set_dark(0);                               /* the accent base survives set_dark */
    aui_set_accent(0x1F6FEBu);
    if (aui_tok()->accent_ink != 0x1A62D6u) FAIL("theme-roles: 蓝 light ink %06x, want 1a62d6", aui_tok()->accent_ink);
    aui_set_accent(LOGIT_ACCENT_DEFAULT);
    if (aui_tok()->accent != 0x5E5CE6u || aui_tok()->accent_hover != 0x5250DAu)
        FAIL("theme-roles: default indigo %06x hover %06x", aui_tok()->accent, aui_tok()->accent_hover);
    aui_theme_override(1);
    roles_match("override", aui_t.accent, 1);
    if (!bad) printf("ok   theme-roles: aui_tok and %s roles follow mode and accent\n", "33");
}

/* ------------------------------------------------------------ scroll offset */
static int off1, off2;
static int q_before[4], q_mid[4];
static void draw_scrolls(void *p)
{
    (void)p;
    for (int i = 0; i < 4; i++) q_before[i] = aui_scroll_offset(i);
    aui_scroll_begin(10, 10, 200, 100, &off1, 1000);
    aui_scroll_end();
    for (int i = 0; i < 4; i++) q_mid[i] = aui_scroll_offset(i);
    aui_scroll_begin(300, 10, 200, 100, &off2, 400);
    aui_scroll_end();
}
static void case_scroll(int unused)
{
    (void)unused;
    setup(100, 0, 0);
    off1 = 120; off2 = 40;
    core_frame(draw_scrolls, 0);
    core_frame(draw_scrolls, 0);
    if (q_before[1] != 120 || q_before[2] != 40 || q_before[0] != 0 || q_before[3] != 0)
        FAIL("scroll-offset: before reopening %d %d %d %d, want 0 120 40 0", q_before[0], q_before[1], q_before[2], q_before[3]);
    off1 = 300;                                    /* an external jump lands at once */
    core_frame(draw_scrolls, 0);
    if (q_mid[1] != 300 || q_mid[2] != 40 || q_before[1] != 120)
        FAIL("scroll-offset: this frame %d %d (before %d), want 300 40 (120)", q_mid[1], q_mid[2], q_before[1]);
    off2 = 9999;                                   /* clamped to content - view */
    core_frame(draw_scrolls, 0);
    if (aui_scroll_offset(2) != 300) FAIL("scroll-offset: clamped %d, want 300", aui_scroll_offset(2));
    if (!bad) printf("ok   scroll-offset: per container, last frame's until reopened\n");
}

int main(void)
{
    core_case("material light", case_material, 0);
    core_case("material dark", case_material, 1);
    for (int a = 0; a < 4; a++) core_case("material-rt", case_rt, a);
    core_case("backdrop light", case_backdrop, 0);
    core_case("backdrop dark", case_backdrop, 1);
    core_case("image", case_image, 0);
    core_case("image nocap", case_image, 1);
    core_case("elev light", case_elev, 0);
    core_case("elev dark", case_elev, 1);
    static const int pcts[4] = { 100, 125, 150, 200 };
    for (int i = 0; i < 4; i++) core_case("halo", case_halo, pcts[i]);
    core_case("theme-roles", case_roles, 0);
    core_case("scroll-offset", case_scroll, 0);
    if (core_failures) { printf("FAIL ui-aui-core-api: %d case(s) failed\n", core_failures); return 1; }
    printf("ok   ui-aui-core-api: all cases\n");
    return 0;
}
