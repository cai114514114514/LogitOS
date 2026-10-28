/* aui-glass-controls: the buttons (design 4.3: capsules with NO glass), the
 * segmented pill and the toggle track of the real ring-3 aui.c over the REAL
 * SYS_GUI_GLASS2 / PATH / SHADOW2 handlers (tests/unit/aui_host/v2_*.c, used
 * unchanged).
 *
 *   secondary-glass2  a Secondary 80x28 button makes 0 GLASS2 calls, <= 3 PATH calls
 *   secondary-path    and exactly 2 SHADOW2 calls (the E1 key and ambient), light and
 *   secondary-shadow  dark, at 100 and 150 %
 *   primary           a Primary button makes 0 GLASS2 calls and paints a LINEAR gradient
 *   ai-stroke         the AI button's LINEAR stroke paint turns after hover + 300 ms and
 *                     is still after 600 ms (and asks for no more frames)
 *   call      the pill and the track are ONE glass2 SYMMETRIC/THIN   FAIL glass-call
 *   reduce    ui.reduce_transparency, or INFO without GLASS2, gives the
 *             gradient face: no glass2, strips present      FAIL reduce
 *   damage    a new track's flush set reaches the glass halo, and a changed
 *             backdrop pixel that the panel samples is flushed       FAIL damage
 *
 * The old rim-uniform check of an 80x28 BUTTON is gone with the button's glass
 * (design 4.3, tests/ui-aui-buttons.mk covers the capsule); the toggle track and
 * the segmented pill are superseded by ui-aui-controls / ui-aui-lists and keep the
 * asserts they had. Each render runs in a forked child (aui.c keeps its scale and
 * capability in file-scope statics). THE CONTROL (-DAUI_BTN_SECONDARY_GLASS) puts
 * glass_face back under a Secondary button: only the button checks run and it must
 * report 'FAIL secondary-glass2'. Exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.h"
#include "aui_damage.h"
#include "aui_host.h"
#include "wm_draw2.h"
#include "openlogit_glass_material.h"

long wm2_glass2(struct wm2_env *e, long a, long b, long c);
long wm2_path(struct wm2_env *e, long a, long b, long c);

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); bad = 1; } while (0)

#define MAXW 330
#define MAXH 250
#define GL_LOG 8
struct shot {
    int w, h, ndmg, status, nglass, nrect, nblit, unhandled, npath, nshadow, nlinear, due, wait;
    int ai[4];
    struct logit_glass2 log[GL_LOG];
    struct aui_host_rect dmg[AUI_HOST_DMG_MAX];
    long viol;
    uint32_t px[MAXW * MAXH];
};
typedef void (*scene_fn)(struct aui_host *h, int arg);
static int g_dark, g_reduce, g_viol_out;

static struct logit_glass2 g_log[GL_LOG];
static int g_nglass;

static int host_copyin(void *dst, const void *usrc, unsigned long n)
{ memcpy(dst, usrc, n); return 0; }

static long glass_hook(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    if (g_nglass < GL_LOG && a) memcpy(&g_log[g_nglass], (const void *)a, sizeof g_log[0]);
    g_nglass++;
    memset(&e, 0, sizeof e);
    e.d = &h->disp; e.scale_pct = h->pct; e.surf_w = h->w; e.surf_h = h->h;
    e.copyin = host_copyin; e.alloc = malloc; e.release = free;
    return wm2_glass2(&e, a, b, c);
}

static int g_nlinear, g_ai[4];
static long path_hook(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    const struct logit_paint2 *pt = (const struct logit_paint2 *)b;
    const struct logit_stroke2 *st = (const struct logit_stroke2 *)c;
    if (pt && pt->kind == LOGIT_PAINT2_LINEAR) {
        g_nlinear++;
        if (st && pt->nstop == 3) { g_ai[0] = pt->x0; g_ai[1] = pt->y0; g_ai[2] = pt->x1; g_ai[3] = pt->y1; }
    }
    memset(&e, 0, sizeof e);
    e.d = &h->disp; e.scale_pct = h->pct; e.surf_w = h->w; e.surf_h = h->h;
    e.copyin = host_copyin; e.alloc = malloc; e.release = free;
    return wm2_path(&e, a, b, c);
}

static long setting_hook(struct aui_host *h, long a, long b, long c)
{
    (void)h; (void)c;
    if (g_reduce && !strcmp((const char *)a, "ui.reduce_transparency")) { strcpy((char *)b, "1"); return 1; }
    return -1;
}

static struct shot *render(int v2, int pct, scene_fn fn, int arg)
{
    struct shot *s = mmap(0, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct aui_host *h = aui_host_new(160, 120, pct);
        h->dark = g_dark;
        if (v2) aui_host_enable_v2(h);
        aui_host_on(SYS_GUI_GLASS2, glass_hook);
        aui_host_on(SYS_SETTING_GET, setting_hook);
        aui_host_on(SYS_GUI_PATH, path_hook);
        g_nlinear = 0;
        gui_create("aui-glass", 160, 120);
        aui_set_size(160, 120);
        aui_round_a(-100, -100, 8, 8, 2, 0xFFFFFF, 128);     /* prime scale, theme and capability */
        aui_host_stats_reset(h);
        g_nglass = 0;
        fn(h, arg);
        s->w = h->w; s->h = h->h;
        s->nglass = g_nglass;
        s->nrect = (int)(h->calls[SYS_GUI_RECT] + h->calls[SYS_GUI_RRECT]);
        s->nblit = (int)h->calls[SYS_GUI_BLIT];
        s->viol = g_viol_out;
        s->npath = (int)h->calls[SYS_GUI_PATH];
        s->nshadow = (int)h->calls[SYS_GUI_SHADOW2];
        s->nlinear = g_nlinear;
        memcpy(s->ai, g_ai, sizeof s->ai);
        s->due = aui_anim_due(); s->wait = aui_anim_wait();
        memcpy(s->log, g_log, sizeof s->log);
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

