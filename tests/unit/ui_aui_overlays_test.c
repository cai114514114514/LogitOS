/* ui_aui_overlays_test.c -- the menu panel, popover, sheet and alert of the
 * real ring-3 aui.c (make test-ui-aui-overlays; design 4.9, 4.10).
 *
 *   menu-snapshot  opening a menu: the animation frames draw NO material (the
 *                  plate is the solid inside one opacity group), the frame
 *                  that settles draws exactly ONE GLASS2, tier THICK, with the
 *                  menu tint, and so does every later repaint; the same for a
 *                  popover (THICK, popover tint)          FAIL menu-snapshot
 *   menu-anim      the open really animates (>= 2 frames) and goes quiet:
 *                  aui_anim_active / aui_anim_wait are 0 afterwards, and a
 *                  close fades out and goes quiet too       FAIL menu-anim
 *   menu-rt        Reduce Transparency: 0 GLASS2 on every frame and the
 *                  settled plate is the material's RT solid   FAIL menu-rt
 *   menu-motion    Reduce Motion: the first frame is settled  FAIL menu-motion
 *   menu-rows      row geometry (24 per row, 9 per separator, 6 padding),
 *                  width 220..360, separators and disabled rows have no aim,
 *                  the hover pill is one accent fill on the hovered row only
 *                                                            FAIL menu-rows
 *   menu-check     a leading U+2713 is ONE CHECK icon call, 12 px
 *                                                           FAIL menu-check
 *   menu-select    a select popup opens over the selected item, rows publish
 *                  popup-<i>                                FAIL menu-select
 *   menu-context   a context menu returns the chosen index once, closes, and
 *                  flips inside the window                 FAIL menu-context
 *   pop-geometry   popover place, flip, dismiss (outside click, Esc, not the
 *                  anchor, not the opening frame)             FAIL pop-geometry
 *   sheet          no title band (title at x + 20, 17 / 600, left), the plate
 *                  is one squircle fill, the rim, buttons right-aligned with
 *                  labels[0] rightmost, the scrim per mode    FAIL sheet-*
 *   alert          300 wide, two buttons side by side 126 wide / three
 *                  stacked, default and cancel semantics      FAIL alert-*
 *   aims           every aim is emitted once per change      FAIL overlay-aims
 *
 * THE CONTROL (-DUI_OVERLAYS_NEGCTL) makes the plate re-materialise on every
 * animation frame: the snapshot checks must report 'FAIL menu-snapshot' and
 * nothing else. Exits 1 on any failure. */
#include "ui_aui_core_hook.h"

/* ---- the hook: core_sys plus the icon and text-run recorders ---- */
#define OV_ICONS 16
#define OV_RUNS 16
static struct { int id, px; } ov_icon[OV_ICONS];
static int ov_nicon;
static struct { int x, y, px, weight; char s[16]; } ov_run[OV_RUNS];
static int ov_nrun;
#define OV_RR 24
static struct { int x, y, w, h, r; unsigned color; } ov_rr[OV_RR];      /* SYS_GUI_RRECT: opaque rounded fills */
static int ov_nrr;

long ov_sys(long n, long a, long b, long c)
{
    if (n == SYS_GUI_ICON) {
        if (ov_nicon < OV_ICONS) { ov_icon[ov_nicon].id = (int)(b >> 16); ov_icon[ov_nicon].px = (int)(b & 0xFFFF); }
        ov_nicon++;
    }
    if (n == SYS_GUI_RRECT) {
        if (ov_nrr < OV_RR) {
            ov_rr[ov_nrr].x = (int)((a >> 16) & 0xFFFF); ov_rr[ov_nrr].y = (int)(a & 0xFFFF);
            ov_rr[ov_nrr].w = (int)((b >> 16) & 0xFFFF); ov_rr[ov_nrr].h = (int)(b & 0xFFFF);
            ov_rr[ov_nrr].r = (int)((c >> 24) & 0xFF); ov_rr[ov_nrr].color = (unsigned)(c & 0xFFFFFF);
        }
        ov_nrr++;
    }
    if (n == SYS_GUI_TEXT_RUN2) {
        const struct logit_run2 *r = (const struct logit_run2 *)a;
        if (ov_nrun < OV_RUNS) {
            ov_run[ov_nrun].x = r->x; ov_run[ov_nrun].y = r->y; ov_run[ov_nrun].px = r->px;
            ov_run[ov_nrun].weight = r->weight;
            int k = r->len < 15 ? r->len : 15;
            memcpy(ov_run[ov_nrun].s, r->s, (size_t)k); ov_run[ov_nrun].s[k] = 0;
        }
        ov_nrun++;
    }
    return core_sys(n, a, b, c);
}

