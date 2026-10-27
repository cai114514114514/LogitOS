/* ui-aui-motion-api: the spring presets, press scale and stagger of design 3.8
 * (P6), driven on the host against the REAL aui.c (included, so the frame clock
 * and the slot table can be set directly, no window manager).
 *
 * Every preset is stepped at 8, 16 and 33 ms frames. A frame is simulated like
 * aui_motion_test.c's: set frame_ms, bump the generation, enter "widget 1".
 * Failures print "FAIL <check>"; the exit status is non-zero on any.
 * -DUI_MOTION_NEGCTL (in aui_motion.inc) makes the bouncy preset critically
 * damped (zeta 1.0) and must fail 'bouncy-overshoot' and nothing else. */
#include <stdio.h>
#include "aui.c"

long uam_sys(long n, long a, long b, long c)
{ (void)n; (void)a; (void)b; (void)c; return 0; }

static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

static void frame(unsigned ms, int wid)
{
    frame_ms = ms; anim_gen++; anim_live = 0; id_ctr = wid;
}

struct run { int settle_ms; int over24; int wake_after; int live_wait; int quiet_wait; int quiet_due; int final; };

/* Step a preset from 0 to step24 sampling every dt ms for 1.5 s. settle_ms is
 * the time at which the first frame that no longer requests another lands. */
static struct run run_preset(enum aui_spring p, int step24, unsigned dt, int wid)
{
    struct run r = {-1, 0, 0, 0, 0, 0, 0};
    unsigned ms = 100000;
    aui_anim_reset(); anim_reduced = 0; anim_gen = 10;
    frame(ms, wid); aui_spring_ex(AUI_AK_VALUE, 0, p);              /* fresh: latched at 0 */
    unsigned t0 = ms + dt;
    int x = 0;
    for (ms = t0; ms < t0 + 1500; ms += dt) {
        frame(ms, wid);
        x = aui_spring_ex(AUI_AK_VALUE, step24, p);
        if (x - step24 > r.over24) r.over24 = x - step24;
        anim_schedule();                                            /* what aui_end() does */
        if (anim_live) {
            if (r.settle_ms >= 0) r.wake_after = 1;
            if (aui_anim_wait() > 0) r.live_wait = 1;
        } else if (r.settle_ms < 0) {
            r.settle_ms = (int)(ms - t0);   /* the move began at t0 - dt; this is the last frame that still moved */
            r.quiet_wait = aui_anim_wait(); r.quiet_due = aui_anim_due();
        }
    }
    r.final = x;
    return r;
}

static const char *pname[AUI_SPRING_COUNT] = { "snappy", "smooth", "bouncy", "gentle", "tick" };

static void t_presets(void)
{
    /* settle limits at a 10 pt step (a switch knob's travel), design 3.8 */
    static const int settle_lim[AUI_SPRING_COUNT] = { 140, 260, 360, 420, 190 };
    static const unsigned dts[3] = { 16, 8, 33 };
    char what[64];
    for (int p = 0; p < AUI_SPRING_COUNT; p++) {
        for (int di = 0; di < 3; di++) {
            unsigned dt = dts[di];
            struct run r = run_preset((enum aui_spring)p, 10 * 256, dt, 1);
            /* settle_ms is the elapsed time of the last frame that still asked for another */
            snprintf(what, sizeof what, "%s-settle", pname[p]);
            check(r.settle_ms >= 0 && r.settle_ms <= settle_lim[p], what);
            snprintf(what, sizeof what, "%s-quiet", pname[p]);
            check(r.settle_ms >= 0 && !r.wake_after && r.quiet_wait == 0 && !r.quiet_due && r.live_wait, what);
            snprintf(what, sizeof what, "%s-lands", pname[p]);
            check(r.final == 10 * 256, what);
            if (dt == 16)
                printf("%-6s 10pt step @%2ums: settled %3d ms (limit %d)\n", pname[p], dt, r.settle_ms, settle_lim[p]);
        }
    }
}

static void t_overshoot(void)
{
    /* overshoot as a fraction of the step, in permille; the step is 100 pt
     * (6 deg for the tick) so 0.4 % is 100+ units of 24.8 and not rounding */
    static const unsigned dts[3] = { 16, 8, 33 };
    for (int di = 0; di < 3; di++) {
        unsigned dt = dts[di];
        struct run sn = run_preset(AUI_SPRING_SNAPPY, 100 * 256, dt, 1);
        struct run sm = run_preset(AUI_SPRING_SMOOTH, 100 * 256, dt, 1);
        struct run bo = run_preset(AUI_SPRING_BOUNCY, 100 * 256, dt, 1);
        struct run ge = run_preset(AUI_SPRING_GENTLE, 100 * 256, dt, 1);
        struct run ti = run_preset(AUI_SPRING_TICK, 6 * 256, dt, 1);
        int sn_pm = sn.over24 * 1000 / (100 * 256), bo_pm = bo.over24 * 1000 / (100 * 256);
        int ti_pm = ti.over24 * 1000 / (6 * 256);
        check(sn_pm <= 10, "snappy-overshoot");           /* <= 1 % */
        check(sm.over24 == 0, "smooth-overshoot");        /* none */
        check(bo_pm >= 60 && bo_pm <= 100, "bouncy-overshoot");
        check(ge.over24 <= 1, "gentle-overshoot");         /* none: 1/256 pt of integer rounding at most */
        /* the tick peaks ~58 ms in: 8 and 16 ms frames land on it (design: 15-25 %);
         * a 33 ms frame samples 8 ms past it, so there the floor is the sampled 14 % */
        check(ti_pm >= (dt <= 16 ? 150 : 140) && ti_pm <= 250, "tick-overshoot");
        printf("tick @%2ums: %d.%d%%\n", dt, ti_pm / 10, ti_pm % 10);
        if (dt == 16)
            printf("overshoot @16ms: snappy %d.%d%% smooth %d bouncy %d.%d%% gentle %d tick %d.%d%% of 6deg\n",
                   sn_pm / 10, sn_pm % 10, sm.over24, bo_pm / 10, bo_pm % 10, ge.over24, ti_pm / 10, ti_pm % 10);
    }
}