static const int pcts[2] = { 100, 150 };

#define BX 40
#define BY 40
#define BW 80
#define BH 28
#define TW 44      /* aui_toggle's track */
#define TH 24

static void sc_button(struct aui_host *h, int arg)
{
    (void)h; (void)arg;
    aui_begin(AUI_BG);
    aui_button(BX, BY, BW, BH, "");
    aui_end();
}
static void sc_primary(struct aui_host *h, int arg)
{
    (void)h; (void)arg;
    aui_begin(AUI_BG);
    aui_button_ex(BX, BY, BW, BH, "", AUI_V_PRIMARY, 1);
    aui_end();
}

/* The AI button, hovered at t = 0 and drawn again `arg` ms later. */
static void sc_ai(struct aui_host *h, int arg)
{
    struct logit_event e;
    aui_begin(AUI_BG); aui_button_ai(BX, BY, BW, BH, "", 1); aui_end(); aui_feed_done();
    memset(&e, 0, sizeof e);
    e.type = EV_MOUSE_MOVE; e.a = BX + BW / 2; e.b = BY + BH / 2;
    aui_host_clock_advance(16);
    aui_feed(&e);
    aui_begin(AUI_BG); aui_button_ai(BX, BY, BW, BH, "", 1); aui_end(); aui_feed_done();
    aui_host_clock_advance((unsigned long long)arg);
    aui_begin(AUI_BG); aui_button_ai(BX, BY, BW, BH, "", 1); aui_end(); aui_feed_done();
    (void)h;
}

