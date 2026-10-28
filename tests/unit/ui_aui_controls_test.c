/* ui-aui-controls: switch, checkbox, radio, slider, stepper, progress, spinner,
 * tooltip and badge of design 4.6 / 4.7 / 4.14 / 4.15 / 4.17, driven on the host
 * against the REAL aui.c (included, so the frame clock and the animation slots
 * can be read) over the aui host transport with the real SYS_GUI_PATH /
 * SHADOW2 / GLASS2 / CLIP / GROUP handlers.
 *
 *   switch-size      the on track is 38 x 22                     FAIL switch-size
 *   switch-knob      knob centre at x+11 off, x+27 on             FAIL switch-knob
 *   switch-settle    the knob settles in <= 160 ms, then the app  FAIL switch-settle
 *                    asks for nothing (wait 0, not due)
 *   switch-rm        Reduce Motion: the first frame is final      FAIL switch-rm
 *   glass            RT off: the switch makes 0 GLASS2 calls; RT  FAIL glass
 *                    on or off, the whole control set makes 0
 *   check-size       checkbox 16 x 16, radio 16 x 16              FAIL check-size
 *   check-tick       the tick is one PATH stroke of 1.75 pt,      FAIL check-tick
 *                    round caps, three points
 *   disabled         a disabled control is composited at 102      FAIL disabled
 *   slider-keys      arrows step, shift x10, Home, End            FAIL slider-keys
 *   slider-value     the value column is right-aligned at the     FAIL slider-value
 *                    44-wide column's right edge
 *   stepper          plus / minus change the value inside range   FAIL stepper
 *   progress         bar h 6; indeterminate is 30 % wide, frozen  FAIL progress
 *                    under Reduce Motion, 0 wake
 *   spinner-rate     <= 20 Hz (24) / <= 12 Hz (16), damage in own FAIL spinner-rate
 *                    rect, keeps turning under Reduce Motion      FAIL spinner-damage
 *   tooltip          dark style, both modes                       FAIL tooltip
 *   badge            capsule h 18, semantic tints                 FAIL badge
 *
 * Failures print "FAIL <check>"; the exit status is 1 on any. The control
 * (-DUI_CONTROLS_NEGCTL, in aui_controls.inc) restores the 44 x 24 switch and
 * must fail 'switch-size' and nothing else. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aui.c"
#include "aui_host.h"

extern void (*uac_path_spy)(const struct logit_path2 *, const struct logit_paint2 *, const struct logit_stroke2 *);

static int checks, fails;
static void check(int ok, const char *fmt, ...)
{
    checks++;
    if (ok) return;
    fails++;
    va_list ap; va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---------------------------------------------------------------- world */

static struct aui_host *H;
static int g_rt, g_rm;

static long setting_hook(struct aui_host *h, long a, long b, long c)
{
    (void)h; (void)c;
    if (g_rt && !strcmp((const char *)a, "ui.reduce_transparency")) { strcpy((char *)b, "1"); return 1; }
    if (g_rm && !strcmp((const char *)a, "ui.reduce_motion")) { strcpy((char *)b, "1"); return 1; }
    return -1;
}

static void world(int pct, int dark, int rt, int rm)
{
    aui_host_handlers_clear();
    H = aui_host_new(200, 160, pct);
    H->dark = dark;
    g_rt = rt; g_rm = rm;
    aui_host_enable_v2(H);
    aui_host_on(SYS_SETTING_GET, setting_hook);
    aui_host_clock_set(10000);
    gui_create("aui-controls", 200, 160);
    aui_set_size(200, 160);
    aui_set_dark(dark);
    aui_anim_reset();
    anim_gen = 10;
}
static void unworld(void) { aui_host_free(H); H = 0; }

/* scene state */
static int sw_state, sw_enabled = 1, cb_state, cb_enabled = 1, rad_group, sl_val = 40, st_val = 5;
static int pg_pct = 50, sp_d = 24, show_tip, bd_kind;
static unsigned sl_flags;
static int sl_step;
static const char *bd_text = "12";
enum { SC_SWITCH, SC_CHECK, SC_RADIO, SC_SLIDER, SC_STEPPER, SC_PROGRESS, SC_SPINNER, SC_BADGE, SC_ALL };
static int g_scene;

