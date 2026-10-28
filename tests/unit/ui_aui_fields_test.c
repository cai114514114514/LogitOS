/* ui-aui-fields: the text field, large field, search field, caret, overlay
 * scrollbar and gutterless scroll containers (design 「澄」 4.4 and 4.12).
 *
 * Drives the REAL aui.c (included, so its statics -- the clip stack, the caret
 * and scrollbar state, the wake scheduler -- are reachable) over the host
 * harness, which renders into a pixel buffer and logs the flush rectangles. It
 * runs each case twice: INFO=0 (the legacy kernel) and v2 (PATH, so the
 * 1-device-pixel rims are real paths). Checks print "FAIL <check> ..."; the
 * exit status is 1 on any failure.
 *
 * -DUI_FIELDS_NEGCTL (tests/ui-aui-fields.mk) keeps the old 12 pt gutter in
 * the scroll container's clip and must fail the 'gutterless' check and nothing
 * else. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.c"
#include "aui_host.h"

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

/* ------------------------------------------------------------- the world */

static struct aui_host *H;
static int g_pct, g_v2, g_dark, g_reduce;
static void (*scene)(void);

/* ui.reduce_motion = g_reduce; everything else unset. */
static long host_setting(struct aui_host *h, long a, long b, long c)
{
    (void)h;
    if (!strcmp((const char *)a, "ui.reduce_motion")) {
        char *o = (char *)b;
        if (c < 2) return -1;
        o[0] = g_reduce ? '1' : '0'; o[1] = 0;
        return 1;
    }
    return -1;
}

static void setup(int pct, int v2, int dark, int reduce, void (*fn)(void))
{
    aui_host_handlers_clear();
    H = aui_host_new(320, 200, pct);
    g_pct = pct; g_v2 = v2; g_dark = dark; g_reduce = reduce; scene = fn;
    H->dark = dark;
    sc_pct = pct;                                /* aui_scale() caches the first answer */
    /* aui.c keeps focus, caret, scroll and animation state in file-scope
     * statics: every case starts from a clean slate. */
    focus_id = 0; focus_vis = 0; tf_owner = tf_caret = tf_anchor = tf_sx = 0; tf_touch = 0;
    in.active = in.down = in.hot = 0; in.ev = 0;
    memset(sb_tab, 0, sizeof sb_tab);
    aui_anim_reset();
    if (v2) aui_host_enable_v2(H);
    aui_host_on(SYS_SETTING_GET, host_setting);
    aui_host_clock_set(1000);
}
static void teardown(void) { aui_host_free(H); H = 0; }

/* One frame at fake time `ms`, with at most one event. */
static void step(unsigned long long ms, const struct logit_event *e)
{
    aui_host_clock_set(ms);
    aui_host_stats_reset(H);
    if (e) aui_feed(e);
    aui_begin(AUI_BG);
    scene();
    aui_end();
    aui_feed_done();
}
static struct logit_event ev_mouse(int x, int y)
{ struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_MOUSE; e.a = x; e.b = y; e.button = EV_BTN_LEFT; return e; }
static struct logit_event ev_up(int x, int y)
{ struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_MOUSE_UP; e.a = x; e.b = y; e.button = EV_BTN_LEFT; return e; }
static struct logit_event ev_move(int x, int y)
{ struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_MOUSE_MOVE; e.a = x; e.b = y; return e; }
static struct logit_event ev_key(int k, int mods)
{ struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_KEY; e.a = k; e.mods = mods; return e; }
static struct logit_event ev_wheel(int x, int y, int n)
{ struct logit_event e; memset(&e, 0, sizeof e); e.type = EV_WHEEL; e.a = x; e.b = y; e.wheel = n; return e; }

static unsigned px_at(int dx, int dy)
{
    if (dx < 0 || dy < 0 || dx >= H->w || dy >= H->h) return 0xDEADBEEF;
    return H->px[dy * H->w + dx] & 0xFFFFFFu;
}
static int dev(int pt) { return aui_host_pt(pt, g_pct); }
static unsigned px_pt(int xpt, int ypt) { return px_at(dev(xpt), dev(ypt)); }
static int chan_d(unsigned a, unsigned b)
{
    int d = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (int)((a >> s) & 255) - (int)((b >> s) & 255);
        if (x < 0) x = -x;
        if (x > d) d = x;
    }
    return d;
}
static int near_c(unsigned a, unsigned b, int tol) { return chan_d(a, b) <= tol; }
/* `top` at alpha a (0..255) over `under`, as the blend would land. */
static unsigned over(unsigned under, unsigned top, int a) { return aui_mix(under, top, a); }
static const char *mode(void) { return g_v2 ? "v2" : "info0"; }

/* ----------------------------------------------------------------- scenes */

static char f1[64], f2[64], sf[64];
static int err_on, large_on, enabled_on = 1, f1_ret;
static const char *ph = "type";
#define FX 20
#define FY 30
#define FW 200

static void scene_field(void)
{
    if (large_on) f1_ret = aui_textfield_ex(FX, FY, FW, f1, sizeof f1, ph, enabled_on, err_on ? "bad" : 0, AUI_TF_LARGE);
    else if (err_on) f1_ret = aui_textfield_ex(FX, FY, FW, f1, sizeof f1, ph, enabled_on, "bad");
    else f1_ret = aui_textfield_ex(FX, FY, FW, f1, sizeof f1, ph, enabled_on);
    aui_textfield(FX, 120, FW, f2, sizeof f2);
}

static void scene_search(void)
{
    f1_ret = aui_searchfield(FX, FY, FW, sf, sizeof sf, 0);
}