static void check_buttons(void)
{
    for (int dark = 0; dark < 2; dark++)
        for (int p = 0; p < 2; p++) {
            int pct = pcts[p];
            g_dark = dark; g_reduce = 0;
            struct shot *s = render(1, pct, sc_button, 0);
            if (s->nglass) FAIL("secondary-glass2 %s @%d%%: %d GLASS2 calls, want 0", dark ? "dark" : "light", pct, s->nglass);
            if (s->npath > 3) FAIL("secondary-path %s @%d%%: %d PATH calls, want <= 3", dark ? "dark" : "light", pct, s->npath);
            if (s->nshadow != 2) FAIL("secondary-shadow %s @%d%%: %d SHADOW2 calls, want exactly 2", dark ? "dark" : "light", pct, s->nshadow);
            munmap(s, sizeof *s);
#ifndef AUI_BTN_SECONDARY_GLASS
            s = render(1, pct, sc_primary, 0);
            if (s->nglass) FAIL("primary-glass2 %s @%d%%: %d GLASS2 calls, want 0", dark ? "dark" : "light", pct, s->nglass);
            if (s->nlinear < 1) FAIL("primary-gradient %s @%d%%: no LINEAR paint", dark ? "dark" : "light", pct);
            munmap(s, sizeof *s);
#endif
        }
    g_dark = 0;
#ifndef AUI_BTN_SECONDARY_GLASS
    {
        double d[3];
        static const int ms[3] = { 0, 300, 700 };
        for (int i = 0; i < 3; i++) {
            struct shot *s = render(1, 100, sc_ai, ms[i] == 0 ? 1 : ms[i]);
            d[i] = atan2((double)(s->ai[3] - s->ai[1]), (double)(s->ai[2] - s->ai[0])) * 180.0 / M_PI;
            if (!s->ai[2]) FAIL("ai-stroke: no gradient stroke recorded at %d ms", ms[i]);
            if (ms[i] == 300 && !s->wait) FAIL("ai-stroke: no frame requested 300 ms after hover");
            munmap(s, sizeof *s);
        }
        {
            struct shot *s = render(1, 100, sc_ai, 1500);
            double d2 = atan2((double)(s->ai[3] - s->ai[1]), (double)(s->ai[2] - s->ai[0])) * 180.0 / M_PI;
            if (fabs(d[1] - d[0]) < 1.0) FAIL("ai-stroke: matrix unchanged 300 ms after hover (%.1f -> %.1f)", d[0], d[1]);
            if (fabs(d[2] - d2) > 0.05) FAIL("ai-stroke: still turning after 600 ms (%.2f vs %.2f)", d[2], d2);
            if (s->due || s->wait) FAIL("ai-stroke: still asking for frames 1500 ms after hover");
            munmap(s, sizeof *s);
        }
    }
#endif
    if (!bad) printf("ok   buttons: Secondary 80x28 = 0 GLASS2, <= 3 PATH, 2 SHADOW2; Primary has a gradient; the AI stroke turns then stops\n");
}

#ifndef AUI_BTN_SECONDARY_GLASS
/* ------------------------------------------------------------- the calls */
static void sc_seg(struct aui_host *h, int arg)
{
    static const char *const it[3] = { "", "", "" };
    int sel = 1;
    (void)h; (void)arg;
    aui_begin(AUI_BG);
    aui_segmented(10, 10, 120, 24, it, 3, &sel);
    aui_end();
}
static void sc_toggle(struct aui_host *h, int arg)
{
    int on = 1;
    (void)h; (void)arg;
    aui_begin(AUI_BG);
    aui_toggle(10, 10, &on, 1);
    aui_end();
}

static int glass_ok(const struct logit_glass2 *g)
{
    return g->rim == OL_GLASS_RIM_SYMMETRIC && g->blur_level == OL_GLASS_BLUR_THIN && g->cut == 0 &&
           (g->flags & LOGIT_RECT_SNAP) && g->radius[1] == g->radius[0] && g->radius[2] == g->radius[0] &&
           g->radius[3] == g->radius[0] && g->opacity > 0 && g->w > 0 && g->h > 0;
}

static void check_calls(void)
{
    static const char *const nm[3] = { "button", "segmented", "toggle" };
    static const scene_fn fn[3] = { sc_button, sc_seg, sc_toggle };
    for (int p = 0; p < 2; p++)
        for (int w = 0; w < 3; w++) {
            struct shot *s = render(1, pcts[p], fn[w], 0);
            if (s->unhandled) FAIL("glass-call %s @%d%%: unhandled syscall numbers", nm[w], pcts[p]);
            if (s->nglass != (w == 0 ? 0 : 1)) FAIL("glass-call %s @%d%%: %d SYS_GUI_GLASS2 calls, want %d", nm[w], pcts[p], s->nglass, w == 0 ? 0 : 1);
            else if (w != 0 && !glass_ok(&s->log[0]))
                FAIL("glass-call %s @%d%%: rim %d blur %d cut %u flags %x (want SYMMETRIC, THIN, no cut, SNAP)", nm[w], pcts[p],
                     s->log[0].rim, s->log[0].blur_level, s->log[0].cut, s->log[0].flags);
            munmap(s, sizeof *s);
        }
    if (!bad) printf("ok   call: the segmented pill and the toggle track are one glass2 SYMMETRIC/THIN; the button makes none\n");
}

static void check_fallback(void)
{
    for (int p = 0; p < 2; p++) {
        for (int mode = 0; mode < 2; mode++) {         /* 0 reduce transparency, 1 kernel without GLASS2 */
            g_reduce = mode == 0;
            struct shot *s = render(mode == 0, pcts[p], sc_toggle, 0);
            if (s->nglass) FAIL("reduce %s @%d%%: %d glass2 calls", mode ? "no-kernel-support" : "reduce-transparency", pcts[p], s->nglass);
            if (!s->nrect && !s->nblit && !s->npath) FAIL("reduce %s @%d%%: no gradient face drawn", mode ? "no-kernel-support" : "reduce-transparency", pcts[p]);
            if (s->unhandled) FAIL("reduce @%d%%: unhandled syscall numbers", pcts[p]);
            munmap(s, sizeof *s);
        }
        g_reduce = 0;
    }
    if (!bad) printf("ok   reduce: ui.reduce_transparency and a kernel without GLASS2 keep the flat track face\n");
}