#define SWX 20
#define SWY 20
#define CBX 20
#define CBY 60
#define SLX 20
#define SLY 100
#define SLW 160

static void scene(void)
{
    if (g_scene == SC_SWITCH || g_scene == SC_ALL) {
        aui_toggle(SWX, SWY, &sw_state, sw_enabled);
        if (show_tip) aui_tooltip_ex("tip text", "K");
    }
    if (g_scene == SC_CHECK || g_scene == SC_ALL) aui_checkbox_ex(CBX, CBY, 0, &cb_state, cb_enabled);
    if (g_scene == SC_RADIO || g_scene == SC_ALL) aui_radio(CBX + 40, CBY, 0, &rad_group, 0);
    if (g_scene == SC_SLIDER || g_scene == SC_ALL) aui_slider_ex(SLX, SLY, SLW, &sl_val, 0, 100, sl_flags, "slow", "fast", sl_step);
    if (g_scene == SC_STEPPER || g_scene == SC_ALL) aui_stepper(20, 130, &st_val, 0, 10, 1, 1);
    if (g_scene == SC_PROGRESS || g_scene == SC_ALL) {
        aui_progress(110, 30, 80, pg_pct);
    }
    if (g_scene == SC_SPINNER || g_scene == SC_ALL) aui_spinner_ex(150, 60, sp_d);
    if (g_scene == SC_BADGE || g_scene == SC_ALL) aui_badge_ex(110, 130, bd_text, bd_kind);
}

static void frame(void)
{
    aui_begin(AUI_BG);
    scene();
    aui_end();
}
static void send(int type, int a, int b, int key)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.a = a; e.b = b; e.button = (type == EV_MOUSE || type == EV_MOUSE_UP) ? EV_BTN_LEFT : 0;
    (void)key;
    aui_feed(&e);
    frame();
    aui_feed_done();
}
static void key(int k, int mods)
{
    struct logit_event e;
    memset(&e, 0, sizeof e);
    e.type = EV_KEY; e.a = k; e.mods = mods;
    aui_feed(&e);
    frame();
    aui_feed_done();
}
/* run frames at dt ms until the app would sleep; returns elapsed ms of the last
 * frame that still asked for another (-1: still going after 2 s) */
static int settle(int dt)
{
    int t = 0;
    for (int i = 0; i < 200; i++) {
        aui_host_clock_advance((unsigned long long)dt); t += dt;
        frame();
        if (!anim_live && !anim_loop_want) return t;
    }
    return -1;
}
static void tick_ms(int dt) { aui_host_clock_advance((unsigned long long)dt); frame(); }

/* ---------------------------------------------------------------- pixels */

static unsigned px_at(int x, int y)
{
    int dx = x * H->pct / 100, dy = y * H->pct / 100;
    return H->px[dy * H->w + dx] & 0xFFFFFFu;
}
static int near_rgb(unsigned a, unsigned b, int tol)
{
    for (int s = 0; s < 24; s += 8) {
        int d = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (d < 0) d = -d;
        if (d > tol) return 0;
    }
    return 1;
}
/* bbox (points, 100 %) of pixels near `col` inside [x0,x1) x [y0,y1) */
struct bb { int x0, y0, x1, y1, n; };
static struct bb bbox_of(unsigned col, int tol, int x0, int y0, int x1, int y1)
{
    struct bb b = { 9999, 9999, -1, -1, 0 };
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (near_rgb(px_at(x, y), col, tol)) {
                if (x < b.x0) b.x0 = x;
                if (y < b.y0) b.y0 = y;
                if (x > b.x1) b.x1 = x;
                if (y > b.y1) b.y1 = y;
                b.n++;
            }
    return b;
}
static int glass_calls(void) { return (int)H->calls[SYS_GUI_GLASS2]; }