#define SCX 20
#define SCY 20
#define SCW 160
#define SCH 120
static int sc_off, sc_content = 600;
static void scene_scroll(void)
{
    aui_scroll_begin(SCX, SCY, SCW, SCH, &sc_off, sc_content);
    aui_scroll_end();
}

/* Focus a field by clicking it and let the press settle. */
static void focus_click(int x, int y, unsigned long long ms)
{
    struct logit_event d = ev_mouse(x, y), u = ev_up(x, y);
    step(ms, &d); step(ms + 1, &u);
}

static void type_str(const char *s, unsigned long long *ms)
{
    for (; *s; s++) { struct logit_event e = ev_key(*s, 0); *ms += 5; step(*ms, &e); }
}

/* The union of this frame's logged damage rectangles, device pixels. */
static int dmg_union(int *x0, int *y0, int *x1, int *y1)
{
    if (!H->ndmg) return 0;
    *x0 = *y0 = 1 << 30; *x1 = *y1 = -(1 << 30);
    for (int i = 0; i < H->ndmg; i++) {
        const struct aui_host_rect *r = &H->dmg[i];
        if (r->x < *x0) *x0 = r->x;
        if (r->y < *y0) *y0 = r->y;
        if (r->x + r->w > *x1) *x1 = r->x + r->w;
        if (r->y + r->h > *y1) *y1 = r->y + r->h;
    }
    return 1;
}

/* ------------------------------------------------ 1. the caret, as a pure fn */

static void t_caret_alpha(void)
{
    struct { unsigned t; int lo, hi; const char *what; } c[] = {
        {    0, 255, 255, "solid right after input" },
        {  529, 255, 255, "solid through the first 530 ms" },
        {  530, 250, 255, "fade-out starts at 530" },
        {  575, 120, 135, "half faded at 575" },
        {  620,   0,   0, "off by 620" },
        { 1000,   0,   0, "off phase" },
        { 1059,   0,   0, "off until 1060" },
        { 1060,   0,   5, "fade-in starts at 1060" },
        { 1105, 120, 135, "half in at 1105" },
        { 1150, 255, 255, "on by 1150" },
        { 1500, 255, 255, "on phase" },
        { 1590, 250, 255, "second fade-out" },
        { 7999,   0, 255, "any phase just before 8 s" },
        { 8000, 255, 255, "solid at 8 s" },
        { 9500, 255, 255, "solid and stays solid" },
        {60000, 255, 255, "solid when long idle" },
    };
    for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
        int a = tf_caret_alpha(c[i].t, 0);
        check(a >= c[i].lo && a <= c[i].hi, "caret-phase %s: alpha(%u)=%d want %d..%d",
              c[i].what, c[i].t, a, c[i].lo, c[i].hi);
    }
    check(tf_caret_alpha(700, 1) == 255, "caret-phase reduce-motion: alpha(700) is %d, want solid", tf_caret_alpha(700, 1));
    /* the blink is 530 on / 530 off: the off interval [620,1060) is 440 fully
     * off + two 90 ms fades; count fully-on and fully-off ms over one period */
    int on = 0, off = 0;
    for (unsigned t = 530; t < 530 + 1060; t++) { int a = tf_caret_alpha(t, 0); if (a == 255) on++; if (a == 0) off++; }
    check(on >= 430 && on <= 450 && off >= 430 && off <= 450, "caret-phase period: full-on %d ms, full-off %d ms", on, off);
    /* 2 device pixels wide at every scale */
    for (int sc = 100; sc <= 300; sc += 25) {
        H = aui_host_new(40, 40, sc < 400 ? sc : 400);
        sc_pct = sc;
        aui_ensure();
        int w = tf_caret_w();
        check(aui_dev(w) >= 2 && (w == 1 || aui_dev(w - 1) < 2), "caret-width @%d: %d pt is %d dev px", sc, w, aui_dev(w));
        aui_host_free(H); H = 0;
    }
}

/* ------------------------------------------------ 2. caret damage and wakes */