#define INFO_ALL (LOGIT_GUI2_F_GLASS2 | LOGIT_GUI2_F_PATH | LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_GROUP | \
                  LOGIT_GUI2_F_TEXT_RUN2 | LOGIT_GUI2_F_CLIP)

static unsigned T;
static void fr(void (*d)(void));
/* The latest position of every aim line seen since setup(): an aim is printed
 * only when it changes, so the frame that settles may print nothing new. */
static struct { char name[32]; int x, y; } AT[24];
static int nat;
static void scan_aims(void)
{
    for (const char *p = W.out; (p = strstr(p, "] aim ")); ) {
        char nm[32];
        int x, y, k = 0, j;
        p += 6;
        while (*p && *p != ' ' && k < 31) nm[k++] = *p++;
        nm[k] = 0;
        if (sscanf(p, "%d %d", &x, &y) != 2) continue;
        for (j = 0; j < nat; j++) if (!strcmp(AT[j].name, nm)) break;
        if (j == nat && nat < 24) { strcpy(AT[nat].name, nm); nat++; }
        if (j < 24) { AT[j].x = x; AT[j].y = y; }
    }
}
static void setup(int pct, int dark, int rt, int rm)
{
    W.pct = pct; W.dark = dark; W.reduce_t = rt; W.reduce_m = rm; W.info = INFO_ALL;
    aui_set_size(1000, 800);
    ui_aim_set_app("t");
    nat = 0;
    T = 1000;
    fr(0);        /* an idle frame first: draw_popup's animation slot is queried every frame, as in an app */
}

static void fr(void (*d)(void))
{
    T += 16; W.ms = T;
    ch_reset(); ov_nicon = 0; ov_nrun = 0; ov_nrr = 0;
    aui_begin(AUI_BG);
    if (d) d();
    aui_end();
    aui_feed_done();
    scan_aims();
}

static void ev(int type, int x, int y, int key)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = type == EV_KEY ? key : x; e.b = y;
    aui_feed(&e);
}

static int aim(const char *name, int *x, int *y)
{
    for (int j = 0; j < nat; j++)
        if (!strcmp(AT[j].name, name)) { *x = AT[j].x; *y = AT[j].y; return 1; }
    return 0;
}
static int naims(void)
{
    int n = 0;
    for (const char *p = W.out; (p = strstr(p, "] aim ")); p++) n++;
    return n;
}
static int count_rgb(unsigned rgb)       /* PATH paints of that colour */
{
    int n = 0;
    for (int i = 0; i < W.npath && i < CH_LOG; i++) if ((W.paint[i].argb & 0xFFFFFFu) == rgb && W.paint[i].kind == LOGIT_PAINT2_SOLID) n++;
    return n;
}
static int count_rr(unsigned rgb, int *which)     /* opaque rounded fills of that colour */
{
    int n = 0;
    for (int i = 0; i < ov_nrr && i < OV_RR; i++) if (ov_rr[i].color == rgb) { if (!n && which) *which = i; n++; }
    return n;
}
static int has_argb(unsigned argb)
{
    for (int i = 0; i < W.npath && i < CH_LOG; i++) if (W.paint[i].argb == argb && W.paint[i].kind == LOGIT_PAINT2_SOLID) return 1;
    return 0;
}