/* ------------------------------------------------------------------ tests */

static int sw_w_meas = 38;
static void t_switch(int dark)
{
    unsigned acc = aui_tok()->accent;
    world(100, dark, 0, 0);
    g_scene = SC_SWITCH; sw_state = 1; sw_enabled = 1;
    frame(); settle(16);
    struct bb b = bbox_of(acc, 24, SWX - 4, SWY - 4, SWX + 60, SWY + 40);
    int w = b.x1 - b.x0 + 1, h = b.y1 - b.y0 + 1;
    check(w >= 37 && w <= 38 && h >= 21 && h <= 22, "switch-size %dx%d (want 38x22)%s", w, h, dark ? " dark" : "");
    /* the knob: white pixels inside the track, centre x */
    struct bb k = bbox_of(0xFFFFFF, 6, SWX, SWY + 4, SWX + 44, SWY + 18);
    /* the knob sits 2 pt inside the track's right edge (on) / left edge (off) */
    sw_w_meas = w;
    check(k.n > 0 && k.x1 >= SWX + 34 && k.x1 <= SWX + 36, "switch-knob on right edge %d (want %d)", k.x1, SWX + 35);
    unworld();

    /* off: knob on the left, fill.control track */
    world(100, dark, 0, 0);
    g_scene = SC_SWITCH; sw_state = 0;
    frame(); settle(16);
    k = bbox_of(0xFFFFFF, 6, SWX, SWY + 4, SWX + 44, SWY + 18);
    check(k.n > 0 && k.x0 >= SWX + 1 && k.x0 <= SWX + 3, "switch-knob off left edge %d (want %d)", k.x0, SWX + 2);
    unworld();
}

static void t_switch_motion(void)
{
    /* a click: the knob settles in <= 140 ms (frames every 16 ms), then quiet */
    world(100, 0, 0, 0);
    g_scene = SC_SWITCH; sw_state = 0;
    frame(); settle(16);
    send(EV_MOUSE, SWX + 19, SWY + 11, 0);
    send(EV_MOUSE_UP, SWX + 19, SWY + 11, 0);
    check(sw_state == 1, "switch-settle click did not toggle");
    int t = settle(8);
    check(t >= 0 && t <= 160, "switch-settle %d ms (limit 160: 16 pt travel on snappy)", t);
    check(aui_anim_wait() == 0 && !aui_anim_due(), "switch-settle still asking for frames after it settled");
    printf("switch settles in %d ms\n", t);
    unworld();

    /* Reduce Motion: the click's own frame already shows the final knob */
    world(100, 0, 0, 1);
    g_scene = SC_SWITCH; sw_state = 0;
    frame(); settle(16);
    send(EV_MOUSE, SWX + 19, SWY + 11, 0);
    send(EV_MOUSE_UP, SWX + 19, SWY + 11, 0);
    struct bb k = bbox_of(0xFFFFFF, 6, SWX, SWY + 4, SWX + 44, SWY + 18);
    check(k.x1 >= SWX + 34 && k.x1 <= SWX + 36 && !anim_live, "switch-rm knob right edge %d live=%d", k.x1, anim_live);
    unworld();
}

static void t_glass(void)
{
    for (int rt = 0; rt < 2; rt++) {
        world(100, 0, rt, 0);
        g_scene = SC_ALL; sw_state = 1; cb_state = 1; show_tip = 0;
        frame(); settle(16);
        aui_host_stats_reset(H);
        sw_state = 0;
        for (int i = 0; i < 6; i++) tick_ms(16);
        check(glass_calls() == 0, "glass RT=%d: %d GLASS2 calls from the control set", rt, glass_calls());
        unworld();
    }
    /* the tooltip too */
    world(100, 0, 1, 0);
    g_scene = SC_SWITCH; show_tip = 1;
    send(EV_MOUSE_MOVE, SWX + 5, SWY + 5, 0);
    for (int i = 0; i < 60; i++) tick_ms(16);
    check(glass_calls() == 0, "glass tooltip: %d GLASS2 calls", glass_calls());
    show_tip = 0;
    unworld();
}

