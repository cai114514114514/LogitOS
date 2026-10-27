/* aui-shadow-adopt: aui_shadow_ex / aui_shadow2 / aui_elevation / aui_glow of
 * aui_paint.inc over the REAL SYS_GUI_SHADOW2 handler (tests/unit/aui_host/v2_fx.c
 * includes c/kernel/gui/wm_draw2_fx.c on a host env bound to the harness display).
 *
 *   elevation  level 0 issues none, levels 1..4 exactly two SHADOW2 calls
 *              (ambient, then key) with the documented parameters, light and
 *              dark                                         FAIL elevation-calls
 *   shadow2    one call, snapped device rect, fields passed through, inset flag
 *                                                           FAIL shadow2-call
 *   exact      a 20 pt sigma at 100/125/150/200% is pixel-identical to
 *              ol_display_shadow_ex driven directly with the device parameters,
 *              so nothing is clamped                        FAIL large-blur
 *   damage     every changed pixel lies in the flush set, and the set reaches
 *              3 sigma + offset                             FAIL damage
 *   legacy     INFO=0 issues no SHADOW2, still draws, an inset draws nothing
 *                                                           FAIL legacy
 *
 * Each render runs in a forked child (aui.c keeps its scale and capability in
 * file-scope statics). THE CONTROL (-DAUI_SHADOW_LEGACY) pretends the kernel
 * never reports SHADOW2: only the elevation check runs there, and it must report
 * 'FAIL elevation-calls'. The fallback's pixels are the legacy goldens
 * (make test-aui-host, INFO=0 mode). Exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.h"
#include "aui_host.h"
#include "openlogit_display_shadow.h"

extern struct logit_shadow2 aui_fx_log[8];
extern int aui_fx_nshadow;

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); bad = 1; } while (0)

#define MAXW 700
struct shot {
    int w, h, ndmg, status, nshadow, unhandled;
    long viol, reach_x0, reach_y0, reach_x1, reach_y1;
    struct logit_shadow2 log[8];
    struct aui_host_rect dmg[AUI_HOST_DMG_MAX];
    uint32_t px[MAXW * MAXW];
};
typedef void (*scene_fn)(struct aui_host *h, int arg);
static int g_dark;
static long g_viol;

static struct shot *render(int v2, int pct, int w_pt, int h_pt, scene_fn fn, int arg)
{
    struct shot *s = mmap(0, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct aui_host *h = aui_host_new(w_pt, h_pt, pct);
        h->dark = g_dark;
        if (v2) aui_host_enable_v2(h);
        gui_create("aui-shadow", w_pt, h_pt);
        aui_set_size(w_pt, h_pt);
        aui_round_a(-100, -100, 8, 8, 2, 0xFFFFFF, 128);     /* prime scale, theme and capability */
        aui_host_stats_reset(h);
        aui_fx_nshadow = 0;
        fn(h, arg);
        s->w = h->w; s->h = h->h;
        s->viol = g_viol;
        s->nshadow = aui_fx_nshadow;
        memcpy(s->log, aui_fx_log, sizeof s->log);
        s->unhandled = aui_host_unhandled_total(h);
        s->ndmg = h->ndmg;
        memcpy(s->dmg, h->dmg, sizeof s->dmg);
        memcpy(s->px, h->px, (size_t)h->w * h->h * 4);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    s->status = st;
    if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("harness child died (status 0x%x)", st);
    return s;
}

#ifndef AUI_SHADOW_LEGACY
static const int pcts[4] = { 100, 125, 150, 200 };
#endif
#define RECT aui_r(40, 36, 60, 40)

/* ------------------------------------------------------------- elevation */
static void sc_elev(struct aui_host *h, int level)
{
    (void)h;
    aui_begin(AUI_BG);
    aui_elevation(RECT, 8, level);
    aui_end();
}

