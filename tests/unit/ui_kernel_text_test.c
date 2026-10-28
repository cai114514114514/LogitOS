/* ui_kernel_text_test.c -- the kernel chrome text helpers
 * (c/kernel/gui/wm_text.c, backlog ui-kernel-text) against the real text.c
 * (weight -> face, metrics, fonts from fsroot/fonts).
 *
 * Below wm_text.c two things are the test's own:
 *   - text_draw_run_a is a RECORDER (device x, y, px, face, colour, alpha,
 *     string per call, in call order), so placement is checked as numbers;
 *   - text_measure answers either the real engine or, in "proportional" mode,
 *     a narrow '1' and ':' next to wide digits. The shipped fonts are already
 *     tabular, so only a proportional measure can tell a real tabular layout
 *     from natural advances.
 *
 * Every check prints `ok <name> ...` or `FAIL <name> ...`; the run ends with
 * `done <n> checks` (the control requires it, so a crash cannot pass for the
 * stated failure) and exits 1 on any failure.
 *
 * NEGATIVE CONTROL: -DUI_KTEXT_NEGCTL builds wm_text.c with natural digit
 * advances; `FAIL ktnum` must appear. */
#define text_draw_run_a text_draw_run_a_engine
#define text_measure text_measure_engine
#include "../../c/kernel/gui/text.c"
#undef text_draw_run_a
#undef text_measure

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logit_abi.h"
#include "ui/logit_tokens.h"
#include "../../c/kernel/gui/wm_text.h"

static const char *root = "fsroot";

void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
void spin_lock(spinlock_t *p) { (void)p; }
void spin_unlock(spinlock_t *p) { (void)p; }
uint64_t spin_lock_irqsave(spinlock_t *p) { (void)p; return 0; }
void spin_unlock_irqrestore(spinlock_t *p, uint64_t f) { (void)p; (void)f; }
static FILE *font_file(const char *path)
{ char name[2048]; snprintf(name, sizeof name, "%s%s", root, path); return fopen(name, "rb"); }
int vfs_size(const char *path)
{ FILE *f = font_file(path); if (!f) return -1; fseek(f, 0, SEEK_END); int n = (int)ftell(f); fclose(f); return n; }
int vfs_read(const char *path, void *buf, int max)
{ FILE *f = font_file(path); if (!f) return -1; int n = (int)fread(buf, 1, (size_t)max, f); fclose(f); return n; }
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{ (void)x; (void)y; (void)cov; (void)w; (void)h; (void)color; }

static int g_fb_pct = 100;
int fb_scale(void) { return g_fb_pct; }

/* ---- measure: real, or proportional --------------------------------- */
static int prop_mode;
int text_measure(const char *s, int len, int px, int face)
{
    if (!prop_mode) return text_measure_engine(s, len, px, face);
    int w = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '1' || s[i] == ':') w += px * 3 / 10;
        else if (s[i] >= '0' && s[i] <= '9') w += px * 6 / 10;
        else w += px / 2;
    }
    return w;
}

/* ---- the recorder ----------------------------------------------------- */
#define NREC 64
static struct rec {
    int x, y, len, px, alpha;
    unsigned face;
    uint32_t rgb;
    char s[80];
} L[NREC];
static int nrec;
static struct ol_display *last_d;
static void rec_reset(void) { nrec = 0; last_d = 0; }

int text_draw_run_a(struct ol_display *d, int x, int y, const char *s, int len,
                    int px, unsigned face, uint32_t rgb, int alpha);
int text_draw_run_a(struct ol_display *d, int x, int y, const char *s, int len,
                    int px, unsigned face, uint32_t rgb, int alpha)
{
    if (nrec < NREC) {
        struct rec *r = &L[nrec++];
        int k = len < 0 ? 0 : len > (int)sizeof r->s - 1 ? (int)sizeof r->s - 1 : len;
        r->x = x; r->y = y; r->len = len; r->px = px; r->face = face; r->rgb = rgb; r->alpha = alpha;
        memcpy(r->s, s, (size_t)k); r->s[k] = 0;
    }
    last_d = d;
    return x + text_measure(s, len, px, (int)face);
}