static const struct logit_path2 *sp_path;
static int sp_n, sp_tick_w, sp_tick_cap, sp_tick_n, sp_lin;
static void path_spy(const struct logit_path2 *p, const struct logit_paint2 *pt, const struct logit_stroke2 *st)
{
    sp_path = p; sp_n++;
    if (pt && pt->kind == LOGIT_PAINT2_LINEAR) sp_lin++;
    if (st && st->width == 448 && p->nverb == 3) { sp_tick_w = st->width; sp_tick_cap = st->cap; sp_tick_n = (int)p->nverb; }
}

static void t_checkbox(int dark)
{
    unsigned acc = aui_tok()->accent;
    world(100, dark, 0, 0);
    uac_path_spy = path_spy;
    g_scene = SC_CHECK; cb_state = 1; cb_enabled = 1;
    sp_tick_w = 0;
    frame(); settle(16);
    uac_path_spy = 0;
    struct bb b = bbox_of(acc, 24, CBX - 4, CBY - 4, CBX + 30, CBY + 30);
    int w = b.x1 - b.x0 + 1, h = b.y1 - b.y0 + 1;
    check(w >= 15 && w <= 16 && h >= 15 && h <= 16, "check-size checkbox %dx%d (want 16x16)", w, h);
    check(sp_tick_w == 448 && sp_tick_cap == LOGIT_STROKE2_CAP_ROUND && sp_tick_n == 3,
          "check-tick stroke %d (want 448 = 1.75 pt) cap %d verbs %d", sp_tick_w, sp_tick_cap, sp_tick_n);
    unworld();

    /* radio */
    world(100, dark, 0, 0);
    g_scene = SC_RADIO; rad_group = 0;
    frame(); settle(16);
    b = bbox_of(acc, 24, CBX + 36, CBY - 4, CBX + 70, CBY + 30);
    w = b.x1 - b.x0 + 1; h = b.y1 - b.y0 + 1;
    check(w >= 15 && w <= 16 && h >= 15 && h <= 16, "check-size radio %dx%d (want 16x16)", w, h);
    struct bb dot = bbox_of(0xFFFFFF, 90, CBX + 40 + 4, CBY + 4, CBX + 40 + 12, CBY + 12);
    check(dot.n > 0 && dot.x1 - dot.x0 + 1 >= 5 && dot.x1 - dot.x0 + 1 <= 6, "check-size radio dot %d (want 6)", dot.x1 - dot.x0 + 1);
    unworld();
}

static void t_disabled(void)
{
    world(100, 0, 0, 0);
    g_scene = SC_CHECK; cb_state = 1; cb_enabled = 0;
    frame(); settle(16);
    unsigned want = aui_mix(AUI_BG, aui_tok()->accent, CTL_DIS_ALPHA);
    unsigned got = px_at(CBX + 13, CBY + 13);
    check(near_rgb(got, want, 8), "disabled corner pixel %06x (want %06x = accent at 102)", got, want);
    cb_enabled = 1;
    unworld();
}