/* Frames until nothing animates; sums the material calls of the moving frames. */
static int anim_frames, anim_glass, settle_glass;
static void run_open(void (*d)(void))
{
    anim_frames = anim_glass = 0;
    settle_glass = -1;
    for (int i = 0; i < 80; i++) {
        fr(d);
        if (aui_anim_active()) { anim_frames++; anim_glass += W.nglass; }
        else { settle_glass = W.nglass; return; }
    }
}

/* ------------------------------------------------------------------ menus */
static const char *const CTX[] = {
    "新建\t⌘N", "-", "✓ 显示隐藏文件", AUI_MI_DESTRUCTIVE "删除\t⌘⇧⌫", AUI_MI_DISABLED "不可用",
};
#define NCTX 5
static int g_open, g_got, g_ctx_x = 100, g_ctx_y = 120;
static void draw_ctx(void)
{
    int r;
    if (g_open) { r = aui_context_menu(g_ctx_x, g_ctx_y, CTX, NCTX); g_open = 0; }
    else r = aui_context_menu(-1, -1, CTX, NCTX);
    if (r >= 0) g_got = r;
}

static void case_menu(int pct)
{
    const struct logit_material *m = logit_material(LOGIT_MAT_MENU);
    int dark = pct >= 1000, x, y, x0;
    if (dark) pct -= 1000;
    setup(pct, dark, 0, 0);
    g_open = 1; g_got = -1;
    run_open(draw_ctx);
    if (anim_frames < 2) FAIL("menu-anim: %d animation frames, want >= 2", anim_frames);
    if (anim_glass != 0) FAIL("menu-snapshot: %d material calls over %d animation frames, want 0", anim_glass, anim_frames);
    if (settle_glass != 1) FAIL("menu-snapshot: %d GLASS2 on the settled frame, want 1", settle_glass);
    else {
        const struct logit_glass2 *g = &W.glass[0];
        unsigned tint = dark ? m->tint_d : m->tint_l;
        if (g->blur_level != m->tier || g->blur_level != LOGIT_TIER_THICK || g->tint_argb != (tint & 0xFFFFFFu) ||
            g->opacity != (int)(tint >> 24))
            FAIL("menu-snapshot: glass tier %d tint %06x opacity %d, want THICK %06x %d", g->blur_level, g->tint_argb,
                 g->opacity, tint & 0xFFFFFFu, (int)(tint >> 24));
    }
    if (naims() != 3) FAIL("overlay-aims: %d aims on the settled menu, want 3 (the enabled rows)", naims());
    /* geometry from the aims */
    if (!aim("context-0", &x, &y)) FAIL("menu-rows: no context-0 aim");
    else {
        int w = 2 * (x - g_ctx_x), y2, y3, y4, x2;
        x0 = x;
        if (w < 220 || w > 360) FAIL("menu-rows: width %d, want 220..360", w);
        if (y != g_ctx_y + 6 + 12) FAIL("menu-rows: row 0 centre %d, want %d", y, g_ctx_y + 18);
        if (!aim("context-2", &x2, &y2) || y2 != g_ctx_y + 6 + 24 + 9 + 12 || x2 != x0)
            FAIL("menu-rows: row 2 centre %d, want %d (24 rows, 9 separator)", y2, g_ctx_y + 51);
        if (!aim("context-3", &x2, &y3) || y3 != y2 + 24) FAIL("menu-rows: row 3 centre %d, want %d", y3, y2 + 24);
        if (aim("context-1", &x2, &y4) || aim("context-4", &x2, &y4))
            FAIL("menu-rows: a separator or a disabled row has an aim");
        /* the pill: none before hover, one accent fill on hover */
        if (count_rr(aui_tok()->accent, 0) != 0) FAIL("menu-rows: an accent fill with nothing hovered");
        ev(EV_MOUSE_MOVE, x0, y, 0);
        fr(draw_ctx);
        {
            int which = -1, nacc = count_rr(aui_tok()->accent, &which);
            if (nacc != 1) FAIL("menu-rows: %d accent fills on hover, want 1 (the pill)", nacc);
            else if (ov_rr[which].x != g_ctx_x + 5 || ov_rr[which].w != 2 * (x0 - g_ctx_x) - 10 ||
                     ov_rr[which].y != g_ctx_y + 6 || ov_rr[which].h != 24 || ov_rr[which].r != 7)
                FAIL("menu-rows: pill at (%d, %d) %dx%d r%d, want (%d, %d) %dx24 r7: inset 5, radius 7",
                     ov_rr[which].x, ov_rr[which].y, ov_rr[which].w, ov_rr[which].h, ov_rr[which].r,
                     g_ctx_x + 5, g_ctx_y + 6, 2 * (x0 - g_ctx_x) - 10);
        }
        if (W.nglass != 1) FAIL("menu-snapshot: %d GLASS2 on a repaint of the open menu, want 1", W.nglass);
        /* the CHECK icon */
        {
            int nc = 0, bad = 0;
            for (int i = 0; i < ov_nicon && i < OV_ICONS; i++) if (ov_icon[i].id == GICON_CHECK) { nc++; if (ov_icon[i].px != 12) bad = 1; }
            if (nc != 1 || bad) FAIL("menu-check: %d CHECK icon calls (px ok %d), want 1 at 12", nc, !bad);
            for (int i = 0; i < ov_nrun && i < OV_RUNS; i++)
                if (strstr(ov_run[i].s, "\xE2\x9C\x93")) FAIL("menu-check: U+2713 drawn as text");
        }
        /* the shortcut cells: modifiers and key are separate runs */
        {
            int found = 0;
            for (int i = 0; i < ov_nrun && i < OV_RUNS; i++) if (!strcmp(ov_run[i].s, "N") && ov_run[i].px == 13 * 256) found = 1;
            if (!found) FAIL("menu-rows: the shortcut key 'N' is not its own callout-13 run");
        }
        /* aims once per change: an identical frame prints none */
        fr(draw_ctx);
        if (naims() != 0) FAIL("overlay-aims: %d aims on an unchanged frame, want 0", naims());
        /* choose row 0 by its aim: returned once, then closed and quiet */
        ev(EV_MOUSE, x0, y, 0);
        fr(draw_ctx);
        fr(draw_ctx);
        if (g_got != 0) FAIL("menu-context: chose row 0, got %d", g_got);
        g_got = -1;
        for (int i = 0; i < 40; i++) { fr(draw_ctx); if (!aui_anim_active()) break; }
        if (aui_anim_active() || aui_anim_wait() != 0) FAIL("menu-anim: the close is still requesting frames");
        if (W.nglass != 0) FAIL("menu-anim: %d GLASS2 after the menu closed, want 0", W.nglass);
        if (g_got != -1) FAIL("menu-context: the choice was returned twice");
    }
    if (!bad) printf("ok   menu: %d%% %s -- snapshot, rows, hover pill, check, aims\n", pct, dark ? "dark" : "light");
}