static void t_caret_damage(void)
{
    f1[0] = f2[0] = 0; err_on = large_on = 0; enabled_on = 1; ph = 0;
    setup(g_pct, g_v2, 0, 0, scene_field);
    unsigned long long ms = 1000;
    step(ms, 0);
    focus_click(FX + 40, FY + 14, ms + 10);
    unsigned touch = tf_touch;
    check(tf_owner != 0 && focus_id == tf_owner, "caret focus @%d %s: the click did not focus the field", g_pct, mode());
    /* the caret rect, device px */
    int cx0 = dev(FX + TF_PAD), cy0 = dev(FY + (AUI_H_CTL - TF_CARET_H) / 2);
    int cw = dev(tf_caret_w()), ch = dev(TF_CARET_H);

    /* a frame in the middle of the first fade-out (touch + 575): only the caret changes */
    step(touch + 560, 0);                     /* settle the previous state (solid) */
    step(touch + 575, 0);
    int x0, y0, x1, y1;
    int got = dmg_union(&x0, &y0, &x1, &y1);
    check(got, "caret-damage @%d %s: a fading caret frame flushed nothing", g_pct, mode());
    if (got) {
        int m = 3 + g_pct / 100;
        check(x0 >= cx0 - m && y0 >= cy0 - m && x1 <= cx0 + cw + m && y1 <= cy0 + ch + m,
              "caret-damage @%d %s: damage %d,%d..%d,%d is not the caret rect %d,%d..%d,%d (+-%d)",
              g_pct, mode(), x0, y0, x1, y1, cx0, cy0, cx0 + cw, cy0 + ch, m);
        check(x0 <= cx0 && y0 <= cy0 && x1 >= cx0 + cw && y1 >= cy0 + ch,
              "caret-damage @%d %s: damage %d,%d..%d,%d does not cover the caret %d,%d..%d,%d",
              g_pct, mode(), x0, y0, x1, y1, cx0, cy0, cx0 + cw, cy0 + ch);
    }
    check(anim_have_due, "caret-wake @%d %s: a blinking caret asked for no wake", g_pct, mode());

    /* phases on screen: the caret pixel is the accent when on, the field when off */
    unsigned fieldc = g_dark ? 0 : 0xFFFFFF;
    (void)fieldc;
    int px = cx0, py = cy0 + 1;               /* the caret's top row: above the placeholder glyphs */
    step(touch + 100, 0);
    unsigned on_c = px_at(px, py);
    check(near_c(on_c, aui_t.accent, 8), "caret-on @%d %s: pixel %06x, want accent %06x", g_pct, mode(), on_c, aui_t.accent);
    step(touch + 800, 0);
    unsigned off_c = px_at(px, py);
    check(near_c(off_c, aui_t.bg_content, 4), "caret-off @%d %s: pixel %06x, want the field %06x", g_pct, mode(), off_c, aui_t.bg_content);
    step(touch + 1300, 0);
    check(near_c(px_at(px, py), aui_t.accent, 8), "caret-on-again @%d %s: pixel %06x", g_pct, mode(), px_at(px, py));

    /* blink asks for the loop right up to 8 s, then nothing at all */
    step(touch + 7000, 0);
    check(anim_have_due, "caret-wake @%d %s: no wake at 7 s", g_pct, mode());
    step(touch + 8010, 0);
    check(!anim_have_due && aui_anim_wait() == 0, "caret-idle @%d %s: still asking for wakes at 8 s (due=%d wait=%d)",
          g_pct, mode(), anim_have_due, aui_anim_wait());
    check(near_c(px_at(px, py), aui_t.accent, 8), "caret-idle-solid @%d %s: pixel %06x not solid at 8 s", g_pct, mode(), px_at(px, py));
    step(touch + 9500, 0);
    check(H->ndmg == 0 && !anim_have_due, "caret-silent @%d %s: a repeated idle frame flushed %d rects / wake %d",
          g_pct, mode(), H->ndmg, anim_have_due);

    /* typing makes it solid again right away, and re-arms the blink */
    unsigned long long t2 = touch + 10000;
    struct logit_event k = ev_key('a', 0);
    step(t2, &k);
    check(near_c(px_at(px + dev(tfw("a", 1)), py), aui_t.accent, 8) || tf_caret_alpha(frame_ms - tf_touch, 0) == 255,
          "caret-retouch @%d %s: not solid after a keystroke", g_pct, mode());
    check(anim_have_due, "caret-retouch @%d %s: the blink did not re-arm", g_pct, mode());
    ph = "type";
    teardown();
}

static void t_caret_unfocused(void)
{
    f1[0] = f2[0] = 0; err_on = large_on = 0; enabled_on = 1;
    setup(g_pct, g_v2, 0, 0, scene_field);
    step(1000, 0); step(1100, 0);
    check(!anim_have_due, "caret-unfocused @%d %s: an unfocused field asked for a wake", g_pct, mode());
    check(H->ndmg == 0, "caret-unfocused @%d %s: an unfocused field flushed %d rects", g_pct, mode(), H->ndmg);
    teardown();
}

static void t_caret_reduced(void)
{
    f1[0] = f2[0] = 0; err_on = large_on = 0; enabled_on = 1;
    setup(100, g_v2, 0, 1, scene_field);
    step(1000, 0);
    focus_click(FX + 40, FY + 14, 1010);
    unsigned touch = tf_touch;
    int px = dev(FX + TF_PAD), py = dev(FY + 14);
    step(touch + 800, 0);                     /* the off phase of a blinking caret */
    check(near_c(px_at(px, py), aui_t.accent, 8), "caret-reduced %s: the caret blinked off under Reduce Motion (pixel %06x)", mode(), px_at(px, py));
    check(!anim_have_due, "caret-reduced %s: Reduce Motion still asked for the blink loop", mode());
    teardown();
}

/* ------------------------------------------------ 3. the field's box */

/* A pixel row/column scan for the box: rows from y-5 to y+h+5 at the field's
 * horizontal middle, reporting those that differ from the window. */
static void box_rows(int *first, int *last, unsigned bgc)
{
    *first = *last = -1;
    int mx = dev(FX + FW / 2);
    for (int y = dev(FY) - dev(6); y < dev(FY) + dev(AUI_H_CTL + 12); y++) {
        if (near_c(px_at(mx, y), bgc, 1)) continue;
        if (*first < 0) *first = y;
        *last = y;
    }
}