/* ---------------------------------------------------------------- damage */
static void sc_damage_new(struct aui_host *h, int arg)
{
    (void)h;
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    aui_host_stats_reset(h);
    aui_begin(AUI_BG);
    if (arg) { int on = 0; aui_toggle(BX, BY, &on, 1); }
    aui_end(); aui_feed_done();
}

/* A backdrop stripe 6 pt outside the button (inside the THIN halo) changes
 * colour between two frames; every pixel that changed, the panel's included,
 * must be in the flush set. */
static void sc_damage_back(struct aui_host *h, int arg)
{
    static uint32_t before[MAXW * MAXH];
    (void)arg;
    aui_begin(AUI_BG);
    aui_round(BX - 8, BY, 4, TH, 0, 0x2060C0u);
    { int on = 0; aui_toggle(BX, BY, &on, 1); }
    aui_end(); aui_feed_done();
    memcpy(before, h->px, (size_t)h->w * h->h * 4);
    aui_host_stats_reset(h);
    aui_begin(AUI_BG);
    aui_round(BX - 8, BY, 4, TH, 0, 0xE08020u);
    { int on = 0; aui_toggle(BX, BY, &on, 1); }
    aui_end(); aui_feed_done();
    g_viol_out = 0;
    for (int y = 0; y < h->h; y++)
        for (int x = 0; x < h->w; x++) {
            int in = 0;
            if (before[y * h->w + x] == h->px[y * h->w + x]) continue;
            for (int i = 0; i < h->ndmg; i++)
                if (x >= h->dmg[i].x && x < h->dmg[i].x + h->dmg[i].w && y >= h->dmg[i].y && y < h->dmg[i].y + h->dmg[i].h) in = 1;
            if (!in) g_viol_out++;
        }
}

static void check_damage(void)
{
    for (int p = 0; p < 2; p++) {
        int pct = pcts[p];
        struct shot *s = render(1, pct, sc_damage_new, 1);
        int hd = ad_halo_glass(aui_host_pt(BX + TW, pct) - aui_host_pt(BX, pct), aui_host_pt(BY + TH, pct) - aui_host_pt(BY, pct),
                               aui_host_pt(TH / 2, pct), OL_GLASS_BLUR_THIN);
        int l = aui_host_pt(BX, pct) - hd, t = aui_host_pt(BY, pct) - hd;
        int r = aui_host_pt(BX + TW, pct) + hd, b = aui_host_pt(BY + TH, pct) + hd, ok_l = 0, ok_t = 0, ok_r = 0, ok_b = 0;
        for (int i = 0; i < s->ndmg; i++) {
            if (s->dmg[i].x <= l) ok_l = 1;
            if (s->dmg[i].y <= t) ok_t = 1;
            if (s->dmg[i].x + s->dmg[i].w >= r) ok_r = 1;
            if (s->dmg[i].y + s->dmg[i].h >= b) ok_b = 1;
        }
        if (!(ok_l && ok_t && ok_r && ok_b))
            FAIL("damage @%d%%: the flush set does not reach the glass halo (%d px) (l %d t %d r %d b %d)", pct, hd, ok_l, ok_t, ok_r, ok_b);
        munmap(s, sizeof *s);
        s = render(1, pct, sc_damage_back, 0);
        if (s->viol) FAIL("damage @%d%%: %ld changed pixels outside the flush rectangles", pct, s->viol);
        if (!s->ndmg) FAIL("damage @%d%%: a changed backdrop flushed nothing", pct);
        munmap(s, sizeof *s);
    }
    if (!bad) printf("ok   damage: a glass track's flush set reaches the halo, and a changed backdrop it samples is flushed\n");
}
#endif

int main(void)
{
    check_buttons();
#ifndef AUI_BTN_SECONDARY_GLASS
    check_calls();
    check_fallback();
    check_damage();
#endif
    printf(bad ? "FAIL aui-glass-controls\n" : "ok   aui-glass-controls\n");
    return bad;
}