static void t_slider(void)
{
    world(100, 0, 0, 0);
    g_scene = SC_SLIDER; sl_val = 40; sl_flags = AUI_SLIDER_VALUE; sl_step = 0;
    frame(); settle(16);
    /* click on the knob focuses without moving it far: click at the 40 % point */
    int tw_ = SLW - 52;
    send(EV_MOUSE, SLX + tw_ * 40 / 100, SLY + 11, 0);
    send(EV_MOUSE_UP, SLX + tw_ * 40 / 100, SLY + 11, 0);
    int v0 = sl_val;
    key(KEY_RIGHT, 0);
    check(sl_val == v0 + 2, "slider-keys right: %d -> %d (want +2)", v0, sl_val);
    v0 = sl_val;
    key(KEY_LEFT, 0);
    check(sl_val == v0 - 2, "slider-keys left: %d -> %d (want -2)", v0, sl_val);
    v0 = sl_val;
    key(KEY_RIGHT, EV_MOD_SHIFT);
    check(sl_val == v0 + 20, "slider-keys shift: %d -> %d (want +20)", v0, sl_val);
    key(KEY_HOME, 0);
    check(sl_val == 0, "slider-keys home: %d", sl_val);
    key(KEY_END, 0);
    check(sl_val == 100, "slider-keys end: %d", sl_val);
    unworld();

    /* stepped: step 10 snaps keys and the pointer */
    world(100, 0, 0, 0);
    g_scene = SC_SLIDER; sl_val = 0; sl_flags = AUI_SLIDER_TICKS; sl_step = 10;
    frame(); settle(16);
    send(EV_MOUSE, SLX + SLW * 33 / 100, SLY + 11, 0);
    send(EV_MOUSE_UP, SLX + SLW * 33 / 100, SLY + 11, 0);
    check(sl_val == 30, "slider-keys step snap: %d (want 30)", sl_val);
    key(KEY_RIGHT, 0);
    check(sl_val == 40, "slider-keys step key: %d (want 40)", sl_val);
    unworld();

    /* the value column: right edge fixed whatever the digit count */
    int right[2] = { 0, 0 }, left[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        world(100, 0, 0, 0);
        g_scene = SC_SLIDER; sl_val = i ? 100 : 7; sl_flags = AUI_SLIDER_VALUE; sl_step = 0;
        frame(); settle(16);
        int c0 = SLX + SLW - 44, c1 = SLX + SLW + 8;
        int x0 = 9999, x1 = -1;
        for (int y = SLY; y < SLY + 22; y++)
            for (int x = c0; x < c1; x++)
                if (!near_rgb(px_at(x, y), AUI_BG, 40)) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
        left[i] = x0; right[i] = x1;
        unworld();
    }
    check(right[0] > 0 && right[1] > 0 && right[0] <= SLX + SLW && right[1] <= SLX + SLW &&
          right[0] - right[1] <= 1 && right[1] - right[0] <= 1 && left[1] < left[0],
          "slider-value right edges %d/%d (<= %d), left %d/%d", right[0], right[1], SLX + SLW, left[0], left[1]);
    sl_flags = 0; sl_step = 0;
}