static void case_menu_rt(int arg)
{
    const struct logit_material *m = logit_material(LOGIT_MAT_MENU);
    int dark = arg & 1, noglass = arg & 2;
    unsigned rt = dark ? m->rt_d : m->rt_l;
    setup(100, dark, 1, 0);
    if (noglass) W.info &= ~LOGIT_GUI2_F_GLASS2;
    g_open = 1;
    int glass = 0;
    for (int i = 0; i < 60; i++) { fr(draw_ctx); glass += W.nglass; if (!aui_anim_active()) break; }
    if (glass != 0) FAIL("menu-rt: %d GLASS2 under Reduce Transparency, want 0", glass);
    if (!has_argb(rt)) FAIL("menu-rt: no solid of the RT colour %08x on the settled plate", rt);
    if (!bad) printf("ok   menu-rt: %s%s -- 0 GLASS2, the RT solid\n", dark ? "dark" : "light", noglass ? " no GLASS2" : "");
}

static void case_menu_motion(int dark)
{
    setup(100, dark, 0, 1);
    g_open = 1;
    fr(draw_ctx);
    fr(draw_ctx);
    if (aui_anim_active() || W.nglass != 1) FAIL("menu-motion: Reduce Motion left %d GLASS2 and active=%d, want 1 and 0", W.nglass, aui_anim_active());
    if (!bad) printf("ok   menu-motion: Reduce Motion settles at once\n");
}

