/* ui_run2_units_test.c -- the REAL SYS_GUI_TEXT_RUN2 handler
 * (c/kernel/gui/wm_draw2_text.c) against the 24.8 contract logit_abi.h states
 * for struct logit_run2: x, y and px are 24.8 logical points.
 *
 * The handler is linked as is. Below it, text_draw_run_a is a RECORDER: it
 * keeps the device x, y, px, face, colour, alpha and string the handler asked
 * for, so the unit conversion is checked as numbers, not as pixels. The real
 * text.c is compiled into this translation unit with its text_draw_run_a
 * renamed out of the way, so text_face_for_weight (and the weight -> face
 * answer the semibold check reads) is the kernel's own, not a stub.
 *
 * Every case prints `ok <case> ...` or `FAIL <case> ...`; the run ends with a
 * `done <n> checks` line (the negative control requires it, so a crash cannot
 * pass for a failure) and exits 1 on any failure.
 *
 * NEGATIVE CONTROL: -DUI_RUN2_NEGCTL builds the handler with the old integer
 * reading (px 1..512 points); aui's px * 256 is refused and `FAIL run2-px24`
 * must appear. */
#define text_draw_run_a text_draw_run_a_engine
#include "../../c/kernel/gui/text.c"
#undef text_draw_run_a

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "logit_abi.h"
#include "wm_draw2.h"
#include "wm2_scale.h"