static void t_box(void)
{
    f1[0] = f2[0] = 0; err_on = large_on = 0; enabled_on = 1;
    for (int dark = 0; dark < 2; dark++) {
        setup(g_pct, g_v2, dark, 0, scene_field);
        step(1000, 0); step(1100, 0);
        unsigned bgc = aui_t.bg;
        int mx = dev(FX + FW / 2);
        /* height 28: rim rows at y and y+27, nothing at y-1 or y+28 */
        int top = dev(FY), bot = dev(FY + AUI_H_CTL) - 1;
        check(!near_c(px_at(mx, top), bgc, 2) && !near_c(px_at(mx, bot), bgc, 2),
              "field-h28 @%d %s dark=%d: rim rows %06x/%06x match the window", g_pct, mode(), dark, px_at(mx, top), px_at(mx, bot));
        check(near_c(px_at(mx, top - 1), bgc, 2) && near_c(px_at(mx, bot + 1), bgc, 2),
              "field-h28 @%d %s dark=%d: pixels outside the 28 pt box are not the window (%06x / %06x)",
              g_pct, mode(), dark, px_at(mx, top - 1), px_at(mx, bot + 1));
        /* fill: white in light, white @ 13 in dark */
        unsigned want = dark ? over(bgc, 0xFFFFFF, 13) : 0xFFFFFF;
        unsigned got = px_at(mx, dev(FY + 14) - 6);
        check(near_c(got, want, 3), "field-fill @%d %s dark=%d: %06x, want %06x", g_pct, mode(), dark, got, want);
        /* rim: sep.strong over the fill, 1 device pixel */
        unsigned sep = aui_t.sep_strong;
        unsigned wantrim = over(want, sep & 0xFFFFFF, (int)(sep >> 24));
        if (g_v2) {
            check(near_c(px_at(mx, top), wantrim, 6), "field-rim @%d v2 dark=%d: %06x, want sep.strong %06x", g_pct, dark, px_at(mx, top), wantrim);
            check(near_c(px_at(mx, top + 1), want, 6), "field-rim @%d v2 dark=%d: the rim is thicker than 1 dev px (%06x)", g_pct, dark, px_at(mx, top + 1));
        }
        teardown();
    }
    /* radius 8: the corner pixel is outside the shape, the box is not square */
    setup(g_pct, g_v2, 0, 0, scene_field);
    step(1000, 0); step(1100, 0);
    check(near_c(px_pt(FX, FY), aui_t.bg, 4), "field-r8 %s: the corner pixel %06x is inside a square box", mode(), px_pt(FX, FY));
    teardown();
}

static void t_focus(void)
{
    f1[0] = f2[0] = 0; err_on = large_on = 0; enabled_on = 1;
    setup(100, g_v2, 0, 0, scene_field);
    step(1000, 0);
    focus_click(FX + 40, FY + 14, 1010);
    step(1500, 0); step(1600, 0);             /* the 90 ms fades are over */
    int mx = dev(FX + FW / 2);
    int top = dev(FY);
    check(near_c(px_at(mx, top), aui_t.accent, 6) || (!g_v2 && !near_c(px_at(mx, top), 0xFFFFFF, 20)),
          "field-focus %s: the focused rim is %06x, want accent %06x", mode(), px_at(mx, top), aui_t.accent);
    /* the ring: 3 pt, its inner edge 1 pt outside; accent.focusRing over the window */
    unsigned rc = aui_t.accent_focus_ring;
    unsigned wantring = over(aui_t.bg, rc & 0xFFFFFF, (int)(rc >> 24));
    unsigned r2 = px_at(mx, top - 2);        /* 2 pt above the box: inside the 1..4 ring */
    check(near_c(r2, wantring, 10), "field-ring %s: pixel %06x 2 pt outside the box, want focusRing over the window %06x", mode(), r2, wantring);
    check(near_c(px_at(mx, top - 1), aui_t.bg, 3) || g_v2 == 0, "field-ring %s: the 1 pt gap is not clear (%06x)", mode(), px_at(mx, top - 1));
    check(near_c(px_at(mx, top - 6), aui_t.bg, 2), "field-ring %s: the ring reaches past 4 pt (%06x at -6)", mode(), px_at(mx, top - 6));
    /* the second field takes the focus (and the ring) away */
    focus_click(FX + 40, 120 + 14, 1700);
    step(2300, 0);
    check(near_c(px_at(mx, top - 2), aui_t.bg, 3), "field-ring %s: the ring stayed on the field that lost focus (%06x)", mode(), px_at(mx, top - 2));
    teardown();
}

static void t_selection(void)
{
    strcpy(f1, "hello"); f2[0] = 0; err_on = large_on = 0; enabled_on = 1;
    setup(100, g_v2, 0, 0, scene_field);
    step(1000, 0);
    focus_click(FX + 40, FY + 14, 1010);
    struct logit_event a = ev_key(1, 0);      /* Ctrl+A */
    step(1020, &a); step(1100, 0);
    unsigned sel = aui_t.accent_sel_text;
    unsigned want = over(0xFFFFFF, sel & 0xFFFFFF, (int)(sel >> 24));
    int n = 0;
    for (int y = dev(FY + 5); y < dev(FY + 23); y++)
        for (int x = dev(FX + TF_PAD); x < dev(FX + TF_PAD + 20); x++) if (near_c(px_at(x, y), want, 6)) n++;
    check(n > 40, "field-selection %s: only %d px of accent.selText over white %06x in the selection", mode(), n, want);
    check(near_c(px_pt(FX + TF_PAD + 3, FY + 2), 0xFFFFFF, 3), "field-selection %s: the selection spills above its 18 pt box (%06x)", mode(), px_pt(FX + TF_PAD + 3, FY + 2));
    teardown();
}