static const char *const SEL[] = { "一", "二", "三", "四" };
static int g_sel = 2, g_chg;
static void draw_sel(void)
{
    aui_aim_prefix("sel");
    g_chg = aui_select(40, 100, 160, SEL, 4, &g_sel);
}
static void case_select(int pct)
{
    int x, y;
    setup(pct, 0, 0, 0);
    g_sel = 2;
    fr(draw_sel);
    if (!aim("sel", &x, &y)) { FAIL("menu-select: no select aim"); return; }
    ev(EV_MOUSE, x, y, 0);
    fr(draw_sel);
    for (int i = 0; i < 60; i++) { fr(draw_sel); if (!aui_anim_active()) break; }
    {
        int px, py;
        if (!aim("popup-2", &px, &py)) FAIL("menu-select: no popup-2 aim");
        else if (py != 100 + 14 || px != 40 + 80)
            FAIL("menu-select: the selected row centres at (%d, %d), want the control's (120, 114)", px, py);
    }
    ev(EV_MOUSE, 120, 114 + 24, 0);          /* row 3 */
    fr(draw_sel);
    fr(draw_sel);
    if (g_sel != 3) FAIL("menu-select: chose row 3, selection is %d", g_sel);
    if (!bad) printf("ok   menu-select: opens over the selected item, popup-<i> aims\n");
}

static void case_flip(int unused)
{
    int x, y;
    (void)unused;
    setup(100, 0, 0, 0);
    g_ctx_x = 900; g_ctx_y = 700; g_open = 1;      /* near the corner: must stay inside */
    run_open(draw_ctx);
    if (!aim("context-0", &x, &y)) { FAIL("menu-context: no aim at the corner"); return; }
    if (y - 18 + 117 > 798 || y - 18 < 2) FAIL("menu-context: row 0 at y %d leaves the window (panel h 117)", y);
    if (x > 998 - 110) FAIL("menu-context: the menu leaves the window on the right (centre x %d)", x);
    g_ctx_x = 100; g_ctx_y = 120;
    if (!bad) printf("ok   menu-context: stays inside the window\n");
}

