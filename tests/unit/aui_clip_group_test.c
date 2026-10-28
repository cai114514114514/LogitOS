/* aui-clip-group-adopt: rounded clips and opacity groups in the ring-3 toolkit.
 * Drives the REAL aui.c (included, so its statics -- the clip / fade stacks, the
 * leak counter -- are reachable) over the host harness with tests/unit/aui_host/
 * v2_state.c, which serves CLIP_PUSH/POP, GROUP_BEGIN/END and BLIT2 through the
 * REAL c/kernel/gui/wm_draw2_state.c. Failures print "FAIL <check>"; the exit
 * status is non-zero on any.
 *
 *   fade-group   a disabled button over a gradient is ONE group begin/end at
 *                AUI_DISABLED_ALPHA, which is 102 (40 %, design 4: was 110),
 *                and its pixels equal the display_group oracle (the enabled
 *                look drawn inside ol_display_group_begin/end at 102)
 *   balance      pushes and pops pair up per frame; a leak is counted by aui and
 *                the kernel's auto-close counter (SYS_GUI2_INFO a=1) stays 0
 *   scroll       a rounded scroll container clips its corners at 100/125/150%
 *   limit        OL_LIMIT from the kernel falls back to today's drawing
 *   nest         8 scroll containers nest; the clip stack never overruns
 *   damage       an alpha-only change of a group still reaches the flush rect
 *
 * -DAUI_FADE_LEGACY_MIX makes aui_fade_* ignore the kernel's GROUP bit; the
 * control build runs only the fade-group check and must fail it ('FAIL
 * fade-group') and nothing else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aui.c"
#include "aui_host.h"
#include "openlogit_display_group.h"

extern int v2_state_limit_group, v2_state_limit_clip;

static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }
static void checkp(int ok, const char *fmt, int a, int b)
{ char buf[160]; snprintf(buf, sizeof buf, fmt, a, b); check(ok, buf); }

static struct aui_host *H;
static void setup(int pct, int v2)
{
    aui_host_handlers_clear();
    H = aui_host_new(160, 120, pct);
    sc_pct = pct;
    if (v2) aui_host_enable_v2(H);
    aui_host_clock_set(1000);
    aui_begin(0x202020);
    aui_host_stats_reset(H);
}
static void teardown(void) { aui_end(); aui_feed_done(); aui_host_free(H); H = 0; }
static unsigned long calls(long n) { return (unsigned long)H->calls[n]; }
static unsigned pxl(int x, int y) { return H->px[(size_t)y * (size_t)H->w + (size_t)x] & 0xFFFFFF; }

/* ------------------------------------------------------------ fade-group */

#define BX 20
#define BY 40
#define BW 80
#define BH 28

static void gradient(void) { aui_vgrad_round(0, 0, 160, 120, 0, 0x3060C0, 0xC06030); }

static void t_fade_group(void)
{
    /* design 4 ("Disabled is always the existing opacity group at
     * AUI_DISABLED_ALPHA, changed from 110 to 102 (40 %)") */
    check(AUI_DISABLED_ALPHA == 102, "fade-group: AUI_DISABLED_ALPHA is 102 (design 4)");
    setup(100, 1);
    gradient();
    aui_host_stats_reset(H);
    aui_button_ex(BX, BY, BW, BH, "Off", AUI_V_PRIMARY, 0);
    check(calls(SYS_GUI_GROUP_BEGIN) == 1 && calls(SYS_GUI_GROUP_END) == 1,
          "fade-group: a disabled button is exactly one group begin/end");
    size_t n = (size_t)H->w * (size_t)H->h;
    uint32_t *got = malloc(n * 4);
    memcpy(got, H->px, n * 4);
    teardown();

    /* the oracle: the enabled look inside a display_group at the same opacity */
    setup(100, 0);
    gradient();
    int rad[4] = { 0, 0, 0, 0 };
    /* design 4.3 (ui-aui-buttons): the disabled group also covers the 8 pt E1
     * shadow margin around the capsule */
    unsigned long bytes = ol_display_group_bytes(BW + 16, BH + 16, rad, AUI_DISABLED_ALPHA);
    uint32_t *snap = malloc(bytes);
    struct ol_display_group g;
    int rc = ol_display_group_begin(&H->disp, &g, BX - 8, BY - 8, BW + 16, BH + 16, rad, AUI_DISABLED_ALPHA, snap, bytes);
    aui_button_ex(BX, BY, BW, BH, "Off", AUI_V_PRIMARY, 1);
    ol_display_group_end(&H->disp, &g);
    check(rc == 0 && !memcmp(got, H->px, n * 4), "fade-group: pixels equal the display_group oracle");
    int differs = 0;
    for (int y = BY; y < BY + BH && !differs; y++)
        for (int x = BX + 4; x < BX + BW - 4; x++) if (pxl(x, y) != 0) { differs = 1; break; }
    check(differs, "fade-group: the oracle drew something");
    teardown();
    free(got); free(snap);
}