static void check_elevation(void)
{
    /* light alpha of {key dy, key sigma, key alpha, ambient sigma, ambient alpha} */
    static const int tab[4][5] = { { 1, 2, 36, 4, 16 }, { 2, 4, 44, 8, 20 }, { 4, 8, 52, 14, 24 }, { 8, 14, 60, 24, 28 } };
    for (int dark = 0; dark < 2; dark++) {
        g_dark = dark;
        for (int level = 0; level <= 5; level++) {
            struct shot *s = render(1, 100, 160, 120, sc_elev, level);
            int want = level == 0 ? 0 : 2;
            if (s->nshadow != want) {
                FAIL("elevation-calls level %d dark %d: %d SYS_GUI_SHADOW2 calls, want %d", level, dark, s->nshadow, want);
            } else if (want) {
                const int *t = tab[(level > 4 ? 4 : level) - 1];
                int mul = dark ? 2 : 1;
                const struct logit_shadow2 *amb = &s->log[0], *key = &s->log[1];
                /* the shadow colour is the theme's: the parent only knows the light one */
                unsigned cm = dark ? 0xFF000000u : 0xFFFFFFFFu, col = dark ? 0 : (AUI_SHADOW & 0xFFFFFFu);
                if (amb->dx || amb->dy || amb->spread || amb->blur != t[3] * 256 || (amb->argb & cm) != (((unsigned)(t[4] * mul) << 24) | col) ||
                    key->dx || key->spread || key->dy != t[0] * 256 || key->blur != t[1] * 256 ||
                    (key->argb & cm) != (((unsigned)(t[2] * mul) << 24) | col) || (key->flags & LOGIT_SHADOW2_INSET) || (amb->flags & LOGIT_SHADOW2_INSET))
                    FAIL("elevation-calls level %d dark %d: parameters differ from the documented table "
                         "(ambient dy %d sigma %d argb %08x; key dy %d sigma %d argb %08x)", level, dark,
                         amb->dy, amb->blur, amb->argb, key->dy, key->blur, key->argb);
            }
            munmap(s, sizeof *s);
        }
    }
    g_dark = 0;
    if (!bad) printf("ok   elevation: level 0 draws nothing, 1..4 (and 5 -> 4) are exactly an ambient and a key SHADOW2, light and dark\n");
}

#ifndef AUI_SHADOW_LEGACY
/* ---------------------------------------------------------- shadow2 call */
static void sc_s2(struct aui_host *h, int inset)
{
    (void)h;
    aui_begin(AUI_BG);
    aui_shadow2(aui_r(21, 17, 50, 30), 6, -3, 5, 7, 2, 0x80336699u, inset);
    aui_end();
}

static void check_shadow2_call(void)
{
    for (int p = 0; p < 4; p++)
        for (int inset = 0; inset < 2; inset++) {
            int pct = pcts[p];
            struct shot *s = render(1, pct, 160, 120, sc_s2, inset);
            const struct logit_shadow2 *c = &s->log[0];
            int x0 = aui_host_pt(21, pct), x1 = aui_host_pt(71, pct), y0 = aui_host_pt(17, pct), y1 = aui_host_pt(47, pct);
            if (s->nshadow != 1 || s->unhandled) { FAIL("shadow2-call @%d%%: %d calls, %d unhandled", pct, s->nshadow, s->unhandled); munmap(s, sizeof *s); continue; }
            /* the rect is a snapped device rect: converting its 24.8 edges back gives the device edges */
            if (((c->x * pct) / 100) / 256 != x0 || (((c->x + c->w) * pct) / 100) / 256 != x1 ||
                (((c->y) * pct) / 100) / 256 != y0 || (((c->y + c->h) * pct) / 100) / 256 != y1 ||
                !(c->flags & LOGIT_RECT_SNAP) || !!(c->flags & LOGIT_SHADOW2_INSET) != inset ||
                c->dx != -3 * 256 || c->dy != 5 * 256 || c->blur != 7 * 256 || c->spread != 2 * 256 || c->argb != 0x80336699u)
                FAIL("shadow2-call @%d%% inset %d: fields not passed through (x %d w %d flags %x dx %d dy %d blur %d spread %d argb %08x)",
                     pct, inset, c->x, c->w, c->flags, c->dx, c->dy, c->blur, c->spread, c->argb);
            munmap(s, sizeof *s);
        }
    if (!bad) printf("ok   shadow2: one call, device-snapped rect, offset/blur/spread/colour/inset passed through at 100..200%%\n");
}