static void t_stepper(void)
{
    world(100, 0, 0, 0);
    g_scene = SC_STEPPER; st_val = 5;
    frame(); settle(16);
    send(EV_MOUSE, 20 + 92 - 12, 130 + 14, 0);
    send(EV_MOUSE_UP, 20 + 92 - 12, 130 + 14, 0);
    check(st_val == 6, "stepper plus: %d (want 6)", st_val);
    send(EV_MOUSE, 20 + 12, 130 + 14, 0);
    send(EV_MOUSE_UP, 20 + 12, 130 + 14, 0);
    send(EV_MOUSE, 20 + 12, 130 + 14, 0);
    send(EV_MOUSE_UP, 20 + 12, 130 + 14, 0);
    check(st_val == 4, "stepper minus: %d (want 4)", st_val);
    st_val = 10;
    send(EV_MOUSE, 20 + 92 - 12, 130 + 14, 0);
    send(EV_MOUSE_UP, 20 + 92 - 12, 130 + 14, 0);
    check(st_val == 10, "stepper clamp: %d (want 10)", st_val);
    /* geometry: the capsule is 92 x 28 of fill.control */
    struct bb b = bbox_of(aui_tok()->fill_control, 3, 14, 124, 120, 164);
    check(b.n > 0 && b.y1 - b.y0 + 1 >= 27 && b.y1 - b.y0 + 1 <= 28 && b.x1 - b.x0 + 1 >= 90 && b.x1 - b.x0 + 1 <= 92,
          "stepper capsule %dx%d (want 92x28)", b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
    unworld();
}

static void t_progress(void)
{
    unsigned acc = aui_tok()->accent;
    world(100, 0, 0, 0);
    g_scene = SC_PROGRESS; pg_pct = 100;
    frame(); settle(16);
    struct bb b = bbox_of(acc, 40, 100, 20, 200, 50);
    check(b.y1 - b.y0 + 1 >= 5 && b.y1 - b.y0 + 1 <= 6 && b.x1 - b.x0 + 1 >= 78 && b.x1 - b.x0 + 1 <= 80,
          "progress %dx%d (want 80x6)", b.x1 - b.x0 + 1, b.y1 - b.y0 + 1);
    /* a value change eases: the first frame after is short of the target and it settles */
    pg_pct = 50; frame(); settle(16);
    pg_pct = 62; tick_ms(16);
    struct bb m = bbox_of(acc, 40, 100, 20, 200, 50);
    check(m.n > 0 && m.x1 - m.x0 + 1 < 49, "progress eases (first frame already %d wide, target 49.6)", m.x1 - m.x0 + 1);
    int t = settle(8);
    check(t >= 0 && t <= 260, "progress settle %d ms", t);
    unworld();

    /* indeterminate: 30 % wide, 20 Hz at most, frozen under Reduce Motion */
    world(100, 0, 0, 0);
    g_scene = SC_PROGRESS; pg_pct = -1;
    frame();
    check(anim_loop_want == 1, "progress indeterminate does not request frames");
    int frames = 0;
    unsigned long long t0 = H->clock_ms;
    while (H->clock_ms < t0 + 2400) {
        int w = aui_anim_wait();
        check(w >= 33 || w <= 0 ? 1 : 0, "progress cadence wait %d ms (>= 33 for <= 30 Hz)", w);
        aui_host_clock_advance((unsigned long long)(w > 0 ? w : 16));
        frame(); frames++;
    }
    check(frames <= 2400 * 30 / 1000 + 1, "progress indeterminate ran %d frames in 2.4 s (<= 30 Hz)", frames);
    unworld();

    world(100, 0, 0, 1);
    g_scene = SC_PROGRESS; pg_pct = -1;
    frame();
    check(!anim_loop_want && aui_anim_wait() == 0, "progress RM: indeterminate still requests frames");
    b = bbox_of(acc, 40, 100, 20, 200, 50);
    check(b.x1 - b.x0 + 1 >= 23 && b.x1 - b.x0 + 1 <= 25, "progress RM bar %d wide (want 24 = 30 %% of 80)", b.x1 - b.x0 + 1);
    unworld();
}

/* a checksum of the pixels in a box */
static unsigned sum_box(int x0, int y0, int x1, int y1)
{
    unsigned s = 2166136261u;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) s = (s ^ px_at(x, y)) * 16777619u;
    return s;
}

static void t_spinner(int d, int rm)
{
    world(100, 0, 0, rm);
    g_scene = SC_SPINNER; sp_d = d;
    uac_path_spy = path_spy;
    sp_lin = 0;
    frame(); frame();
    uac_path_spy = 0;
    check(sp_lin > 0, "spinner d=%d: no LINEAR-paint arc", d);
    int x0 = 150 - d / 2 - 4, y0 = 60 - d / 2 - 4, x1 = 150 + d / 2 + 4, y1 = 60 + d / 2 + 4;
    int changes = 0, frames = 0, outside = 0;
    unsigned last = sum_box(x0, y0, x1, y1);
    unsigned long long t0 = H->clock_ms;
    aui_host_stats_reset(H);
    while (H->clock_ms < t0 + 2000) {
        int w = aui_anim_wait();
        check(w > 0, "spinner d=%d rm=%d: asks for no frames (wait %d)", d, rm, w);
        aui_host_clock_advance((unsigned long long)(w > 0 ? w : 16));
        frame(); frames++;
        unsigned s = sum_box(x0, y0, x1, y1);
        if (s != last) changes++;
        last = s;
    }
    int hz_lim = d <= 20 ? 12 : 20;
    check(changes <= 2 * hz_lim + 1, "spinner-rate d=%d: %d changes in 2 s (limit %d Hz = %d)", d, changes, hz_lim, 2 * hz_lim + 1);
    check(changes >= 8, "spinner-rate d=%d rm=%d: only %d changes in 2 s (not turning)", d, rm, changes);
    /* the flush set stays inside the spinner's own rect (plus its stroke halo) */
    for (int i = 0; i < H->ndmg; i++) {
        const struct aui_host_rect *r = &H->dmg[i];
        if (r->x < x0 - 4 || r->y < y0 - 4 || r->x + r->w > x1 + 4 || r->y + r->h > y1 + 4) outside++;
    }
    check(outside == 0 && H->ndmg > 0, "spinner-damage d=%d: %d of %d flush rects leave the spinner box (frames %d)", d, outside, H->ndmg, frames);
    printf("spinner d=%d rm=%d: %d pixel changes in 2 s\n", d, rm, changes);
    unworld();
}

