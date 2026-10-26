/* aui-motion-adopt: curve catalog, spring and premultiplied colour tween,
 * driven on the host against the REAL aui.c (included, so its statics -- the
 * frame clock, the slot table -- can be set directly, no window manager).
 * Failures print "FAIL <check>"; the exit status is non-zero on any.
 * -DAUI_MOTION_LEGACY3 restores the old three-curve map and must fail 'curve'. */
#include <stdio.h>
#include "aui.c"

long aui_mt_sys(long n, long a, long b, long c)
{ (void)n; (void)a; (void)b; (void)c; return 0; }

static int checks, fails;
static void check(int ok, const char *what)
{ checks++; if (!ok) { fails++; printf("FAIL %s\n", what); } }

/* one simulated frame at clock ms, inside "widget" 1 */
static void frame(unsigned ms)
{
    frame_ms = ms; anim_gen++; anim_live = 0; id_ctr = 1;
}

static const int all_curves[] = {
    AUI_EASE_LINEAR, AUI_EASE_OUT, AUI_EASE_INOUT, AUI_EASE_IN, AUI_EASE_OUT_CUBIC,
    AUI_EASE_IN_CUBIC, AUI_EASE_INOUT_CUBIC, AUI_EASE_OUT_QUART, AUI_EASE_OUT_EXPO,
    AUI_EASE_OUT_BACK, AUI_EASE_INOUT_BACK, AUI_EASE_STANDARD, AUI_EASE_EMPHASIZED,
    AUI_EASE_DECEL, AUI_EASE_BACK };

static struct ol_bezier preset(int c)
{
    static const struct ol_bezier b[4] = {
        { OL_FX(0.2), OL_FX(0.0), OL_FX(0.0), OL_FX(1.0) },
        { OL_FX(0.05), OL_FX(0.7), OL_FX(0.1), OL_FX(1.0) },
        { OL_FX(0.0), OL_FX(0.0), OL_FX(0.2), OL_FX(1.0) },
        { OL_FX(0.34), OL_FX(1.56), OL_FX(0.64), OL_FX(1.0) } };
    return b[c - AUI_EASE_STANDARD];
}

static void t_curves(void)
{
    int bad = 0, n = 0;
    for (unsigned ci = 0; ci < sizeof all_curves / sizeof *all_curves; ci++) {
        int c = all_curves[ci];
        aui_anim_reset(); anim_reduced = 0; anim_gen = 10;
        frame(1000); aui_anim(AUI_AK_VALUE, 0, 200, c);            /* fresh: latched at 0 */
        frame(1010); aui_anim(AUI_AK_VALUE, 255, 200, c);          /* leg starts at t0 = 1010 */
        for (unsigned el = 10; el < 200; el += 10) {
            frame(1010 + el);
            int got = aui_anim(AUI_AK_VALUE, 255, 200, c);
            int t = (int)((double)el * 256 / 200.0), e;
            if (c >= AUI_EASE_STANDARD) { struct ol_bezier b = preset(c); e = ol_ease256_ex(OL_CUBIC_BEZIER, t, &b, 0); }
            else e = ol_ease256_ex((enum ol_curve)c, t, 0, 0);
            int want = (int)(255LL * e / 256);
            if (want < 0) want = 0; if (want > 255) want = 255;
            n++;
            if (got != want) { if (!bad) printf("curve %d el %u: got %d want %d\n", c, el, got, want); bad++; }
        }
    }
    check(!bad, "curve");
    printf("curve identity: %d samples, %d mismatches\n", n, bad);
}