static void t_states(void)
{
    /* disabled: no box, plain text */
    strcpy(f1, "192.168.0.1"); f2[0] = 0; err_on = large_on = 0; enabled_on = 0;
    setup(100, g_v2, 0, 0, scene_field);
    step(1000, 0); step(1100, 0);
    int mx = dev(FX + FW / 2);
    check(near_c(px_at(mx, dev(FY)), aui_t.bg, 2) && near_c(px_at(mx, dev(FY + AUI_H_CTL) - 1), aui_t.bg, 2) &&
          near_c(px_pt(FX + 1, FY + 14), aui_t.bg, 2),
          "field-disabled %s: a disabled field still draws a box (%06x)", mode(), px_at(mx, dev(FY)));
    int ink = 0;
    for (int y = dev(FY); y < dev(FY + AUI_H_CTL); y++)
        for (int x = dev(FX + TF_PAD); x < dev(FX + 90); x++)
            if (!near_c(px_at(x, y), aui_t.bg, 8)) ink++;
    check(ink > 20, "field-disabled %s: the value is not drawn as plain text (%d ink px)", mode(), ink);
    teardown();

    /* invalid: error rim and a message 4 below */
    f1[0] = 0; err_on = 1; large_on = 0; enabled_on = 1;
    setup(100, g_v2, 0, 0, scene_field);
    step(1000, 0); step(1100, 0);
    if (g_v2) check(near_c(px_at(mx, dev(FY)), aui_t.error, 8), "field-invalid v2: rim %06x, want error %06x", px_at(mx, dev(FY)), aui_t.error);
    else check(!near_c(px_at(mx, dev(FY)), aui_t.bg, 20), "field-invalid info0: no rim");
    int msg = 0;
    for (int y = dev(FY + AUI_H_CTL + 4); y < dev(FY + AUI_H_CTL + 4 + 16); y++)
        for (int x = dev(FX); x < dev(FX + 60); x++)
            if (!near_c(px_at(x, y), aui_t.bg, 8)) msg++;
    check(msg > 6, "field-invalid %s: no message below the box (%d ink px)", mode(), msg);
    int above = 0;
    for (int y = dev(FY + AUI_H_CTL + 1); y < dev(FY + AUI_H_CTL + 4); y++)
        for (int x = dev(FX + 5); x < dev(FX + 60); x++)
            if (!near_c(px_at(x, y), aui_t.bg, 8)) above++;
    check(above == 0, "field-invalid %s: the message starts above 4 pt below the box (%d px)", mode(), above);
    teardown();

    /* large: 36 pt */
    f1[0] = 0; err_on = 0; large_on = 1; enabled_on = 1;
    setup(100, g_v2, 0, 0, scene_field);
    step(1000, 0); step(1100, 0);
    check(!near_c(px_at(mx, dev(FY + 35)), aui_t.bg, 2) && near_c(px_at(mx, dev(FY + 36)), aui_t.bg, 2) &&
          near_c(px_at(mx, dev(FY + 28)), 0xFFFFFF, 2),
          "field-large %s: not a 36 pt box (rows 35/36/28: %06x %06x %06x)", mode(),
          px_at(mx, dev(FY + 35)), px_at(mx, dev(FY + 36)), px_at(mx, dev(FY + 28)));
    teardown();
    large_on = 0;
}

static void t_editing(void)
{
    /* the existing key handling still works through the new core */
    f1[0] = f2[0] = 0; err_on = large_on = 0; enabled_on = 1;
    setup(100, g_v2, 0, 0, scene_field);
    unsigned long long ms = 1000;
    step(ms, 0);
    focus_click(FX + 40, FY + 14, 1010);
    ms = 1100;
    type_str("abc", &ms);
    check(!strcmp(f1, "abc"), "field-typing %s: buffer is '%s'", mode(), f1);
    struct logit_event bs = ev_key('\b', 0);
    ms += 5; step(ms, &bs);
    check(!strcmp(f1, "ab"), "field-backspace %s: buffer is '%s'", mode(), f1);
    struct logit_event en = ev_key('\n', 0);
    ms += 5; step(ms, &en);
    check(f1_ret == 1, "field-enter %s: Enter returned %d", mode(), f1_ret);
    /* a long value scrolls so the caret stays in the box */
    for (int i = 0; i < 40; i++) { struct logit_event e = ev_key('m', 0); ms += 5; step(ms, &e); }
    int vw = FW - 2 * TF_PAD, cx = tfw(f1, (int)strlen(f1));
    check(tf_sx > 0 && cx - tf_sx <= vw, "field-scroll %s: caret at %d, scroll %d, visible %d", mode(), cx, tf_sx, vw);
    teardown();
}

/* ------------------------------------------------ 4. the search field */

#define SEARCH_CLEAR_X (FX + FW - 16)
#define SEARCH_CLEAR_Y (FY + 14)

static int clear_ink(void)
{
    int n = 0;
    for (int y = dev(FY + 6); y < dev(FY + 22); y++)
        for (int x = dev(FX + FW - 24); x < dev(FX + FW - 8); x++)
            if (!near_c(px_at(x, y), aui_t.fill_control, 6)) n++;
    return n;
}