static void t_tooltip(int dark)
{
    world(100, dark, 0, 0);
    g_scene = SC_SWITCH; show_tip = 1; sw_state = 0;
    send(EV_MOUSE_MOVE, SWX + 5, SWY + 5, 0);
    for (int i = 0; i < 80; i++) tick_ms(16);
    /* box at (mx + 12, my + 22) */
    int tx = SWX + 5 + 12, ty = SWY + 5 + 22;
    unsigned bg = dark ? 0x3A3A44u : 0x2A2A32u;
    int a = dark ? 240 : 235;
    unsigned want = aui_mix(AUI_BG, bg, a);
    unsigned got = px_at(tx + 3, ty + 3);
    check(near_rgb(got, want, 6), "tooltip %s: pixel %06x (want %06x = %06x @ %d over the window)", dark ? "dark" : "light", got, want, bg, a);
    /* text is #F4F4F8: some pixel in the box is near it */
    struct bb ink = bbox_of(0xF4F4F8, 24, tx + 2, ty + 2, tx + 60, ty + 26);
    check(ink.n > 0, "tooltip %s: no #F4F4F8 text ink", dark ? "dark" : "light");
    /* and it fades out: moving away leaves the window clean after 120 ms + */
    send(EV_MOUSE_MOVE, 190, 150, 0);
    for (int i = 0; i < 20; i++) tick_ms(16);
    check(near_rgb(px_at(tx + 3, ty + 3), AUI_BG, 6), "tooltip %s: still visible 320 ms after the pointer left", dark ? "dark" : "light");
    show_tip = 0;
    unworld();
}

static void t_badge(void)
{
    world(100, 0, 0, 0);
    g_scene = SC_BADGE;
    int kinds[5] = { AUI_BADGE_NEUTRAL, AUI_BADGE_ACCENT, AUI_BADGE_SUCCESS, AUI_BADGE_WARNING, AUI_BADGE_ERROR };
    int w0 = 0;
    for (int i = 0; i < 5; i++) {
        bd_kind = kinds[i];
        frame();
        struct bb b = bbox_of(AUI_BG, 0, 0, 0, 0, 0);
        (void)b;
        /* a pixel inside the capsule left of the text differs from the window */
        unsigned px = px_at(110 + 3, 130 + 9);
        check(!near_rgb(px, AUI_BG, 2), "badge kind %d draws no capsule", kinds[i]);
    }
    aui_begin(AUI_BG);
    w0 = aui_badge_ex(110, 130, "12", AUI_BADGE_NEUTRAL);
    int w1 = aui_badge_ex(110, 150, "\xE6\x96\xB0", AUI_BADGE_ACCENT);
    aui_end();
    check(w0 >= 18 && w1 >= 18, "badge widths %d %d (min 18)", w0, w1);
    bd_kind = 0;
    unworld();
}

int main(void)
{
    for (int dark = 0; dark < 2; dark++) { t_switch(dark); t_checkbox(dark); t_tooltip(dark); }
    t_switch_motion();
    t_glass();
    t_disabled();
    t_slider();
    t_stepper();
    t_progress();
    t_spinner(24, 0); t_spinner(32, 0); t_spinner(16, 0); t_spinner(24, 1);
    t_badge();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