static void t_spring(void)
{
    aui_anim_reset(); anim_reduced = 0; anim_gen = 10;
    unsigned ms = 5000; int target = 100 * 256;
    frame(ms); int x = aui_spring(AUI_AK_VALUE, 0);
    check(x == 0, "spring fresh latches to target");
    int prev = x, maxjump = 0, settled_at = -1, over = 0, wake_after = 0;
    for (int i = 0; i < 6000; i++) {
        ms += 16; frame(ms);
        x = aui_spring(AUI_AK_VALUE, target);
        if (x > target) over = 1;
        int d = x > prev ? x - prev : prev - x;
        if (d > maxjump) maxjump = d;
        prev = x;
        if (!anim_live) { if (settled_at < 0) settled_at = i; }
        else if (settled_at >= 0) wake_after = 1;
    }
    check(x == target, "spring settle lands exactly on target");
    check(settled_at >= 0 && settled_at < 300, "spring settle stops requesting frames");
    check(!wake_after, "spring settle stays quiet");
    check(over, "spring settle underdamped overshoot visible in 24.8");
    /* no snap: the last live frame must be within 1/256 of the target, i.e. the
     * snap to target moves by less than one 24.8 unit */
    aui_anim_reset(); ms = 9000; frame(ms); aui_spring(AUI_AK_VALUE, 0);
    int last_live = 0;
    for (int i = 0; i < 6000; i++) {
        ms += 1; frame(ms);
        int v = aui_spring(AUI_AK_VALUE, target);
        if (anim_live) last_live = v; else break;
    }
    int snap = target - last_live; if (snap < 0) snap = -snap;
    if (snap > 1) printf("snap %d\n", snap);
    check(snap <= 1, "spring snap > 1/256");
    printf("spring: settled after %d frames, max step %d, final snap %d\n", settled_at, maxjump, snap);
}

static void t_reduced(void)
{
    aui_anim_reset(); anim_gen = 10; anim_reduced = 1;
    frame(100);
    check(aui_anim(AUI_AK_VALUE, 255, 200, AUI_EASE_OUT) == 255 && !anim_live, "reduced snaps aui_anim");
    frame(110);
    check(aui_spring(AUI_AK_FACE, 5000) == 5000 && !anim_live, "reduced snaps spring");
    frame(120);
    check(aui_anim_color(AUI_AK_FACE, 0xFFFF0000u, 0x00FF0000u) == 0x00FF0000u && !anim_live,
          "reduced snaps colour");
    anim_reduced = 0;
}

static void t_color(void)
{
    aui_anim_reset(); anim_reduced = 0; anim_gen = 10;
    uint32_t red = 0xFFFF0000u, clear = 0x00FF0000u;
    frame(2000); uint32_t c0 = aui_anim_color(AUI_AK_FACE, red, clear);
    check(c0 == red, "color starts at from");
    frame(2000 + AUI_T_FAST / 2); uint32_t c = aui_anim_color(AUI_AK_FACE, red, clear);
    int a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    printf("colour midpoint: a=%d r=%d g=%d b=%d\n", a, r, g, b);
    check(a >= 127 && a <= 129, "color alpha midpoint");
    check(r >= 254 && g <= 1 && b <= 1, "color premul rgb stays red");
    check(anim_live == 1, "color requests frames mid-tween");
    frame(2000 + AUI_T_FAST); c = aui_anim_color(AUI_AK_FACE, red, clear);
    check(c == clear && !anim_live, "color lands on target and goes quiet");
    /* the SDK mix itself, independent of the clock */
    uint32_t m = ol_mix_argb_premul(red, clear, 128);
    check((m >> 24) >= 127 && (m >> 24) <= 129 && ((m >> 16) & 255) >= 254, "color ol_mix_argb_premul");
}

static void t_slots(void)
{
    aui_anim_reset(); anim_reduced = 0; anim_gen = 10;
    for (int k = 0; k < AUI_ANIM_MAX + 8; k++) {
        frame(300 + k); id_ctr = 1 + k;
        aui_spring(AUI_AK_VALUE, k); aui_anim_color(AUI_AK_FACE, 0, 1);
    }
    int used = 0;
    for (int i = 0; i < AUI_ANIM_MAX; i++) used += anim_tab[i].id != 0;
    check(used == AUI_ANIM_MAX, "slot limit stays 32");
}

int main(void)
{
    t_curves(); t_spring(); t_reduced(); t_color(); t_slots();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