/* ------------------------------------------------------------ large blur */
static void sc_big_aui(struct aui_host *h, int a)
{
    (void)h; (void)a;
    aui_begin(AUI_BG);
    aui_shadow2(aui_r(70, 80, 60, 40), 10, 4, 6, 20, 3, 0xC0102030u, 0);
    aui_end();
}
static void sc_big_ref(struct aui_host *h, int a)
{
    int pct = h->pct, r = aui_host_pt(10, pct);
    int rad[4] = { r, r, r, r };
    (void)a;
    aui_begin(AUI_BG);
    ol_display_shadow_ex(&h->disp, aui_host_pt(70, pct), aui_host_pt(80, pct),
                         aui_host_pt(130, pct) - aui_host_pt(70, pct), aui_host_pt(120, pct) - aui_host_pt(80, pct),
                         rad, 4 * pct * 256 / 100, 6 * pct * 256 / 100, 20 * pct * 256 / 100, 3 * pct * 256 / 100,
                         0x102030u, 0xC0, 0);
    aui_end();
}

static void check_large_blur(void)
{
    for (int p = 0; p < 4; p++) {
        int pct = pcts[p];
        struct shot *a = render(1, pct, 200, 200, sc_big_aui, 0), *b = render(1, pct, 200, 200, sc_big_ref, 0);
        long diff = 0, shaded = 0;
        int mx = 0, far = aui_host_pt(30, pct);        /* 2 sigma left of the rect (x=70 - 40) */
        int mid = aui_host_pt(100, pct);
        for (int i = 0; i < a->w * a->h; i++) {
            int d = abs((int)(a->px[i] & 255) - (int)(b->px[i] & 255));
            if (a->px[i] != b->px[i]) diff++;
            if (d > mx) mx = d;
        }
        /* a pixel 2 sigma from the caster is darkened relative to the far corner: the blur was not clamped away */
        if ((a->px[mid * a->w + far] & 255) >= (a->px[0] & 255)) shaded = 0; else shaded = 1;
        if (diff || !shaded) FAIL("large-blur @%d%%: %ld pixels differ from ol_display_shadow_ex (max %d), 2-sigma pixel %s",
                                  pct, diff, mx, shaded ? "shaded" : "NOT shaded");
        munmap(a, sizeof *a); munmap(b, sizeof *b);
    }
    if (!bad) printf("ok   large-blur: sigma 20 pt is pixel-identical to ol_display_shadow_ex at 100/125/150/200%% (unclamped)\n");
}

/* ---------------------------------------------------------------- damage */
enum { D_SHADOW2, D_ELEV, D_GLOW, D_EX, D_INSET };
static void frame(int kind, int on)
{
    aui_begin(AUI_BG);
    aui_round(50, 45, 40, 20, 5, 0xFFFFFFu);
    if (on) switch (kind) {
        case D_SHADOW2: aui_shadow2(aui_r(50, 45, 40, 20), 5, 6, 10, 4, 2, 0xC0000000u, 0); break;
        case D_ELEV:    aui_elevation(aui_r(50, 45, 40, 20), 5, 4); break;
        case D_GLOW:    aui_glow(aui_r(50, 45, 40, 20), 5, 0xFF40A0FFu); break;
        case D_EX:      aui_shadow_ex(50, 45, 40, 20, 5, 6, 16, 200); break;
        case D_INSET:   aui_shadow2(aui_r(50, 45, 40, 20), 5, 3, 4, 3, 0, 0xFF000000u, 1); break;
    }
    aui_end();
}
static void sc_damage(struct aui_host *h, int kind)
{
    static uint32_t before[MAXW * MAXW];
    frame(kind, 0); aui_feed_done();
    memcpy(before, h->px, (size_t)h->w * h->h * 4);
    aui_host_stats_reset(h);
    frame(kind, 1); aui_feed_done();
    g_viol = 0;
    for (int y = 0; y < h->h; y++)
        for (int x = 0; x < h->w; x++) {
            if (before[y * h->w + x] == h->px[y * h->w + x]) continue;
            int in = 0;
            for (int i = 0; i < h->ndmg; i++)
                if (x >= h->dmg[i].x && x < h->dmg[i].x + h->dmg[i].w && y >= h->dmg[i].y && y < h->dmg[i].y + h->dmg[i].h) in = 1;
            if (!in) g_viol++;
        }
}

