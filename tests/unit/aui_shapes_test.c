/* aui-shapes-native: the shape primitives of aui_paint.inc over the REAL
 * SYS_GUI_PATH handler (tests/unit/aui_host/v2_path.c includes
 * c/kernel/gui/wm_draw2_path.c on a host env bound to the harness display).
 *
 *   syscalls  every rounded alpha shape, stroke, squircle, arc, ... is exactly
 *             ONE syscall (v2 mode, 100/125/150%)           FAIL syscalls/rounded
 *   coverage  the path rrect matches the legacy tile path's coverage, per
 *             pixel, for r <= 72                            FAIL coverage
 *   bigradius r = 180 (inside the tile ceiling) and 300 (past BIG_MASK, where
 *             the tile path degrades to a square) are round FAIL bigradius
 *   hairline  aui_hairline_dev is one device pixel at 100/125/150/200%
 *                                                           FAIL hairline
 *   legacy    INFO=0 never issues SYS_GUI_PATH and every shape still draws
 *                                                           FAIL legacy
 *   damage    a changed arc / alpha stroke is covered by the frame's flush
 *             rects, and a spinner tick flushes only its own box
 *                                                           FAIL damage
 *   gallery   the S1 controls scene costs <= 60% of the legacy syscalls
 *                                                           FAIL gallery
 *
 * Each render runs in a forked child (aui.c keeps its scale and capability in
 * file-scope statics) and hands its pixels and counters back through shared
 * memory. THE CONTROL (-DAUI_FORCE_TILE_PATH) pretends the kernel never
 * reports PATH, so every shape takes the tile path: only the syscall check
 * runs there, and it must report 'FAIL syscalls/rounded'. Exits 1 on any
 * failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "aui.h"
#include "aui_host.h"

static int bad;
#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); bad = 1; } while (0)

#define MAXW 1300
struct shot {
    int w, h, ndmg, unhandled, status;
    long viol, area;                 /* damage scenes: changed px outside the flush set; flushed device px */
    unsigned long long total, path_calls, calls[AUI_HOST_NSYS];
    struct aui_host_rect dmg[AUI_HOST_DMG_MAX];
    uint32_t px[MAXW * MAXW];
};
typedef void (*scene_fn)(struct aui_host *h, int arg);
static long g_viol, g_area;     /* set by the damage scenes, inside the child */