/* ---------------------------------------------------------------- popover */
static struct aui_rect g_anchor;
static int g_pw = 240, g_ph = 120, g_edge, g_psel, g_pon;
static void draw_pop(void)
{
    if (!g_pon) return;
    aui_popover_begin(g_anchor, g_pw, g_ph, g_edge);
    aui_aim_prefix("p");
    aui_select(0, 0, 100, SEL, 4, &g_psel);
    aui_popover_end();
}
static void case_popover(int pct)
{
    const struct logit_material *m = logit_material(LOGIT_MAT_POPOVER);
    int x, y;
    setup(pct, 0, 0, 0);
    g_anchor = aui_r(100, 100, 80, 28); g_edge = AUI_EDGE_BOTTOM; g_pon = 1;
    run_open(draw_pop);
    if (anim_glass != 0) FAIL("menu-snapshot: popover drew %d material calls over %d animation frames, want 0", anim_glass, anim_frames);
    if (settle_glass != 1 || W.glass[0].blur_level != LOGIT_TIER_THICK || W.glass[0].tint_argb != (m->tint_l & 0xFFFFFFu))
        FAIL("menu-snapshot: popover settled with %d GLASS2 (tier %d), want 1 THICK popover tint", settle_glass, W.glass[0].blur_level);
    if (!aim("p", &x, &y) || x != 20 + 16 + 50 || y != 142 + 16 + 14)
        FAIL("pop-geometry: content aim (%d, %d), want (86, 172): below the anchor, 6 + 8 away, padding 16", x, y);
    /* an unchanged frame is quiet; the opening frame never dismisses */
    if (aui_popover_dismissed()) FAIL("pop-geometry: dismissed on open");
    /* clicks: inside, on the anchor -> stay; outside -> dismissed */
    ev(EV_MOUSE, 100, 200, 0); fr(draw_pop);
    if (aui_popover_dismissed()) FAIL("pop-geometry: dismissed by a click inside");
    ev(EV_MOUSE, 120, 110, 0); fr(draw_pop);
    if (aui_popover_dismissed()) FAIL("pop-geometry: dismissed by a click on the anchor");
    ev(EV_MOUSE, 700, 500, 0); fr(draw_pop);
    if (!aui_popover_dismissed()) FAIL("pop-geometry: an outside click did not dismiss");
    if (aui_popover_dismissed()) FAIL("pop-geometry: the dismissal flag was not cleared by the read");
    ev(EV_KEY, 0, 0, 27); fr(draw_pop);
    if (!aui_popover_dismissed()) FAIL("pop-geometry: Escape did not dismiss");
    if (!bad) printf("ok   popover: %d%% snapshot, place, dismiss\n", pct);
}
static void case_popover_flip(int unused)
{
    int x, y;
    (void)unused;
    setup(100, 0, 0, 0);
    g_anchor = aui_r(100, 760, 80, 28); g_edge = AUI_EDGE_BOTTOM; g_pon = 1;
    run_open(draw_pop);
    if (!aim("p", &x, &y) || y != 760 - 14 - 120 + 16 + 14)
        FAIL("pop-geometry: flipped aim (%d, %d), want y %d (above the anchor)", x, y, 760 - 14 - 120 + 30);
    g_anchor = aui_r(100, 300, 80, 28); g_edge = AUI_EDGE_RIGHT;
    T += 500; run_open(draw_pop);
    if (!aim("p", &x, &y) || x != 180 + 14 + 16 + 50)
        FAIL("pop-geometry: right-edge aim x %d, want %d", x, 180 + 14 + 16 + 50);
    if (!bad) printf("ok   popover: flips and opens to the right\n");
}
static void case_popover_rt(int unused)
{
    int glass = 0;
    (void)unused;
    setup(100, 0, 1, 0);
    g_anchor = aui_r(100, 100, 80, 28); g_edge = AUI_EDGE_BOTTOM; g_pon = 1;
    for (int i = 0; i < 60; i++) { fr(draw_pop); glass += W.nglass; if (!aui_anim_active()) break; }
    if (glass) FAIL("menu-rt: popover drew %d GLASS2 under Reduce Transparency", glass);
    if (!bad) printf("ok   popover-rt: 0 GLASS2\n");
}