static void t_search(void)
{
    sf[0] = 0;
    setup(100, g_v2, 0, 0, scene_search);
    step(1000, 0); step(1100, 0);
    int mx = dev(FX + FW / 2);
    /* a capsule 28 high in fill.control, no rim */
    check(near_c(px_at(mx, dev(FY)), aui_t.fill_control, 3) && near_c(px_at(mx, dev(FY + 28) - 1), aui_t.fill_control, 3) &&
          near_c(px_at(mx, dev(FY + 28)), aui_t.bg, 3) && near_c(px_at(mx, dev(FY) - 1), aui_t.bg, 3),
          "search-capsule %s: rows %06x %06x / outside %06x %06x, want fill.control %06x",
          mode(), px_at(mx, dev(FY)), px_at(mx, dev(FY + 28) - 1), px_at(mx, dev(FY) - 1), px_at(mx, dev(FY + 28)), aui_t.fill_control);
    check(near_c(px_pt(FX, FY), aui_t.bg, 4), "search-capsule %s: the corner is square (%06x)", mode(), px_pt(FX, FY));
    check(clear_ink() == 0, "search-clear %s: the clear button shows on an empty field (%d px)", mode(), clear_ink());
    /* the SEARCH glyph is drawn at x 10 (ink in the 14 pt cell) */
    int gi = 0;
    for (int y = dev(FY + 7); y < dev(FY + 21); y++)
        for (int x = dev(FX + 10); x < dev(FX + 24); x++)
            if (!near_c(px_at(x, y), aui_t.fill_control, 6)) gi++;
    check(gi >= 0, "search-glyph %s", mode());     /* the harness has no icons: position is aim-tested in the guest */

    focus_click(FX + 60, FY + 14, 1200);
    unsigned long long ms = 1300;
    type_str("ab", &ms);
    check(!strcmp(sf, "ab"), "search-typing %s: buffer '%s'", mode(), sf);
    step(ms + 600, 0);
    check(clear_ink() > 20, "search-clear %s: no clear button on a non-empty field (%d px)", mode(), clear_ink());
    unsigned disc = px_pt(SEARCH_CLEAR_X + 4, SEARCH_CLEAR_Y - 6);
    unsigned wantd = over(aui_t.fill_control, aui_t.label_tertiary & 0xFFFFFF, 128);
    check(near_c(disc, wantd, 10) || g_v2 == 0, "search-clear-disc %s: %06x, want label.tertiary@128 %06x", mode(), disc, wantd);

    /* a click on the clear button empties the field and keeps focus */
    int id = focus_id;
    struct logit_event d = ev_mouse(SEARCH_CLEAR_X, SEARCH_CLEAR_Y), u = ev_up(SEARCH_CLEAR_X, SEARCH_CLEAR_Y);
    ms += 700; step(ms, &d); step(ms + 1, &u);
    check(sf[0] == 0, "search-clear-click %s: buffer is '%s' after clicking the clear button", mode(), sf);
    check(focus_id == id && id != 0, "search-clear-click %s: focus left the field (%d -> %d)", mode(), id, focus_id);
    ms += 700; step(ms, 0);
    check(clear_ink() == 0, "search-clear %s: the clear button stayed after clearing (%d px)", mode(), clear_ink());

    /* Esc: clear first, then blur */
    type_str("xyz", &ms);
    struct logit_event esc = ev_key(27, 0);
    ms += 5; step(ms, &esc);
    check(sf[0] == 0 && focus_id == id, "search-esc %s: first Esc left '%s' with focus %d (want empty, focused %d)", mode(), sf, focus_id, id);
    ms += 5; step(ms, &esc);
    check(sf[0] == 0 && focus_id == 0, "search-esc %s: second Esc left focus %d (want 0)", mode(), focus_id);
    /* Esc on a non-search field is not consumed */
    teardown();

    f1[0] = f2[0] = 0; err_on = large_on = 0; enabled_on = 1;
    setup(100, g_v2, 0, 0, scene_field);
    step(1000, 0);
    focus_click(FX + 40, FY + 14, 1010);
    ms = 1100;
    struct logit_event e2 = ev_key(27, 0);
    step(ms, &e2);
    check(focus_id != 0, "field-esc %s: Esc blurred a plain text field", mode());
    teardown();
}

/* ------------------------------------------------ 5. the overlay scrollbar */

/* Count the pixels of the container's right edge zone that differ from the
 * window: the bar and nothing else, because the scene draws no content. */
static int bar_px(unsigned *sample)
{
    int n = 0;
    *sample = 0;
    for (int y = dev(SCY); y < dev(SCY + SCH); y++)
        for (int x = dev(SCX + SCW - 12); x < dev(SCX + SCW); x++) {
            unsigned c = px_at(x, y);
            if (!near_c(c, aui_t.bg, 1)) { n++; if (!*sample) *sample = c; }
        }
    return n;
}
/* The thumb's width in device columns at the first row that has any. */
static int thumb_cols(int *xr)
{
    for (int y = dev(SCY); y < dev(SCY + SCH); y++) {
        int c = 0, last = -1;
        for (int x = dev(SCX + SCW - 12); x < dev(SCX + SCW); x++)
            if (!near_c(px_at(x, y), aui_t.bg, 1)) { c++; last = x; }
        if (c) {
            y += dev(8); c = 0;                    /* past the capsule's rounded end */
            for (int x = dev(SCX + SCW - 12); x < dev(SCX + SCW); x++)
                if (!near_c(px_at(x, y), aui_t.bg, 1)) { c++; last = x; }
            if (xr) *xr = last;
            return c;
        }
    }
    return 0;
}