#ifndef AUI_FADE_LEGACY_MIX

/* the disabled look in INFO = 0 is flat and unchanged, and the gradient does NOT show through */
static void t_disabled_legacy(void)
{
    setup(100, 0);
    gradient();
    unsigned before = pxl(BX + 40, BY + 3);
    aui_button_ex(BX, BY, BW, BH, "Off", AUI_V_PRIMARY, 0);
    check(calls(SYS_GUI_GROUP_BEGIN) == 0, "legacy: no group call without the GROUP bit");
    check(pxl(BX + 40, BY + 3) != before, "legacy: the disabled face is drawn");
    teardown();
}

/* ------------------------------------------------------------- balance */

static void busy_frame(int rounded)
{
    static int off1, off2;
    struct aui_rect r = aui_r(10, 10, 140, 100);
    int k = aui_clip_push_round(r, 10);
    check(k == rounded, "balance: aui_clip_push_round reports the kernel rounding");
    aui_fill(0, 0, 160, 120, 0x00AA00);
    aui_fade_begin(aui_r(20, 20, 60, 40), 128);
    aui_fill(25, 25, 40, 20, 0xFF0000);
    aui_clip_push_round(aui_r(30, 30, 20, 20), 4);
    aui_fill(0, 0, 160, 120, 0x0000FF);
    aui_clip_pop();
    aui_fade_end();
    aui_clip_pop();
    aui_scroll_begin_round(10, 10, 100, 60, &off1, 200, 8);
    aui_scroll_begin(5, 5, 60, 30, &off2, 100);
    aui_fill(0, 0, 40, 40, 0x808080);
    aui_scroll_end();
    aui_scroll_end();
    if (aui_dialog_begin("Sheet", 100, 50)) {
        aui_label(4, 4, "body", AUI_TEXT);
        aui_dialog_end();
    }
}

static void t_balance(void)
{
    setup(100, 1);
    cg_leaks = 0;
    busy_frame(1);
    aui_end();
    check(cg_leaks == 0, "balance: a balanced frame leaks no level");
    check(ol_gui2_info(1) == 0, "balance: the kernel auto-closed nothing");
    check(calls(SYS_GUI_CLIP_PUSH) == calls(SYS_GUI_CLIP_POP) && calls(SYS_GUI_CLIP_PUSH) > 0,
          "balance: clip push/pop counts match");
    check(calls(SYS_GUI_GROUP_BEGIN) == calls(SYS_GUI_GROUP_END) && calls(SYS_GUI_GROUP_BEGIN) > 0,
          "balance: group begin/end counts match");
    check(clipn == 0 && faden == 0 && cgn == 0, "balance: aui's stacks are empty");

    /* a frame that leaks: counted, closed before the flush, so the kernel never has to */
    aui_begin(0x202020);
    aui_clip_push_round(aui_r(10, 10, 50, 50), 6);
    aui_fade_begin(aui_r(10, 10, 50, 50), 100);
    aui_fill(0, 0, 160, 120, 0xFFFFFF);
    aui_end();
    check(cg_leaks == 1, "balance: a leaked frame is detected");
    check(ol_gui2_info(1) == 0, "balance: ...and aui closes it before the flush");
    check(calls(SYS_GUI_CLIP_PUSH) == calls(SYS_GUI_CLIP_POP) &&
          calls(SYS_GUI_GROUP_BEGIN) == calls(SYS_GUI_GROUP_END), "balance: ...in kernel order");
    aui_begin(0x202020);
    cg_leaks = 0;
    teardown();

    setup(100, 0);
    busy_frame(0);
    aui_end();
    check(cg_leaks == 0 && calls(SYS_GUI_CLIP_PUSH) == 0 && calls(SYS_GUI_GROUP_BEGIN) == 0,
          "balance: INFO = 0 issues no v2 call and stays balanced");
    aui_begin(0x202020);
    teardown();
}