/* ------------------------------------------------------------------ sheet */
static const char *const DL[] = { "好", "取消" };
static int g_dres;
static void draw_sheet(void)
{
    aui_dialog_begin("标题", 420, 200);
    g_dres = aui_dialog_buttons(DL, 2);
    aui_dialog_end();
}
static void case_sheet(int dark)
{
    int x0, y0, x1, y1, x2, y2;
    unsigned raised;
    setup(100, dark, 0, 0);
    raised = aui_tok()->bg_raised;
    fr(draw_sheet);                       /* the opening frame: the title run, the plate, the rim */
    {
        int title = -1;
        for (int i = 0; i < ov_nrun && i < OV_RUNS; i++) if (!strcmp(ov_run[i].s, "标题")) { title = i; break; }
        if (title < 0) FAIL("sheet-title: no title run");
        else if (ov_run[title].x < (290 + 20) * 256 || ov_run[title].x > (290 + 22) * 256 || ov_run[title].px != 17 * 256 ||
                 ov_run[title].weight != 600)
            FAIL("sheet-title: x %d px %d weight %d, want x %d (left, padding 20) 17 / 600",
                 ov_run[title].x >> 8, ov_run[title].px >> 8, ov_run[title].weight, 290 + 20);
    }
    if (count_rgb(raised) != 1) FAIL("sheet-plate: %d bg.raised fills, want 1 (no title band, no second plate)", count_rgb(raised));
    {
        int rim = 0;
        for (int i = 0; i < W.npath && i < CH_LOG; i++)
            if (dark ? W.paint[i].kind == LOGIT_PAINT2_LINEAR : (W.paint[i].kind == LOGIT_PAINT2_SOLID && W.paint[i].argb == 0x14141428u)) rim++;
        if (rim != 1) FAIL("sheet-rim: %d rim strokes (%s), want 1", rim, dark ? "dark LINEAR" : "light 141428@20");
    }
    for (int i = 0; i < 40; i++) { fr(draw_sheet); if (!aui_anim_active()) break; }
    fr(draw_sheet);
    if (aui_anim_active() || aui_anim_wait() != 0) FAIL("sheet-motion: the settled sheet still requests frames");
    if (!has_argb((dark ? 89u : 46u) << 24)) FAIL("sheet-scrim: no scrim of alpha %d on the settled sheet", dark ? 89 : 46);
    ev(EV_MOUSE_MOVE, 5, 5, 0); fr(draw_sheet);                  /* nothing changed: no aims */
    if (naims() != 0) FAIL("overlay-aims: %d dialog aims on an unchanged frame, want 0", naims());
    if (!aim("dialog-0", &x0, &y0) || !aim("dialog-1", &x1, &y1) || !aim("dialog-confirm", &x2, &y2) ||
        x2 != x0 || y2 != y0 || !aim("dialog-cancel", &x2, &y2) || x2 != x1 || y2 != y1)
        FAIL("sheet-buttons: dialog aims missing or confirm/cancel are not 0 / n-1");
    else {
        int by = (800 - 200 - 40) / 3 + 40 + 200 - 28 - 20 + 14;
        if (y0 != by || y1 != by) FAIL("sheet-buttons: centre y %d / %d, want %d (h 28, padding 20)", y0, y1, by);
        if (x0 <= x1) FAIL("sheet-buttons: labels[0] at x %d is not right of labels[1] at x %d", x0, x1);
        if (690 - x0 < 36) FAIL("sheet-buttons: the rightmost button's right edge is not at padding 20 (centre %d)", x0);
        if (x0 - x1 - 8 - (690 - x0) < 36) FAIL("sheet-buttons: buttons are closer than 8 apart");
    }
    /* Enter picks 0, Escape the last */
    ev(EV_KEY, 0, 0, '\n'); fr(draw_sheet);
    if (g_dres != 0) FAIL("sheet-buttons: Enter returned %d, want 0", g_dres);
    ev(EV_KEY, 0, 0, 27); fr(draw_sheet);
    if (g_dres != 1) FAIL("sheet-buttons: Escape returned %d, want 1", g_dres);
    if (!bad) printf("ok   sheet: %s -- no band, plate, rim, buttons, scrim\n", dark ? "dark" : "light");
}