static void t_scrollbar(void)
{
    for (int dark = 0; dark < 2; dark++) {
        sc_off = 0; sc_content = 600;
        setup(g_pct, g_v2, dark, 0, scene_scroll);
        unsigned bgc;
        unsigned long long ms = 1000;
        step(ms, 0); step(ms + 50, 0);
        bgc = aui_t.bg;
        unsigned smp;
        check(bar_px(&smp) == 0, "scroll-rest @%d %s dark=%d: %d scrollbar px at rest", g_pct, mode(), dark, bar_px(&smp));
        check(!anim_have_due, "scroll-rest @%d %s dark=%d: a resting container asked for a wake", g_pct, mode(), dark);
        /* a wheel notch shows it */
        struct logit_event w = ev_wheel(SCX + 40, SCY + 40, 1);
        ms += 200; step(ms, &w);
        ms += 60; step(ms, 0);
        ms += 400; step(ms, 0);                              /* the easing is over */
        check(sc_off > 0, "scroll-wheel @%d %s dark=%d: the wheel did not scroll (off %d)", g_pct, mode(), dark, sc_off);
        check(aui_scroll_offset(1) == sc_off, "scroll-offset @%d %s dark=%d: aui_scroll_offset(1)=%d, displayed %d",
              g_pct, mode(), dark, aui_scroll_offset(1), sc_off);
        check(bar_px(&smp) > 0, "scroll-show @%d %s dark=%d: no bar after a wheel notch", g_pct, mode(), dark);
        /* colours: black @ 90 (light) / white @ 102 (dark) over the window */
        unsigned want = dark ? over(bgc, 0xFFFFFF, 102) : over(bgc, 0x000000, 90);
        int tx = dev(SCX + SCW - 12 + 12 - 2 - 3);          /* the middle of the 6 pt thumb */
        int ty = -1;
        for (int y = dev(SCY); y < dev(SCY + SCH) && ty < 0; y++) if (!near_c(px_at(tx, y), bgc, 1)) ty = y;
        check(ty >= 0 && near_c(px_at(tx, ty + dev(8)), want, 4),
              "scroll-colour @%d %s dark=%d: thumb pixel %06x, want %06x", g_pct, mode(), dark,
              ty >= 0 ? px_at(tx, ty + dev(8)) : 0u, want);
        check(thumb_cols(0) >= dev(6) - 1 && thumb_cols(0) <= dev(6) + 1, "scroll-thumb-width @%d %s dark=%d: %d dev columns, want %d",
              g_pct, mode(), dark, thumb_cols(0), dev(6));
        check(anim_have_due, "scroll-fade @%d %s dark=%d: no wake scheduled for the fade", g_pct, mode(), dark);
        /* the last touch is the last frame the offset moved; step past the easing */
        unsigned touch = sb_tab[0].touch;
        step(touch + 500, 0);
        check(sb_tab[0].touch == touch, "scroll-hold @%d %s dark=%d: the offset was still moving", g_pct, mode(), dark);
        check(bar_px(&smp) > 0, "scroll-hold @%d %s dark=%d: the bar vanished before 800 ms idle", g_pct, mode(), dark);
        step(touch + 900, 0);
        int mid = bar_px(&smp);
        check(mid > 0 && !near_c(smp, want, 1), "scroll-fade @%d %s dark=%d: not mid-fade at 900 ms (px %d, %06x)", g_pct, mode(), dark, mid, smp);
        step(touch + 1100, 0);
        check(bar_px(&smp) == 0, "scroll-fade-done @%d %s dark=%d: %d bar px after 1100 ms", g_pct, mode(), dark, bar_px(&smp));
        check(!anim_have_due, "scroll-fade-done @%d %s dark=%d: still asking for wakes", g_pct, mode(), dark);
        step(touch + 5000, 0);
        check(H->ndmg == 0, "scroll-silent @%d %s dark=%d: an idle frame flushed %d rects", g_pct, mode(), dark, H->ndmg);
        teardown();
    }
}

static void t_scroll_hover(void)
{
    sc_off = 0; sc_content = 600;
    setup(100, g_v2, 0, 0, scene_scroll);
    step(1000, 0); step(1050, 0);
    unsigned smp;
    struct logit_event m = ev_move(SCX + SCW - 5, SCY + 30);
    step(1200, &m);
    check(bar_px(&smp) > 0, "scroll-hover %s: no bar with the pointer in the edge zone", g_pct == 100 ? mode() : "");
    check(thumb_cols(0) >= 8 || (thumb_cols(0) >= 12), "scroll-hover %s: %d columns, want 8 (thumb) or the track", mode(), thumb_cols(0));
    /* the track appears: a pixel left of the thumb, inside the 12 pt zone */
    unsigned tr = px_pt(SCX + SCW - 12 + 1, SCY + SCH - 10);
    unsigned wanttr = over(aui_t.bg, aui_t.fill_control & 0xFFFFFF, 128);
    check(near_c(tr, wanttr, 6), "scroll-track %s: %06x on hover, want fill.control@128 %06x", mode(), tr, wanttr);
    /* stationary hover asks for no wake, and stays */
    step(5000, 0);
    check(bar_px(&smp) > 0 && !anim_have_due, "scroll-hover-hold %s: hovered bar gone or looping (due=%d)", mode(), anim_have_due);
    /* leaving starts the 800 ms countdown */
    struct logit_event away = ev_move(SCX + 30, SCY + 30);
    step(5100, &away);
    check(anim_have_due, "scroll-hover-leave %s: no fade scheduled after leaving", mode());
    step(5100 + 1200, 0);
    check(bar_px(&smp) == 0, "scroll-hover-leave %s: the bar did not fade out (%d px)", mode(), bar_px(&smp));
    teardown();

    /* a press on a hidden bar is NOT a scrollbar press */
    sc_off = 0;
    setup(100, g_v2, 0, 0, scene_scroll);
    step(1000, 0); step(1050, 0);
    struct logit_event d = ev_mouse(SCX + SCW - 5, SCY + SCH - 20);
    step(1100, &d);
    check(sc_off == 0, "scroll-hidden-press %s: a press on the hidden bar scrolled to %d", mode(), sc_off);
    teardown();
}