/* ------------------------------------------------------- rounded scroll */

static void scroll_corner(int pct, int radius, int expect_cut)
{
    setup(pct, 1);
    int off = 0;
    aui_host_stats_reset(H);
    if (radius) aui_scroll_begin_round(20, 20, 100, 60, &off, 200, radius);
    else aui_scroll_begin(20, 20, 100, 60, &off, 200);
    aui_fill(0, 0, 100, 200, 0xFF0000);
    aui_scroll_end();
    int x0 = aui_host_pt(20, pct), y0 = aui_host_pt(20, pct);
    int x1 = aui_host_pt(20 + 88, pct) - 1, y1 = aui_host_pt(20 + 60, pct) - 1;   /* bar gutter: 12 pt */
    int cut = pxl(x0, y0) == 0x202020 && pxl(x0, y1) == 0x202020;
    int mid = pxl((x0 + x1) / 2, (y0 + y1) / 2) == 0xFF0000;
    int edge = pxl(x0 + aui_host_pt(30, pct), y0) == 0xFF0000;       /* the straight part of the top edge */
    char w[96];
    snprintf(w, sizeof w, "scroll: %d%% r=%d corners %s", pct, radius, expect_cut ? "cut" : "square");
    check(cut == expect_cut && mid && edge, w);
    if (radius) check(calls(SYS_GUI_CLIP_PUSH) == 1, "scroll: the rounding is one kernel clip level");
    teardown();
}

static void t_scroll(void)
{
    static const int pcts[3] = { 100, 125, 150 };
    for (int i = 0; i < 3; i++) {
        scroll_corner(pcts[i], 14, 1);
        scroll_corner(pcts[i], 0, 0);
    }
}

/* ------------------------------------------------------------- fallback */

static void t_limit(void)
{
    /* the group refused: today's flat disabled look, pixel for pixel */
    setup(100, 1);
    v2_state_limit_group = 1;
    gradient();
    aui_host_stats_reset(H);
    aui_button_ex(BX, BY, BW, BH, "Off", AUI_V_PRIMARY, 0);
    check(calls(SYS_GUI_GROUP_BEGIN) == 1 && calls(SYS_GUI_GROUP_END) == 0,
          "limit: OL_LIMIT is tried once and no end is issued");
    size_t n = (size_t)H->w * (size_t)H->h;
    uint32_t *lim = malloc(n * 4);
    memcpy(lim, H->px, n * 4);
    teardown();
    check(faden == 0 && cgn == 0, "limit: balanced after the frame");
    setup(100, 0);
    gradient();
    aui_button_ex(BX, BY, BW, BH, "Off", AUI_V_PRIMARY, 0);
    check(!memcmp(lim, H->px, n * 4), "limit: the fallback equals the INFO = 0 drawing");
    teardown();
    free(lim);

    /* the popup fade: the same refusal, balanced, mixed colours */
    setup(100, 1);
    v2_state_limit_group = 1;
    static const char *items[3] = { "One", "Two", 0 };
    static int sel;
    for (int f = 0; f < 3; f++) {
        if (f) aui_begin(0x202020);
        if (f) aui_host_clock_advance(f == 1 ? 1 : 40);
        pop.kind = f ? 1 : 0; pop.owner = 7; pop.x = 20; pop.y = 20; pop.w = 80;
        pop.itemh = 26; pop.n = 2; pop.items = items; pop.sel = &sel; pop.hi = -1;
        if (f < 2) aui_end();
    }
    aui_end();
    unsigned long tries = calls(SYS_GUI_GROUP_BEGIN);
    check(tries >= 1, "limit: the popup fade tried a group");
    check(calls(SYS_GUI_GROUP_END) == 0 && ol_gui2_info(1) == 0, "limit: a refused popup fade stays balanced");
    teardown();

    /* the clip refused: the square clip still applies (gui_clip) */
    setup(100, 1);
    v2_state_limit_clip = 1;
    int k = aui_clip_push_round(aui_r(10, 10, 50, 50), 12);
    aui_fill(0, 0, 160, 120, 0xFF0000);
    aui_clip_pop();
    check(k == 0 && calls(SYS_GUI_CLIP_PUSH) == 1 && calls(SYS_GUI_CLIP_POP) == 0,
          "limit: a refused clip is square and popped locally");
    check(pxl(5, 5) == 0x202020 && pxl(30, 30) == 0xFF0000 && pxl(70, 70) == 0x202020,
          "limit: ...with the square clip applied");
    teardown();
}