/* ------------------------------------------------------------------ alert */
static const char *const AL2[] = { "删除", "取消" };
static const char *const AL3[] = { "存储", "不存储", "取消" };
static int g_ares, g_alert_n = 2, g_alert_flags;
static void draw_alert(void)
{
    g_ares = aui_alert(0, "标题", "", g_alert_n == 2 ? AL2 : AL3, g_alert_n, g_alert_flags);
}
static void settle_alert(void)
{
    for (int i = 0; i < 60; i++) { fr(draw_alert); if (!aui_anim_active()) break; }
    fr(draw_alert);
    if (naims() != 0) FAIL("overlay-aims: %d alert aims on an unchanged frame, want 0", naims());
}
static void case_alert(int dark)
{
    int x0, y0, x1, y1, x2, y2, x3, y3;
    setup(100, dark, 0, 0);
    g_alert_n = 2; g_alert_flags = 0;
    settle_alert();
    if (!aim("alert-0", &x0, &y0) || !aim("alert-1", &x1, &y1) || !aim("alert-confirm", &x2, &y2) ||
        !aim("alert-cancel", &x3, &y3) || x2 != x0 || y2 != y0 || x3 != x1 || y3 != y1)
        FAIL("alert-aims: alert-<i> / alert-confirm / alert-cancel missing or mismatched");
    else {
        if (x0 - x1 != 126 + 8) FAIL("alert-buttons: centres %d apart, want 134 (two 126 buttons, 8 gap)", x0 - x1);
        if (x1 != 350 + 20 + 63) FAIL("alert-buttons: left button centre %d, want %d (width 126, padding 20)", x1, 350 + 20 + 63);
        if (y0 != y1 || y0 != 274 + 20 + 22 + 20 + 16) FAIL("alert-buttons: centre y %d / %d, want %d (h 32, side by side)", y0, y1, 274 + 78);
    }
    ev(EV_MOUSE, x0, y0, 0); fr(draw_alert);
    if (g_ares != 0) FAIL("alert-buttons: clicking alert-0 returned %d, want 0", g_ares);
    aui_set_focus(0);
    ev(EV_KEY, 0, 0, '\n'); fr(draw_alert);
    if (g_ares != 0) FAIL("alert-default: Enter returned %d, want 0 (the primary)", g_ares);
    ev(EV_KEY, 0, 0, 27); fr(draw_alert);
    if (g_ares != 1) FAIL("alert-default: Escape returned %d, want 1 (the cancel)", g_ares);
    g_alert_flags = AUI_ALERT_DESTRUCTIVE;
    aui_set_focus(0);
    ev(EV_KEY, 0, 0, '\n'); fr(draw_alert);
    if (g_ares != 1) FAIL("alert-default: Enter on a destructive alert returned %d, want 1 (Cancel)", g_ares);
    /* three buttons stack, full width, 8 apart */
    g_alert_n = 3; g_alert_flags = 0;
    nat = 0; T += 1000; settle_alert();
    if (!aim("alert-0", &x0, &y0) || !aim("alert-1", &x1, &y1) || !aim("alert-2", &x2, &y2))
        FAIL("alert-aims: three-button aims missing");
    else {
        if (x0 != 500 || x1 != 500 || x2 != 500) FAIL("alert-buttons: stacked centres at x %d %d %d, want 500 (full width 260)", x0, x1, x2);
        if (y1 - y0 != 40 || y2 - y1 != 40) FAIL("alert-buttons: stacked pitch %d / %d, want 40 (32 + 8)", y1 - y0, y2 - y1);
        if (y0 != 242 + 20 + 22 + 20 + 16) FAIL("alert-buttons: primary on top at y %d, want %d", y0, 242 + 78);
    }
    if (!bad) printf("ok   alert: %s -- 300 wide, 126 buttons, stack, semantics\n", dark ? "dark" : "light");
}

static void case_scrim(int dark)
{
    setup(100, dark, 0, 0);
    g_alert_n = 2; g_alert_flags = 0;
    for (int i = 0; i < 60; i++) { fr(draw_alert); if (!aui_anim_active()) break; }
    fr(draw_alert);
    if (!has_argb((dark ? 89u : 46u) << 24)) FAIL("alert-scrim: no scrim of alpha %d over the owner window", dark ? 89 : 46);
    if (!bad) printf("ok   alert-scrim: %d\n", dark ? 89 : 46);
}

int main(void)
{
    static const int pcts[3] = { 100, 125, 150 };
    for (int i = 0; i < 3; i++) core_case("menu", case_menu, pcts[i]);
    core_case("menu dark", case_menu, 1100);
    for (int a = 0; a < 4; a++) core_case("menu-rt", case_menu_rt, a);
    core_case("menu-motion", case_menu_motion, 0);
    core_case("menu-select", case_select, 100);
    core_case("menu-select", case_select, 150);
    core_case("menu-flip", case_flip, 0);
    core_case("popover", case_popover, 100);
    core_case("popover", case_popover, 125);
    core_case("popover", case_popover, 150);
    core_case("popover-flip", case_popover_flip, 0);
    core_case("popover-rt", case_popover_rt, 0);
    core_case("sheet light", case_sheet, 0);
    core_case("sheet dark", case_sheet, 1);
    core_case("alert light", case_alert, 0);
    core_case("alert dark", case_alert, 1);
    core_case("alert-scrim", case_scrim, 0);
    core_case("alert-scrim", case_scrim, 1);
    if (core_failures) { printf("FAIL ui-aui-overlays: %d case(s) failed\n", core_failures); return 1; }
    printf("ok   ui-aui-overlays: all cases\n");
    return 0;
}