static struct ol_display DISP;
static int bad, checks;
static void check(int cond, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void check(int cond, const char *name, const char *fmt, ...)
{
    char d[256];
    va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap);
    printf("%s %s %s\n", cond ? "ok" : "FAIL", name, d);
    checks++;
    if (!cond) bad = 1;
}

static int utf8_valid_end(const char *s, int n)   /* s[0..n) ends on a boundary */
{ return n == 0 || (unsigned char)s[n] == 0 || ((unsigned char)s[n] & 0xC0) != 0x80; }

int main(int argc, char **argv)
{
    if (argc > 1) root = argv[1];
    text_init();
    static const int pcts[] = { 100, 125, 150, 200 };

    /* 1. weight -> face, the TEXT_RUN2 resolver. */
    static const int wts[] = { 0, 400, 500, 590, 600, 700 };
    for (int k = 0; k < 6; k++) {
        rec_reset();
        wm_text_w_pct(100, &DISP, 4, 5, 13, wts[k], 0x112233, "Logit");
        unsigned want = wts[k] ? text_face_for_weight(0, wts[k]) : 0;
        check(nrec == 1 && L[0].face == want && L[0].rgb == 0x112233 && L[0].alpha == 255 &&
              last_d == &DISP, "weight-face", "weight %d face %u want %u", wts[k], L[0].face, want);
    }
    rec_reset(); wm_text_w_pct(100, &DISP, 0, 0, 13, 600, 0, "a");
    unsigned f600 = L[0].face;
    rec_reset(); wm_text_w_pct(100, &DISP, 0, 0, 13, 400, 0, "a");
    check(f600 != L[0].face && f600 != 0, "weight-distinct", "600 face %u vs 400 face %u", f600, L[0].face);
    rec_reset(); wm_text_w_pct(100, &DISP, 0, 0, 13, 590, 0, "a");
    check(L[0].face == f600, "weight-nearest", "590 resolves to the 600 face");
    check(wm_text_measure_w_pct(100, 14, 600, "Logit OS 设置") > wm_text_measure_w_pct(100, 14, 400, "Logit OS 设置"),
          "weight-measure", "semibold run is wider than regular");

    /* 2. points -> device pixels at every scale (floor, like wm2_pt). */
    for (int k = 0; k < 4; k++) {
        int p = pcts[k];
        rec_reset();
        wm_text_w_pct(p, &DISP, 10, 4, 13, 400, 0xAABBCCDD, "Week");
        check(nrec == 1 && L[0].x == 10 * p / 100 && L[0].y == 4 * p / 100 && L[0].px == 13 * p / 100 &&
              L[0].rgb == 0xBBCCDD, "scale-w", "%d%%: dev %d,%d px %d", p, L[0].x, L[0].y, L[0].px);
    }
    g_fb_pct = 125; rec_reset();
    wm_text_w(&DISP, 8, 8, 12, 400, 0, "x");
    check(nrec == 1 && L[0].x == 10 && L[0].px == 15, "scale-wm", "wrapper reads fb_scale: x %d px %d", L[0].x, L[0].px);
    g_fb_pct = 100;

    /* 3. tabular digits. Proportional measure: '1' narrow, '8' wide. */
    prop_mode = 1;
    for (int k = 0; k < 4; k++) {
        int p = pcts[k], x4[2];
        const char *strs[2] = { "11:11", "88:88" };
        for (int j = 0; j < 2; j++) {
            rec_reset();
            wm_text_tnum_pct(p, &DISP, 0, 0, 14, 400, 0xFFFFFF, strs[j], WM_TEXT_LEFT);
            x4[j] = nrec >= 4 ? L[3].x : -1;
            if (j == 0) check(nrec == 5, "ktnum-runs", "%d%%: five runs for 11:11, got %d", p, nrec);
        }
        check(x4[0] == x4[1] && x4[0] > 0, "ktnum", "%d%%: 4th glyph x %d for 11:11, %d for 88:88", p, x4[0], x4[1]);
    }
    {   /* right alignment ends on the last digit's cell; width is alignment-free. */
        rec_reset(); int wl = wm_text_tnum_pct(100, &DISP, 100, 0, 14, 400, 0, "11:11", WM_TEXT_LEFT);
        rec_reset(); int wr = wm_text_tnum_pct(100, &DISP, 100, 0, 14, 400, 0, "11:11", WM_TEXT_RIGHT);
        int cell = 14 * 6 / 10, nar = 14 * 3 / 10;          /* 8 and 4 */
        check(wl == wr && wl == 4 * cell + nar && L[0].x == 100 - wr &&
              L[nrec - 1].x == 100 - cell, "tnum-align",
              "width %d/%d, first %d, last digit %d", wl, wr, L[0].x, L[nrec - 1].x);
        rec_reset(); wm_text_tnum_pct(100, &DISP, 100, 0, 14, 400, 0, "11:11", WM_TEXT_CENTER);
        check(L[0].x == 100 - wl / 2, "tnum-center", "first digit x %d", L[0].x);
    }
    prop_mode = 0;

    /* 4. cache: keyed by scale and size, bounded at eight, never stale. */
    {
        int c100 = wm_text_tnum_pct(100, &DISP, 0, 0, 13, 600, 0, "0", WM_TEXT_LEFT);
        int c150 = wm_text_tnum_pct(150, &DISP, 0, 0, 13, 600, 0, "0", WM_TEXT_LEFT);
        int c200 = wm_text_tnum_pct(200, &DISP, 0, 0, 13, 600, 0, "0", WM_TEXT_LEFT);
        check(c100 > 0 && c150 > c100 && c200 > c150, "cache-rekey", "cell 100/150/200 = %d/%d/%d", c100, c150, c200);
        check(c200 >= 2 * c100 - 2 && c200 <= 2 * c100 + 2, "cache-200", "200%% cell %d vs 2x %d", c200, c100);
        for (int px = 11; px < 30; px++)
            wm_text_tnum_pct(100, &DISP, 0, 0, px, 500, 0, "0", WM_TEXT_LEFT);
        int again = wm_text_tnum_pct(100, &DISP, 0, 0, 13, 600, 0, "0", WM_TEXT_LEFT);
        check(again == c100, "cache-evict", "after 19 other keys the 100%% cell is still %d (was %d)", again, c100);
    }

    /* 5. metric baseline. */
    check(wm_text_baseline(0, 24, 14) == 18, "baseline-menubar", "(0,24,14) -> %d", wm_text_baseline(0, 24, 14));
    check(wm_text_baseline(0, 30, 13) == 21, "baseline-titlebar", "(0,30,13) -> %d", wm_text_baseline(0, 30, 13));
    check(wm_text_baseline(100, 24, 14) == 118, "baseline-offset", "(100,24,14) -> %d", wm_text_baseline(100, 24, 14));
    for (int k = 0; k < 4; k++) {
        int p = pcts[k];
        struct wm_text_rect r = { 0, 0, 200, 24 };
        rec_reset();
        wm_text_vc_pct(p, &DISP, r, "Logit", 14, 400, 0, WM_TEXT_LEFT);
        int pxd = 14 * p / 100, hd = 24 * p / 100;
        struct logit_text_metrics m;
        text_measure_run_metrics("", 0, pxd, 0, &m);
        int want = (logit_text_baseline24(0, hd * 256, pxd) + 128) >> 8;
        check(nrec == 1 && L[0].y + m.ascent == want, "vc-baseline", "%d%%: drawn baseline %d want %d", p, L[0].y + m.ascent, want);
    }
    {   /* alignment inside the rect */
        struct wm_text_rect r = { 10, 0, 100, 24 };
        rec_reset(); int w = wm_text_vc_pct(100, &DISP, r, "Logit", 14, 400, 0, WM_TEXT_RIGHT);
        check(L[0].x == 110 - w, "vc-right", "x %d", L[0].x);
        rec_reset(); wm_text_vc_pct(100, &DISP, r, "Logit", 14, 400, 0, WM_TEXT_CENTER);
        check(L[0].x == 10 + (100 - w) / 2, "vc-center", "x %d", L[0].x);
    }

    /* 6. shadowed ink: the shadow first, one point lower, then the ink. */
    for (int k = 0; k < 4; k++) {
        int p = pcts[k];
        rec_reset();
        wm_text_shadowed_pct(p, &DISP, 20, 3, 14, 400, 0xFFFFFF, 0x66000000, "Wed 13:08");
        int off = p / 100 < 1 ? 1 : p / 100;
        check(nrec == 2 && L[0].rgb == 0 && L[0].alpha == 0x66 && L[1].rgb == 0xFFFFFF && L[1].alpha == 255 &&
              L[0].y == L[1].y + off && L[0].x == L[1].x && L[1].y == 3 * p / 100, "shadow-order",
              "%d%%: shadow a%d y%d then ink a%d y%d", p, L[0].alpha, L[0].y, L[1].alpha, L[1].y);
    }
    rec_reset(); wm_text_shadowed_pct(100, &DISP, 0, 0, 14, 400, 0xFFFFFF, 0x00000000, "x");
    check(nrec == 1 && L[0].alpha == 255, "shadow-none", "alpha 0 shadow skips its pass (%d runs)", nrec);

    /* 7. ellipsis. */
    {
        const char *longs = "这是一个很长很长的文件名称用来测试省略号.txt";
        const char *shorts = "设置";
        struct wm_text_rect r = { 0, 0, 90, 24 };
        rec_reset(); wm_text_ellipsis_pct(100, &DISP, r, shorts, 13, 400, 0);
        check(nrec == 1 && strcmp(L[0].s, shorts) == 0, "ellipsis-fits", "short text drawn whole: %s", L[0].s);
        for (int k = 0; k < 4; k++) {
            int p = pcts[k];
            rec_reset(); wm_text_ellipsis_pct(p, &DISP, r, longs, 13, 600, 0);
            int pxd = 13 * p / 100, avail = 90 * p / 100;
            unsigned face = text_face_for_weight(0, 600);
            int n = nrec ? (int)strlen(L[0].s) : 0;
            int tail = n >= 3 && memcmp(L[0].s + n - 3, "\xE2\x80\xA6", 3) == 0;
            int cut = n - 3;
            int fits = text_measure(L[0].s, n, pxd, (int)face) <= avail;
            int whole = text_measure(longs, (int)strlen(longs), pxd, (int)face) > avail;
            /* maximal: one more code point would not fit */
            int nx = cut + 1;
            while (longs[nx] && ((unsigned char)longs[nx] & 0xC0) == 0x80) nx++;
            char more[128]; memcpy(more, longs, (size_t)nx); memcpy(more + nx, "\xE2\x80\xA6", 4);
            int maximal = text_measure(more, nx + 3, pxd, (int)face) > avail;
            check(whole && nrec == 1 && tail && fits && maximal && memcmp(L[0].s, longs, (size_t)cut) == 0 &&
                  utf8_valid_end(longs, cut), "ellipsis-cut",
                  "%d%%: '%s' fits=%d maximal=%d", p, L[0].s, fits, maximal);
        }
        struct wm_text_rect tiny = { 0, 0, 2, 24 };
        rec_reset(); wm_text_ellipsis_pct(100, &DISP, tiny, longs, 13, 400, 0);
        check(nrec == 1 && strcmp(L[0].s, "\xE2\x80\xA6") == 0, "ellipsis-tiny", "only the tail: %s", L[0].s);
        char big[800]; memset(big, 'a', 799); big[799] = 0;
        struct wm_text_rect wide = { 0, 0, 4000, 24 };
        rec_reset(); wm_text_ellipsis_pct(100, &DISP, wide, big, 13, 400, 0);
        check(nrec == 1 && L[0].len <= 255 && L[0].len > 3, "ellipsis-bounded", "staging is bounded (%d bytes drawn)", L[0].len);
    }

    printf("done %d checks\n", checks);
    return bad;
}
