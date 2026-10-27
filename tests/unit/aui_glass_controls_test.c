/* aui-glass-controls: buttons, the segmented pill and the toggle track of the
 * real ring-3 aui.c over the REAL SYS_GUI_GLASS2 handler (tests/unit/aui_host/
 * v2_fx.c, used unchanged, includes c/kernel/gui/wm_draw2_fx.c).
 *
 *   call      a button is ONE glass2 SYMMETRIC/THIN and no gradient strips
 *             (no SYS_GUI_RECT, no SYS_GUI_BLIT); the pill and the track too
 *                                                          FAIL glass-call
 *   reduce    ui.reduce_transparency, or INFO without GLASS2, gives the
 *             gradient face: no glass2, strips present      FAIL reduce
 *   rim       the luminance of the rim around an 80x28 button, sampled at the
 *             four edge midpoints and the four corner arcs, spans <= 8/255 at
 *             100 and 150%, light and dark                  FAIL rim-uniform
 *   damage    a new button's flush set reaches the glass halo, and a changed
 *             backdrop pixel that the panel samples is flushed
 *                                                          FAIL damage
 *
 * Each render runs in a forked child (aui.c keeps its scale and capability in
 * file-scope statics). THE CONTROL (-DAUI_GLASS_DIRECTIONAL) makes aui.c ask
 * for the directional rim: only the rim check runs there and it must report
 * 'FAIL rim-uniform'. Exits 1 on any failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.h"
#include "aui_damage.h"
#include "aui_host.h"
#include "wm_draw2.h"
#include "openlogit_glass_material.h"

long wm2_glass2(struct wm2_env *e, long a, long b, long c);

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); bad = 1; } while (0)

#define MAXW 330
#define MAXH 250
#define GL_LOG 8
struct shot {
    int w, h, ndmg, status, nglass, nrect, nblit, unhandled;
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
        gui_create("aui-glass", 160, 120);
        aui_set_size(160, 120);
        aui_round_a(-100, -100, 8, 8, 2, 0xFFFFFF, 128);     /* prime scale, theme and capability */
        aui_host_stats_reset(h);
        g_nglass = 0;
        fn(h, arg);
        s->w = h->w; s->h = h->h;
        s->nglass = g_nglass;
        s->nrect = (int)h->calls[SYS_GUI_RECT];
        s->nblit = (int)h->calls[SYS_GUI_BLIT];
        s->viol = g_viol_out;
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

/* ------------------------------------------------------------ the rim */
#define BX 40
#define BY 40
#define BW 80
#define BH 28

static void sc_button(struct aui_host *h, int arg)
{
    (void)h; (void)arg;
    aui_begin(AUI_BG);
    aui_button(BX, BY, BW, BH, "");
    aui_end();
}

static int lum(uint32_t p)
{ return (int)((((p >> 16) & 255) * 54 + ((p >> 8) & 255) * 183 + (p & 255) * 19) >> 8); }

/* Peak luminance of the rim along the inward normal (nx, ny) from the edge
 * pixel (ex, ey), over the outer four pixels. The peak and not one fixed depth:
 * the material may place its two equal outer levels on pixels 0-1 or 1-2
 * depending on how the edge falls on the pixel grid (a half covered outer
 * pixel), and that is not what the eye reads as a brighter side. */
static int rim_at(const struct shot *s, int ex, int ey, int nx, int ny)
{
    int best = 0;
    for (int d = 0; d < 4; d++) {
        int l = lum(s->px[(ey + d * ny) * s->w + ex + d * nx]);
        if (l > best) best = l;
    }
    return best;
}