/* ---- kernel stubs text.c needs (the same set text_alpha_test.c supplies) */
void *kmalloc(unsigned long n) { return malloc(n); }
void kfree(void *p) { free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
void spin_lock(spinlock_t *p) { (void)p; }
void spin_unlock(spinlock_t *p) { (void)p; }
uint64_t spin_lock_irqsave(spinlock_t *p) { (void)p; return 0; }
void spin_unlock_irqrestore(spinlock_t *p, uint64_t f) { (void)p; (void)f; }
int vfs_size(const char *path) { (void)path; return -1; }
int vfs_read(const char *path, void *buf, int max) { (void)path; (void)buf; (void)max; return -1; }
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{ (void)x; (void)y; (void)cov; (void)w; (void)h; (void)color; }

/* ---- the recorder ------------------------------------------------------ */
static struct rec {
    int n;                       /* calls since rec_reset */
    struct ol_display *d;
    int x, y, len, px, alpha;
    unsigned face;
    uint32_t rgb;
    char s[1100];
} R;
static void rec_reset(void) { memset(&R, 0, sizeof R); }

int text_draw_run_a(struct ol_display *d, int x, int y, const char *s, int len,
                    int px, unsigned face, uint32_t rgb, int alpha);
int text_draw_run_a(struct ol_display *d, int x, int y, const char *s, int len,
                    int px, unsigned face, uint32_t rgb, int alpha)
{
    R.n++; R.d = d; R.x = x; R.y = y; R.len = len; R.px = px; R.face = face;
    R.rgb = rgb; R.alpha = alpha;
    int k = len < 0 ? 0 : len > (int)sizeof R.s - 1 ? (int)sizeof R.s - 1 : len;
    memcpy(R.s, s, (size_t)k); R.s[k] = 0;
    return x;
}

/* ---- copy-in stubs: a plain copy, and one that faults on the string ---- */
static const void *fault_ptr;
static int copyin(void *dst, const void *src, unsigned long n)
{
    if (src == fault_ptr) return -1;
    memcpy(dst, src, n);
    return 0;
}

static struct ol_display DISP;   /* identity only: the recorder never draws */
static long run2(struct logit_run2 *r, int pct)
{
    struct wm2_env e;
    memset(&e, 0, sizeof e);
    e.d = &DISP; e.scale_pct = pct; e.surf_w = 800; e.surf_h = 600; e.copyin = copyin;
    rec_reset();
    return wm2_text_run2(&e, (long)r, 0, 0);
}
static void mkrun(struct logit_run2 *r, const char *s, int x24, int y24, int px24)
{
    memset(r, 0, sizeof *r);
    r->size = sizeof *r; r->x = x24; r->y = y24; r->px = px24;
    r->color = 0x203040; r->s = s; r->len = (int)strlen(s); r->alpha = 255;
}

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

int main(void)
{
    const char *str = "Week";
    struct logit_run2 r;
    long rc;

    /* 1. THE CONTRACT: {x 5 pt, y 4 pt, px 12 pt} in 24.8, as aui sends it.
     * Device px = floor(12 * pct / 100); x and y land where the legacy run
     * (wm2_pt == fb_pt) puts whole points. */
    static const int pcts[] = { 100, 125, 150, 200 };
    static const int want_px[] = { 12, 15, 18, 24 };
    for (int k = 0; k < 4; k++) {
        int p = pcts[k];
        mkrun(&r, str, 5 * 256, 4 * 256, 12 * 256);
        rc = run2(&r, p);
        check(rc == OL_OK && R.n == 1 && R.px == want_px[k], "run2-px24",
              "pct=%d rc=%ld calls=%d px=%d want %d", p, rc, R.n, R.px, want_px[k]);
        check(rc == OL_OK && R.n == 1 && R.x == wm2_pt(5, p) && R.y == wm2_pt(4, p),
              "run2-pos24", "pct=%d x=%d y=%d want %d %d", p, R.x, R.y, wm2_pt(5, p), wm2_pt(4, p));
        check(R.n == 1 && R.len == 4 && strcmp(R.s, str) == 0 && R.d == &DISP,
              "run2-string", "pct=%d len=%d s=\"%s\"", p, R.len, R.s);
    }

    /* 2. Fractions: 24.8 is floored, never truncated toward zero. 5.5 pt at
     * 100% is pixel 5 (the half pixel is the unreached sub-pixel pen, see the
     * handler's header) and 11 at 200%; -3.5 pt at 150% is -5.25 -> -6. */
    mkrun(&r, str, 5 * 256 + 128, 4 * 256 + 128, 12 * 256 + 128);
    rc = run2(&r, 100);
    check(rc == OL_OK && R.x == 5 && R.y == 4 && R.px == 12, "run2-frac-100",
          "x=%d y=%d px=%d want 5 4 12", R.x, R.y, R.px);
    rc = run2(&r, 200);
    check(rc == OL_OK && R.x == 11 && R.y == 9 && R.px == 25, "run2-frac-200",
          "x=%d y=%d px=%d want 11 9 25", R.x, R.y, R.px);
    mkrun(&r, str, -(3 * 256 + 128), -(1 * 256), 12 * 256);
    rc = run2(&r, 150);
    check(rc == OL_OK && R.x == -6 && R.y == -2 && R.x == (wm2_pt24(-(3 * 256 + 128), 150) >> 8),
          "run2-neg-floor", "x=%d y=%d want -6 -2", R.x, R.y);

    /* 3. px bounds, in 24.8: 1 pt .. 512 pt inclusive. */
    mkrun(&r, str, 0, 0, 255);
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "px24-below-1pt", "px24=255 rc=%ld calls=%d", rc, R.n);
    mkrun(&r, str, 0, 0, 512 * 256 + 1);
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "px24-above-512pt", "px24=%d rc=%ld calls=%d",
          512 * 256 + 1, rc, R.n);
    mkrun(&r, str, 0, 0, 512 * 256);
    rc = run2(&r, 100);
    check(rc == OL_OK && R.n == 1 && R.px == 512, "px24-512pt-accepted", "rc=%ld px=%d", rc, R.px);
    rc = run2(&r, 200);
    check(rc == OL_OK && R.n == 1 && R.px == 1024, "px24-512pt-at-200", "rc=%ld px=%d", rc, R.px);
    mkrun(&r, str, 0, 0, 256);
    rc = run2(&r, 100);
    check(rc == OL_OK && R.n == 1 && R.px == 1, "px24-1pt-accepted", "rc=%ld px=%d", rc, R.px);
    mkrun(&r, str, 0, 0, 0);
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "px24-zero", "rc=%ld", rc);
    mkrun(&r, str, 0, 0, -12 * 256);
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "px24-negative", "rc=%ld", rc);

    /* 4. Weight: 600 -> the semibold slot, replacing a bold face bit; 0 keeps
     * the face bits; 700 is bold; mono has 400 and 700 only. */
    unsigned semi = text_face_for_weight(0, 600);
    mkrun(&r, str, 256, 256, 13 * 256); r.weight = 600; r.face = LOGIT_FACE_BOLD;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.face == semi && semi == (2u << TEXT_FACE_WSHIFT) &&
          !(R.face & LOGIT_FACE_BOLD), "weight600-semibold", "face=%#x want %#x", R.face, semi);
    mkrun(&r, str, 256, 256, 13 * 256); r.weight = 0; r.face = LOGIT_FACE_BOLD | 0xF0;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.face == LOGIT_FACE_BOLD, "weight0-keeps-face",
          "face=%#x want %#x (undefined bits dropped)", R.face, LOGIT_FACE_BOLD);
    mkrun(&r, str, 256, 256, 13 * 256); r.weight = 700;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.face == LOGIT_FACE_BOLD, "weight700-bold", "face=%#x", R.face);
    mkrun(&r, str, 256, 256, 13 * 256); r.weight = 500;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.face == (1u << TEXT_FACE_WSHIFT), "weight500-medium", "face=%#x", R.face);
    mkrun(&r, str, 256, 256, 13 * 256); r.weight = 600; r.face = LOGIT_FACE_MONO;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.face == (LOGIT_FACE_MONO | LOGIT_FACE_BOLD), "weight600-mono-bold",
          "face=%#x", R.face);

    /* 5. Alpha clamps to 0..255; colour keeps RGB only (backdrop marker). */
    static const int ain[] = { -40, 0, 128, 255, 300 }, aout[] = { 0, 0, 128, 255, 255 };
    for (int k = 0; k < 5; k++) {
        mkrun(&r, str, 256, 256, 13 * 256); r.alpha = ain[k];
        rc = run2(&r, 100);
        check(rc == OL_OK && R.n == 1 && R.alpha == aout[k], "alpha-clamp",
              "in=%d out=%d want %d", ain[k], R.alpha, aout[k]);
    }
    mkrun(&r, str, 256, 256, 13 * 256); r.color = 0xAB123456u;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.rgb == 0x123456u, "color-rgb-only", "rgb=%#x", (unsigned)R.rgb);

    /* 6. len and copy-in rules (unchanged by the unit fix). */
    static char big[1100];
    memset(big, 'a', sizeof big - 1);
    mkrun(&r, str, 256, 256, 13 * 256); r.len = -1;
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "len-negative", "rc=%ld", rc);
    mkrun(&r, big, 256, 256, 13 * 256); r.len = 1024;
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "len-1024", "rc=%ld", rc);
    mkrun(&r, big, 256, 256, 13 * 256); r.len = 1023;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.n == 1 && R.len == 1023 && (int)strlen(R.s) == 1023, "len-1023",
          "rc=%ld len=%d", rc, R.len);
    mkrun(&r, str, 256, 256, 13 * 256); r.s = 0;
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "len-null-string", "rc=%ld", rc);
    mkrun(&r, str, 256, 256, 13 * 256); r.len = 0; r.s = 0;
    rc = run2(&r, 100);
    check(rc == OL_OK && R.n == 1 && R.len == 0 && R.s[0] == 0, "len-zero", "rc=%ld", rc);
    mkrun(&r, str, 256, 256, 13 * 256); fault_ptr = str;
    rc = run2(&r, 100);
    fault_ptr = 0;
    check(rc == OL_ARGUMENT && R.n == 0, "copyin-fault", "rc=%ld", rc);
    mkrun(&r, str, 256, 256, 13 * 256); r.size = 4;
    rc = run2(&r, 100);
    check(rc == OL_ARGUMENT && R.n == 0, "bad-size", "rc=%ld", rc);
    check(wm2_text_info == LOGIT_GUI2_F_TEXT_RUN2, "info-bit", "%#x", wm2_text_info);

    printf("done %d checks\n", checks);
    return bad ? 1 : 0;
}