/* ----------------------------------------------------------- nesting */

static void t_nest(void)
{
    setup(100, 1);
    int off[10] = { 0 };
    for (int i = 0; i < 9; i++) aui_scroll_begin(2 + i, 2 + i, 140 - 2 * i, 100 - 2 * i, &off[i], 400);
    check(calls(SYS_GUI_CLIP_PUSH) == 8, "nest: eight containers nest, the ninth is skipped");
    for (int i = 0; i < 9; i++) aui_scroll_end();
    check(calls(SYS_GUI_CLIP_POP) == 8 && clipn == 0, "nest: all nine ends balance");
    for (int i = 0; i < 20; i++) aui_clip_push_round(aui_r(1, 1, 150, 110), 4);
    check(calls(SYS_GUI_CLIP_PUSH) == 8 + 16, "nest: the clip stack stops at 16 levels");
    for (int i = 0; i < 20; i++) aui_clip_pop();
    check(calls(SYS_GUI_CLIP_POP) == 8 + 16 && clipn == 0 && clip_over == 0 && cgn == 0,
          "nest: twenty pops balance twenty pushes");
    aui_end();
    check(ol_gui2_info(1) == 0 && cg_leaks == 0, "nest: nothing leaked");
    aui_begin(0x202020);
    teardown();
}

/* ------------------------------------------------------------ damage */

static void dmg_frame(int alpha)
{
    aui_begin(0x202020);
    aui_fade_begin(aui_r(30, 30, 40, 30), alpha);
    aui_fill(35, 35, 20, 10, 0xFF0000);
    aui_fade_end();
    aui_end();
}

static void t_damage(void)
{
    setup(100, 1);
    dmg_frame(100);                       /* the first frame is a full flush */
    aui_host_stats_reset(H);
    dmg_frame(180);                       /* only the group alpha changed */
    int ok = H->ndmg > 0;
    if (ok) {
        struct aui_host_rect d = H->dmg[H->ndmg - 1];
        ok = d.x <= 30 && d.y <= 30 && d.x + d.w >= 70 && d.y + d.h >= 60 && d.w * d.h < 160 * 120;
    }
    check(ok, "damage: an alpha-only group change reaches a partial flush rect");
    aui_host_stats_reset(H);
    dmg_frame(180);
    check(H->ndmg == 0 || H->dmg[H->ndmg - 1].w <= 0 || calls(SYS_GUI_FLUSH_RECT) + calls(SYS_GUI_FLUSH) == 0,
          "damage: an identical frame flushes nothing");
    aui_begin(0x202020);
    teardown();
}
#endif /* !AUI_FADE_LEGACY_MIX */

int main(void)
{
    t_fade_group();
#ifndef AUI_FADE_LEGACY_MIX
    t_disabled_legacy();
    t_balance();
    t_scroll();
    t_limit();
    t_nest();
    t_damage();
#endif
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