/* aui_spring() IS snappy now: same trajectory sample for sample as the preset */
static void t_default(void)
{
    aui_anim_reset(); anim_reduced = 0; anim_gen = 10;
    unsigned ms = 7000; int same = 1;
    frame(ms, 1); aui_spring(AUI_AK_VALUE, 0);
    id_ctr = 2;   aui_spring_ex(AUI_AK_VALUE, 0, AUI_SPRING_SNAPPY);
    for (int i = 0; i < 80; i++) {
        ms += 16; frame(ms, 1);
        int a = aui_spring(AUI_AK_VALUE, 100 * 256);
        id_ctr = 2;
        int b = aui_spring_ex(AUI_AK_VALUE, 100 * 256, AUI_SPRING_SNAPPY);
        if (a != b) same = 0;
    }
    check(same, "default-snappy");
}

static void t_reduced(void)
{
    for (int p = 0; p < AUI_SPRING_COUNT; p++) {
        aui_anim_reset(); anim_gen = 10; anim_reduced = 1;
        frame(100, 1); aui_spring_ex(AUI_AK_VALUE, 0, (enum aui_spring)p);
        frame(116, 1);
        int x = aui_spring_ex(AUI_AK_VALUE, 5000, (enum aui_spring)p);
        anim_schedule();
        check(x == 5000 && !anim_live && aui_anim_wait() == 0, "rm-one-frame");
    }
    aui_anim_reset(); anim_gen = 10; anim_reduced = 1;
    frame(100, 1);
    check(aui_press_scale_q8(0, 1) == 248, "rm-press");
    frame(116, 1);
    check(aui_press_scale_q8(0, 0) == 256 && !anim_live, "rm-press-release");
    check(aui_stagger_ms(5, 14) == 0, "rm-stagger");
    anim_reduced = 0;
}

static void t_press(void)
{
    aui_anim_reset(); anim_reduced = 0; anim_gen = 10;
    unsigned ms = 20000;
    frame(ms, 1);  int s = aui_press_scale_q8(0, 0);
    check(s == 256, "press-rest");
    ms += 10; frame(ms, 1); aui_press_scale_q8(0, 1);                /* leg starts */
    unsigned t0 = ms, prev = 256; int mono = 1, mid = 0;
    for (ms = t0 + 10; ms <= t0 + 200; ms += 10) {
        frame(ms, 1); s = aui_press_scale_q8(0, 1);
        if ((unsigned)s > prev) mono = 0;
        prev = (unsigned)s;
        if (ms == t0 + 30) mid = s;
        if (ms == t0 + 60) check(s == LOGIT_PRESS_SCALE_Q8 && s == 248, "press-248-at-60ms");
        if (ms > t0 + 60) check(!anim_live, "press-held-quiet");
    }
    check(mono && mid > 248 && mid < 256, "press-linear-down");
    /* release: snappy back to 256, settles, quiet */
    int settled_at = -1; int over = 0; unsigned r0 = ms;
    for (int i = 0; i < 100; i++) {
        ms += 16; frame(ms, 1); s = aui_press_scale_q8(0, 0);
        if (s > 256) over = 1;
        if (!anim_live && settled_at < 0) settled_at = (int)(ms - r0);
    }
    check(s == 256 && settled_at > 0 && settled_at <= 140 + 16, "press-release-snappy");
    check(!over || s == 256, "press-release-lands");
    /* dock tile */
    aui_anim_reset(); ms += 100; frame(ms, 1); aui_press_scale_dock_q8(0, 0);
    ms += 16; frame(ms, 1); aui_press_scale_dock_q8(0, 1);
    unsigned d0 = ms; ms += 60; frame(ms, 1);
    s = aui_press_scale_dock_q8(0, 1);
    check(s == LOGIT_PRESS_SCALE_DOCK_Q8 && s == 236 && ms - d0 == 60, "press-dock-236");
    /* interrupted press reverses from where it is, never snaps */
    aui_anim_reset(); ms += 100; frame(ms, 1); aui_press_scale_q8(0, 0);
    ms += 16; frame(ms, 1); aui_press_scale_q8(0, 1);
    ms += 30; frame(ms, 1); int a = aui_press_scale_q8(0, 1);
    ms += 16; frame(ms, 1); int b = aui_press_scale_q8(0, 0);
    check(a < 256 && b <= 256 && b >= a - 1 && b - a <= 4, "press-interrupt-continuous");
}

static void t_stagger(void)
{
    anim_reduced = 0;
    check(aui_stagger_ms(0, 14) == 0 && aui_stagger_ms(1, 14) == 14 && aui_stagger_ms(5, 8) == 40, "stagger");
    check(aui_stagger_ms(-3, 14) == 0 && aui_stagger_ms(3, 0) == 0, "stagger-clamp");
}

int main(void)
{
    t_presets(); t_overshoot(); t_default(); t_reduced(); t_press(); t_stagger();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