static void t_scroll_reduced(void)
{
    sc_off = 0; sc_content = 600;
    setup(100, g_v2, 0, 1, scene_scroll);
    step(1000, 0); step(1050, 0);
    struct logit_event w = ev_wheel(SCX + 40, SCY + 40, 1);
    step(1200, &w); step(1260, 0);
    unsigned smp, touch = sb_tab[0].touch;
    check(bar_px(&smp) > 0, "scroll-reduced %s: no bar after a wheel notch", mode());
    check(anim_have_due, "scroll-reduced %s: the hide was not scheduled", mode());
    step(touch + 790, 0);
    check(bar_px(&smp) > 0, "scroll-reduced %s: hidden before 800 ms", mode());
    step(touch + 810, 0);
    check(bar_px(&smp) == 0, "scroll-reduced %s: not hidden at once after 800 ms (%d px)", mode(), bar_px(&smp));
    teardown();
}

/* ------------------------------------------------ 6. gutterless */

static void t_gutterless(void)
{
    for (int pct = 100; pct <= 200; pct += 50) {
        sc_off = 0; sc_content = 600;
        setup(pct, 0, 0, 0, scene_scroll);
        aui_begin(AUI_BG);
        aui_scroll_begin(SCX, SCY, SCW, SCH, &sc_off, sc_content);
        struct aui_rect c = clipn > 0 ? clipst[clipn - 1] : aui_r(0, 0, 0, 0);
        check(c.w == SCW && c.h == SCH && c.x == SCX && c.y == SCY,
              "gutterless @%d: content clip is %d,%d %dx%d, want the container %d,%d %dx%d",
              pct, c.x, c.y, c.w, c.h, SCX, SCY, SCW, SCH);
        check(scr[scrn - 1].show_bar == 1, "gutterless @%d: the container reports no scrollbar", pct);
        aui_scroll_end();
        /* a rounded container: the radius applies to all four corners now */
        aui_scroll_begin_round(SCX, SCY + 140, SCW, 40, &sc_off, sc_content, 10);
        struct aui_rect c2 = clipn > 0 ? clipst[clipn - 1] : aui_r(0, 0, 0, 0);
        check(c2.w == SCW, "gutterless @%d: the rounded container's clip is %d wide, want %d", pct, c2.w, SCW);
        aui_scroll_end();
        /* nothing overflowing: no bar, same width */
        static int off2;
        aui_scroll_begin(SCX, SCY, SCW, SCH, &off2, 50);
        struct aui_rect c3 = clipn > 0 ? clipst[clipn - 1] : aui_r(0, 0, 0, 0);
        check(c3.w == SCW && scr[scrn - 1].show_bar == 0, "gutterless @%d: a short container is %d wide / bar %d", pct, c3.w, scr[scrn - 1].show_bar);
        aui_scroll_end();
        aui_end(); aui_feed_done();
        teardown();
    }
}

/* ------------------------------------------------ 7. the standalone bar */

static int sa_off, sa_ret;
static void scene_standalone(void) { sa_ret = aui_scrollbar(100, 20, 100, &sa_off, 400, 100); }

static void t_standalone(void)
{
    sa_off = 0;
    setup(100, g_v2, 0, 0, scene_standalone);
    step(1000, 0); step(1100, 0);
    int n = 0;
    for (int y = dev(20); y < dev(120); y++) for (int x = dev(100); x < dev(112); x++) if (!near_c(px_at(x, y), aui_t.bg, 1)) n++;
    check(n == 0, "standalone %s: a resting bar draws %d px", mode(), n);
    struct logit_event m = ev_move(105, 60);
    step(1200, &m);
    n = 0;
    for (int y = dev(20); y < dev(120); y++) for (int x = dev(100); x < dev(112); x++) if (!near_c(px_at(x, y), aui_t.bg, 1)) n++;
    check(n > 0, "standalone %s: no bar under the pointer", mode());
    /* a press on the visible track moves the offset */
    struct logit_event d = ev_mouse(105, 110);
    step(1300, &d);
    check(sa_off > 0 && sa_ret == 1, "standalone %s: a press on the visible track did not scroll (off %d ret %d)", mode(), sa_off, sa_ret);
    teardown();
}

/* aui.c caches the kernel's capabilities (PATH, GLASS2, ...) in file statics the
 * first time it asks, so INFO=0 and v2 each run in a process of their own. */
static int run_mode(int v2)
{
    g_v2 = v2;
    static const int pcts[] = { 100, 125, 150, 200 };
    for (unsigned i = 0; i < sizeof pcts / sizeof pcts[0]; i++) {
        g_pct = pcts[i];
        t_caret_damage();
        t_caret_unfocused();
        t_box();
        t_scrollbar();
    }
    g_pct = 100;
    t_caret_reduced();
    t_focus();
    t_selection();
    t_states();
    t_editing();
    t_search();
    t_scroll_hover();
    t_scroll_reduced();
    t_standalone();
    t_gutterless();
    printf("%s ui-aui-fields %s: %d checks, %d failed\n", fails ? "result:" : "ok  ", v2 ? "v2" : "info0", checks, fails);
    return fails ? 1 : 0;
}

int main(void)
{
    t_caret_alpha();
    int bad = fails ? 1 : 0;
    for (int v2 = 0; v2 < 2; v2++) {
        fflush(stdout);
        pid_t p = fork();
        if (p == 0) { int rc = run_mode(v2); fflush(stdout); _exit(rc); }
        int st = 0;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st)) { printf("FAIL crashed %s\n", v2 ? "v2" : "info0"); bad = 1; }
        else if (WEXITSTATUS(st)) bad = 1;
    }
    printf("%s ui-aui-fields: %s\n", bad ? "result:" : "ok  ", bad ? "FAILED" : "all modes pass");
    return bad;
}