static void check_rim(void)
{
    for (int dark = 0; dark < 2; dark++)
        for (int p = 0; p < 2; p++) {
            int pct = pcts[p];
            g_dark = dark; g_reduce = 0;
            struct shot *s = render(1, pct, sc_button, 0);
            int x0 = aui_host_pt(BX, pct), x1 = aui_host_pt(BX + BW, pct);
            int y0 = aui_host_pt(BY, pct), y1 = aui_host_pt(BY + BH, pct);
            int r = aui_host_pt(11, pct), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            int c45 = r - (r * 7071 + 5000) / 10000;       /* offset of the arc's 45 degree point from the corner */
            int v[8], k = 0, mn = 255, mx = 0;
            if (!s->nglass) { FAIL("rim-uniform %s @%d%%: no glass2 call", dark ? "dark" : "light", pct); munmap(s, sizeof *s); continue; }
            v[k++] = rim_at(s, cx, y0, 0, 1);
            v[k++] = rim_at(s, cx, y1 - 1, 0, -1);
            v[k++] = rim_at(s, x0, cy, 1, 0);
            v[k++] = rim_at(s, x1 - 1, cy, -1, 0);
            v[k++] = rim_at(s, x0 + c45, y0 + c45, 1, 1);
            v[k++] = rim_at(s, x1 - 1 - c45, y0 + c45, -1, 1);
            v[k++] = rim_at(s, x0 + c45, y1 - 1 - c45, 1, -1);
            v[k++] = rim_at(s, x1 - 1 - c45, y1 - 1 - c45, -1, -1);
            for (int i = 0; i < 8; i++) { if (v[i] < mn) mn = v[i]; if (v[i] > mx) mx = v[i]; }
            if (getenv("AUI_GLASS_VERBOSE") && pct == 100) {
                for (int d = -2; d < 8; d++) printf("      top+%d %06x  bot-%d %06x  left+%d %06x right-%d %06x\n", d, s->px[(y0 + d) * s->w + cx] & 0xFFFFFF, d, s->px[(y1 - 1 - d) * s->w + cx] & 0xFFFFFF, d, s->px[cy * s->w + x0 + d] & 0xFFFFFF, d, s->px[cy * s->w + x1 - 1 - d] & 0xFFFFFF);
            }
            if (getenv("AUI_GLASS_VERBOSE"))
                printf("     rim %s @%d%%: %d %d %d %d | %d %d %d %d\n", dark ? "dark" : "light", pct,
                       v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
            if (mx - mn > 8)
                FAIL("rim-uniform %s @%d%%: rim luminance spans %d..%d (%d > 8)", dark ? "dark" : "light", pct, mn, mx, mx - mn);
            munmap(s, sizeof *s);
        }
    g_dark = 0;
    if (!bad) printf("ok   rim: an 80x28 button's rim luminance spans <= 8/255 at 100%% and 150%%, light and dark\n");
}

#ifndef AUI_GLASS_DIRECTIONAL
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
            if (s->nglass != 1) FAIL("glass-call %s @%d%%: %d SYS_GUI_GLASS2 calls, want 1", nm[w], pcts[p], s->nglass);
            else if (!glass_ok(&s->log[0]))
                FAIL("glass-call %s @%d%%: rim %d blur %d cut %u flags %x (want SYMMETRIC, THIN, no cut, SNAP)", nm[w], pcts[p],
                     s->log[0].rim, s->log[0].blur_level, s->log[0].cut, s->log[0].flags);
            /* the toggle's knob and the pill's shadow are not gradient strips; only the button must be strip-free */
            if (w == 0 && (s->nrect || s->nblit)) FAIL("glass-call button @%d%%: %d rect + %d blit calls (gradient strips) beside the glass", pcts[p], s->nrect, s->nblit);
            munmap(s, sizeof *s);
        }
    if (!bad) printf("ok   call: button, segmented pill and toggle track are one glass2 SYMMETRIC/THIN; the button draws no gradient strips\n");
}

static void check_fallback(void)
{
    for (int p = 0; p < 2; p++) {
        for (int mode = 0; mode < 2; mode++) {         /* 0 reduce transparency, 1 kernel without GLASS2 */
            g_reduce = mode == 0;
            struct shot *s = render(mode == 0, pcts[p], sc_button, 0);
            if (s->nglass) FAIL("reduce %s @%d%%: %d glass2 calls", mode ? "no-kernel-support" : "reduce-transparency", pcts[p], s->nglass);
            if (!s->nrect && !s->nblit) FAIL("reduce %s @%d%%: no gradient face drawn", mode ? "no-kernel-support" : "reduce-transparency", pcts[p]);
            if (s->unhandled) FAIL("reduce @%d%%: unhandled syscall numbers", pcts[p]);
            munmap(s, sizeof *s);
        }
        g_reduce = 0;
    }
    if (!bad) printf("ok   reduce: ui.reduce_transparency and a kernel without GLASS2 keep the gradient face\n");
}

/* ---------------------------------------------------------------- damage */
static void sc_damage_new(struct aui_host *h, int arg)
{
    (void)h;
    aui_begin(AUI_BG); aui_end(); aui_feed_done();
    aui_host_stats_reset(h);
    aui_begin(AUI_BG);
    if (arg) aui_button(BX, BY, BW, BH, "");
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
    aui_round(BX - 8, BY, 4, BH, 0, 0x2060C0u);
    aui_button(BX, BY, BW, BH, "");
    aui_end(); aui_feed_done();
    memcpy(before, h->px, (size_t)h->w * h->h * 4);
    aui_host_stats_reset(h);
    aui_begin(AUI_BG);
    aui_round(BX - 8, BY, 4, BH, 0, 0xE08020u);
    aui_button(BX, BY, BW, BH, "");
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
        int hd = ad_halo_glass(aui_host_pt(BX + BW, pct) - aui_host_pt(BX, pct), aui_host_pt(BY + BH, pct) - aui_host_pt(BY, pct),
                               aui_host_pt(11, pct), OL_GLASS_BLUR_THIN);
        int l = aui_host_pt(BX, pct) - hd, t = aui_host_pt(BY, pct) - hd;
        int r = aui_host_pt(BX + BW, pct) + hd, b = aui_host_pt(BY + BH, pct) + hd, ok_l = 0, ok_t = 0, ok_r = 0, ok_b = 0;
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
    if (!bad) printf("ok   damage: a glass button's flush set reaches the halo, and a changed backdrop it samples is flushed\n");
}
#endif

int main(void)
{
    check_rim();
#ifndef AUI_GLASS_DIRECTIONAL
    check_calls();
    check_fallback();
    check_damage();
#endif
    printf(bad ? "FAIL aui-glass-controls\n" : "ok   aui-glass-controls\n");
    return bad;
}