static void check_damage(void)
{
    static const char *const nm[5] = { "shadow2", "elevation", "glow", "shadow_ex", "inset" };
    for (int k = 0; k < 5; k++)
        for (int p = 0; p < 4; p++) {
            int pct = pcts[p];
            struct shot *s = render(1, pct, 160, 120, sc_damage, k);
            if (s->viol) FAIL("damage %s @%d%%: %ld changed pixels outside the flush rectangles", nm[k], pct, s->viol);
            if (s->ndmg == 0) FAIL("damage %s @%d%%: a changed frame flushed nothing", nm[k], pct);
            if (k == D_SHADOW2) {
                /* the set must reach 3 sigma (+ offset on the side it moves toward) beyond the caster: sigma 4, dx 6, dy 10, spread 2 */
                int rx = aui_host_pt(90 + 6 + 12 + 2, pct), by = aui_host_pt(65 + 10 + 12 + 2, pct);
                int lx = aui_host_pt(50 - 12 - 2, pct), ty = aui_host_pt(45 - 12 - 2, pct);
                int ok_r = 0, ok_b = 0, ok_l = 0, ok_t = 0;
                for (int i = 0; i < s->ndmg; i++) {
                    if (s->dmg[i].x + s->dmg[i].w >= rx) ok_r = 1;
                    if (s->dmg[i].y + s->dmg[i].h >= by) ok_b = 1;
                    if (s->dmg[i].x <= lx) ok_l = 1;
                    if (s->dmg[i].y <= ty) ok_t = 1;
                }
                if (!(ok_r && ok_b && ok_l && ok_t))
                    FAIL("damage shadow2 @%d%%: flush set does not reach 3 sigma + offset (r %d b %d l %d t %d)", pct, ok_r, ok_b, ok_l, ok_t);
            }
            munmap(s, sizeof *s);
        }
    if (!bad) printf("ok   damage: shadow2 / elevation / glow / shadow_ex / inset are covered by the flush set; it reaches 3 sigma + offset\n");
}

/* ---------------------------------------------------------------- legacy */
static void sc_legacy(struct aui_host *h, int a)
{
    (void)h; (void)a;
    aui_begin(AUI_BG);
    aui_shadow_ex(40, 36, 60, 40, 8, 3, 10, 120);
    aui_elevation(aui_r(40, 36, 60, 40), 8, 3);
    aui_glow(aui_r(40, 36, 60, 40), 8, 0xFF40A0FFu);
    aui_end();
}
static void sc_legacy_inset(struct aui_host *h, int a)
{
    (void)h; (void)a;
    aui_begin(AUI_BG);
    aui_shadow2(aui_r(40, 36, 60, 40), 8, 0, 2, 5, 0, 0xFF000000u, 1);
    aui_end();
}
static long differs(const struct shot *s)
{
    long n = 0;
    for (int i = 1; i < s->w * s->h; i++) if (s->px[i] != s->px[0]) n++;
    return n;
}
static void check_legacy(void)
{
    for (int p = 0; p < 4; p++) {
        struct shot *s = render(0, pcts[p], 160, 120, sc_legacy, 0), *n = render(0, pcts[p], 160, 120, sc_legacy_inset, 0);
        if (s->nshadow) FAIL("legacy @%d%%: %d SYS_GUI_SHADOW2 calls with INFO=0", pcts[p], s->nshadow);
        if (s->unhandled || n->unhandled) FAIL("legacy @%d%%: unhandled syscall numbers", pcts[p]);
        if (!differs(s)) FAIL("legacy @%d%%: nothing drawn on a kernel without SHADOW2", pcts[p]);
        if (differs(n)) FAIL("legacy @%d%%: an inset shadow drew on a kernel without SHADOW2", pcts[p]);
        munmap(s, sizeof *s); munmap(n, sizeof *n);
    }
    if (!bad) printf("ok   legacy: INFO=0 issues no SHADOW2 and the tile fallback still draws (its pixels are the legacy goldens)\n");
}
#endif

int main(void)
{
    check_elevation();
#ifndef AUI_SHADOW_LEGACY
    check_shadow2_call();
    check_large_blur();
    check_damage();
    check_legacy();
#endif
    printf(bad ? "FAIL aui-shadow-adopt\n" : "ok   aui-shadow-adopt\n");
    return bad;
}