static struct shot *render(int v2, int pct, int w_pt, int h_pt, scene_fn fn, int arg)
{
    struct shot *s = mmap(0, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        struct aui_host *h = aui_host_new(w_pt, h_pt, pct);
        if (v2) aui_host_enable_v2(h);
        gui_create("aui-shapes", w_pt, h_pt);
        aui_set_size(w_pt, h_pt);
        /* Prime the capability query and the cached scale off-surface, so the
         * counters below see only the scene's own calls. */
        aui_round_a(-100, -100, 8, 8, 2, 0xFFFFFF, 128);
        aui_host_stats_reset(h);
        fn(h, arg);
        s->w = h->w; s->h = h->h;
        s->viol = g_viol; s->area = g_area;
        for (int i = 0; i < AUI_HOST_NSYS; i++) { s->calls[i] = h->calls[i]; s->total += h->calls[i]; }
        s->path_calls = h->calls[SYS_GUI_PATH];
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
#ifndef AUI_FORCE_TILE_PATH
static int chan(const struct shot *s, int x, int y) { return (int)(s->px[y * s->w + x] & 0xFF); }
#endif

/* ------------------------------------------------------------- scenes */
#define WHITE 0xFFFFFFu
static void sc_round_a(struct aui_host *h, int a)       { (void)h; (void)a; aui_round_a(10, 10, 120, 70, 12, WHITE, 128); }
static void sc_round_big(struct aui_host *h, int a)     { (void)h; (void)a; aui_round_a(10, 10, 180, 180, 90, WHITE, 200); }
static void sc_round_opaque(struct aui_host *h, int a)  { (void)h; (void)a; aui_round(10, 10, 100, 60, 10, WHITE); }
static void sc_circle(struct aui_host *h, int a)        { (void)h; (void)a; aui_circle(60, 60, 30, WHITE); }
static void sc_stroke(struct aui_host *h, int a)        { (void)h; (void)a; aui_stroke(10, 10, 100, 60, 10, 2, WHITE); }
static void sc_ring(struct aui_host *h, int a)          { (void)h; (void)a; aui_ring(60, 60, 30, 3, WHITE); }
static void sc_fill_a(struct aui_host *h, int a)        { (void)h; (void)a; aui_fill_a(10, 10, 50, 30, WHITE, 100); }
static void sc_round4(struct aui_host *h, int a)
{ static const int r[4] = { 4, 16, 0, 24 }; (void)h; (void)a; aui_round4(aui_r(10, 10, 100, 70), r, 0x80FFFFFFu); }
static void sc_stroke_a(struct aui_host *h, int a)      { (void)h; (void)a; aui_stroke_a(aui_r(10, 10, 100, 70), 12, 384, 0x80FFFFFFu); }
static void sc_squircle(struct aui_host *h, int a)      { (void)h; (void)a; aui_squircle(aui_r(10, 10, 100, 70), 16, 0xC0FFFFFFu); }
static void sc_arc(struct aui_host *h, int a)           { (void)h; (void)a; aui_arc(60, 60, 30, -90, 270, 4, 0xFFFFFFFFu); }
static void sc_hair(struct aui_host *h, int a)          { (void)h; (void)a; aui_hairline_dev(10, 20, 90, 20, 0xFFFFFFFFu); }
static void sc_hair_d(struct aui_host *h, int a)        { (void)h; (void)a; aui_hairline_dev(10, 10, 90, 50, 0xFFFFFFFFu); }

static const struct { const char *name; scene_fn fn; int one; } shapes[] = {
    { "round_a", sc_round_a, 1 }, { "round_a r90", sc_round_big, 1 }, { "round opaque", sc_round_opaque, 1 },
    { "circle", sc_circle, 1 }, { "stroke", sc_stroke, 1 }, { "ring", sc_ring, 1 }, { "fill_a", sc_fill_a, 1 },
    { "round4", sc_round4, 1 }, { "stroke_a", sc_stroke_a, 1 }, { "squircle", sc_squircle, 1 },
    { "arc", sc_arc, 1 }, { "hairline_dev", sc_hair, 1 }, { "hairline_dev diagonal", sc_hair_d, 1 },
};
#define NSHAPES ((int)(sizeof shapes / sizeof shapes[0]))
static const int pcts[4] = { 100, 125, 150, 200 };

static void check_syscalls(void)
{
    for (int p = 0; p < 3; p++)
        for (int i = 0; i < NSHAPES; i++) {
            struct shot *s = render(1, pcts[p], 200, 200, shapes[i].fn, 0);
            if (s->total != 1 || s->unhandled)
                FAIL("syscalls/rounded %s @%d%%: %llu syscalls (%llu SYS_GUI_PATH), want exactly 1",
                     shapes[i].name, pcts[p], s->total, s->path_calls);
            munmap(s, sizeof *s);
        }
    if (!bad) printf("ok   syscalls: %d shapes x 3 scales are one syscall each\n", NSHAPES);
}


#ifndef AUI_FORCE_TILE_PATH
static void sc_hair_v(struct aui_host *h, int a)        { (void)h; (void)a; aui_hairline_dev(30, 10, 30, 90, 0xFFFFFFFFu); }
/* -------------------------------------------------------------- coverage */
static int g_r, g_w;
static void sc_cover(struct aui_host *h, int a) { (void)h; (void)a; aui_round_a(5, 5, g_w, g_w, g_r, WHITE, 254); }

/* The path rrect (v2) against the tile path (INFO=0), white at alpha 254 over
 * black: the largest per-pixel difference, and the signed sum over the whole
 * surface relative to the shape's own total (the AREA agreement). */
static int cover_delta(int r, int pct, double *area_rel)
{
    int n = 2 * r + 10, mx = 0;
    long sd = 0, st = 0;
    g_r = r; g_w = n < 24 ? 24 : n;
    struct shot *a = render(1, pct, g_w + 10, g_w + 10, sc_cover, 0);
    struct shot *b = render(0, pct, g_w + 10, g_w + 10, sc_cover, 0);
    for (int y = 0; y < a->h; y++)
        for (int x = 0; x < a->w; x++) {
            int d = chan(a, x, y) - chan(b, x, y);
            if (abs(d) > mx) mx = abs(d);
            sd += d; st += chan(b, x, y);
        }
    if (a->path_calls == 0) FAIL("coverage r=%d @%d%%: v2 mode drew no SYS_GUI_PATH", r, pct);
    *area_rel = st ? (double)sd / (double)st : 0;
    munmap(a, sizeof *a); munmap(b, sizeof *b);
    return mx;
}

static void check_coverage(void)
{
    static const int radii[] = { 2, 4, 8, 12, 16, 24, 36, 48, 72 };
    int worst100 = 0, worst = 0;
    double arel = 0;
    for (int p = 0; p < 4; p++)
        for (unsigned i = 0; i < sizeof radii / sizeof radii[0]; i++) {
            double rel;
            int mx = cover_delta(radii[i], pcts[p], &rel);
            /* SYS_GUI_PATH rasterizes at the default 4 sub-scanlines (wm2_path
             * passes subs 0 to ol_display_fill_path) where the tile path's
             * corner mask uses more, so an edge pixel can differ by a few
             * levels either way: bounded by 24/255 per pixel, and the AREA of
             * the shape (signed sum of the differences over the sum of the
             * tile's coverage) must agree within 1/255. At 125-200% the tile
             * blit also rescales a mirrored mask by up to a device pixel,
             * which is what the looser per-pixel bound at those scales is for. */
            int lim = pcts[p] == 100 ? 24 : 96;
            if (pcts[p] == 100 && mx > worst100) worst100 = mx;
            if (mx > worst) worst = mx;
            if (rel < 0) rel = -rel;
            if (rel > arel) arel = rel;
            if (mx > lim) FAIL("coverage r=%d @%d%%: max pixel delta %d > %d", radii[i], pcts[p], mx, lim);
            if (rel > 1.0 / 255) FAIL("coverage r=%d @%d%%: area differs by %.4f > 1/255", radii[i], pcts[p], rel);
        }
    printf("ok   coverage: path rrect vs tile path, r<=72, 4 scales: max pixel delta %d/255 (%d at 100%%), worst area error %.5f (limit %.5f)\n",
           worst, worst100, arel, 1.0 / 255);
}

/* ----------------------------------------------------------- big radius */
static void sc_big(struct aui_host *h, int r) { (void)h; aui_round_a(10, 10, 2 * r, 2 * r, r, WHITE, 200); }

static void check_bigradius(void)
{
    static const int rs[2] = { 180, 300 };      /* inside the tile ceiling; past BIG_MASK (256) */
    for (int k = 0; k < 2; k++) {
        int r = rs[k];
        for (int p = 0; p < 4; p++) {
            int pct = pcts[p];
            struct shot *s = render(1, pct, 2 * r + 20, 2 * r + 20, sc_big, r);
            int x0 = aui_host_pt(10, pct), c = aui_host_pt(10 + r, pct);
            int diag = aui_host_pt(10 + r / 5, pct);       /* 0.8r from the centre on both axes: outside the circle */
            if (chan(s, diag, diag) != 0)
                FAIL("bigradius r=%d @%d%%: the corner is square (px %d at the 20%% diagonal)", r, pct, chan(s, diag, diag));
            if (abs(chan(s, c, c) - 200) > 1 || abs(chan(s, x0 + 1, c) - 200) > 1)
                FAIL("bigradius r=%d @%d%%: the body is not filled (centre %d, edge %d)", r, pct, chan(s, c, c), chan(s, x0 + 1, c));
            munmap(s, sizeof *s);
        }
    }
    printf("ok   bigradius: r=180 and r=300 are round at 100/125/150/200%%\n");
}

/* ------------------------------------------------------------- hairline */
static void check_hairline(void)
{
    for (int p = 0; p < 4; p++) {
        int pct = pcts[p];
        struct shot *h = render(1, pct, 100, 100, sc_hair, 0), *v = render(1, pct, 100, 100, sc_hair_v, 0);
        int rows = 0, row = -1, cols = 0, col = -1, len = 0, vlen = 0;
        int mx = aui_host_pt(50, pct), my = aui_host_pt(50, pct);
        for (int y = 0; y < h->h; y++) if (chan(h, mx, y)) { rows++; row = y; }
        for (int x = 0; x < h->w; x++) if (row >= 0 && chan(h, x, row)) { len++; if (chan(h, x, row) != 255) len += 1000; }
        for (int x = 0; x < v->w; x++) if (chan(v, x, my)) { cols++; col = x; }
        for (int y = 0; y < v->h; y++) if (col >= 0 && chan(v, col, y)) { vlen++; if (chan(v, col, y) != 255) vlen += 1000; }
        if (rows != 1 || row != aui_host_pt(20, pct))
            FAIL("hairline @%d%%: horizontal line is %d device rows (row %d, want 1 at %d)", pct, rows, row, aui_host_pt(20, pct));
        if (len != aui_host_pt(90, pct) - aui_host_pt(10, pct))
            FAIL("hairline @%d%%: horizontal line has %d px (not all opaque, or wrong length; want %d)", pct, len, aui_host_pt(90, pct) - aui_host_pt(10, pct));
        if (cols != 1 || col != aui_host_pt(30, pct))
            FAIL("hairline @%d%%: vertical line is %d device columns (col %d, want 1 at %d)", pct, cols, col, aui_host_pt(30, pct));
        if (vlen != aui_host_pt(90, pct) - aui_host_pt(10, pct))
            FAIL("hairline @%d%%: vertical line has %d px (want %d)", pct, vlen, aui_host_pt(90, pct) - aui_host_pt(10, pct));
        munmap(h, sizeof *h); munmap(v, sizeof *v);
    }
    printf("ok   hairline: aui_hairline_dev is exactly 1 device px at 100/125/150/200%%\n");
}

/* --------------------------------------------------------------- legacy */
static long nonzero(const struct shot *s)
{
    long n = 0;
    for (int i = 0; i < s->w * s->h; i++) if (s->px[i] & 0xFFFFFF) n++;
    return n;
}
static void check_legacy(void)
{
    for (int i = 0; i < NSHAPES; i++) {
        struct shot *s = render(0, 100, 200, 200, shapes[i].fn, 0);
        int diag = shapes[i].fn == sc_hair_d;        /* a diagonal one-pixel line has no tile-path twin */
        if (s->path_calls) FAIL("legacy %s: SYS_GUI_PATH issued with INFO=0", shapes[i].name);
        if (s->unhandled) FAIL("legacy %s: %d unhandled syscall numbers", shapes[i].name, s->unhandled);
        if (!diag && !nonzero(s)) FAIL("legacy %s: nothing drawn on a kernel without PATH", shapes[i].name);
        munmap(s, sizeof *s);
    }
    printf("ok   legacy: with INFO=0 no shape issues SYS_GUI_PATH and every one still draws\n");
}

/* --------------------------------------------------------------- damage */
static int g_dmg_kind;
static void frame(struct aui_host *h, int kind, int k, unsigned long long ms)
{
    aui_host_clock_set(ms);
    aui_begin(AUI_BG);
    if (kind == 0) aui_arc(60, 60, 30, k * 100, 120, 6, 0xFFFFFFFFu);
    else if (kind == 1) aui_stroke_a(aui_r(20 + k * 8, 20, 90, 60), 14, 5 * 256, 0xC0FF8000u);
    else aui_spinner(60, 60, 20);
    aui_end();
    (void)h;
}
/* Two frames, the second changed: every pixel that differs between them must lie
 * inside the flush rectangles of the second frame. */
static void sc_damage(struct aui_host *h, int kind)
{
    static uint32_t before[MAXW * MAXW];
    frame(h, kind, 0, 1000); aui_feed_done();
    memcpy(before, h->px, (size_t)h->w * h->h * 4);
    aui_host_stats_reset(h);
    frame(h, kind, 1, kind == 2 ? 1150 : 1000); aui_feed_done();
    g_viol = 0; g_area = 0;
    for (int i = 0; i < h->ndmg; i++) g_area += (long)h->dmg[i].w * h->dmg[i].h;
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
    static const char *const nm[3] = { "arc", "stroke_a", "spinner" };
    (void)g_dmg_kind;
    for (int k = 0; k < 3; k++)
        for (int p = 0; p < 4; p++) {
            int pct = pcts[p];
            struct shot *s = render(1, pct, 160, 120, sc_damage, k);
            if (s->viol) FAIL("damage %s @%d%%: %ld changed pixels outside the flush rectangles", nm[k], pct, s->viol);
            if (s->ndmg == 0) FAIL("damage %s @%d%%: a changed frame flushed nothing", nm[k], pct);
            if (k == 2) {
                /* the spinner's arc box: circle of r-1 plus its 2-pt stroke halo each side, plus 1 device px of AA */
                long box = (long)(2 * 19 + 2 * 3) * pct / 100 + 2;
                if (s->area > box * box) FAIL("damage spinner @%d%%: flushed %ld px > its own box %ld", pct, s->area, box * box);
            }
            munmap(s, sizeof *s);
        }
    printf("ok   damage: path shapes are covered by the flush set; the spinner flushes only its own box\n");
}

/* -------------------------------------------------------------- gallery */
static void sc_s1(struct aui_host *h, int a) { (void)a; aui_host_scene_draw(0, h); }
static void check_gallery(void)
{
    for (int p = 0; p < 4; p++) {
        struct shot *v = render(1, pcts[p], 160, 120, sc_s1, 0), *l = render(0, pcts[p], 160, 120, sc_s1, 0);
        double ratio = l->total ? (double)v->total / (double)l->total : 1;
        if (ratio > 0.60) FAIL("gallery @%d%%: %llu syscalls vs the legacy baseline %llu = %.1f%% > 60%%", pcts[p], v->total, l->total, 100 * ratio);
        else printf("ok   gallery @%d%%: S1 controls frame set %llu syscalls vs the tile-path baseline %llu (%.1f%%)\n", pcts[p], v->total, l->total, 100 * ratio);
        munmap(v, sizeof *v); munmap(l, sizeof *l);
    }
}
#endif

int main(void)
{
    check_syscalls();
#ifndef AUI_FORCE_TILE_PATH
    check_coverage();
    check_bigradius();
    check_hairline();
    check_legacy();
    check_damage();
    check_gallery();
#endif
    printf(bad ? "FAIL aui-shapes-native\n" : "ok   aui-shapes-native\n");
    return bad;
}
