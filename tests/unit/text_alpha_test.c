/* text_draw_run_a and the SYS_GUI_TEXT_RUN2 handler (wm_draw2_text.c) on a
 * host display, with the real text engine and shipped fonts. fb_blit_glyph is
 * stubbed to draw onto the SAME display through ol_display_blit_glyph_a at
 * alpha 255, so the legacy path and the alpha path are comparable pixel for
 * pixel. Prints "FAIL <name>" per failing check; exits 1 on any failure.
 * TEXT_ALPHA_IGNORED (the control) makes text.c drop alpha: "alpha128" fails.
 * struct logit_run2 carries x, y and px in 24.8 POINTS (logit_abi.h), so every
 * run below is built with whole points times 256 (P24); the checks themselves
 * are the ones this file always made. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "logit_abi.h"
#include "text.h"
#include "ttf.h"
#include "spinlock.h"
#include "openlogit_display.h"
#include "openlogit_display_blit.h"
#include "wm_draw2.h"

static const char *root;
static int bad;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); bad = 1; } \
                            else printf("PASS %s\n", name); } while (0)

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

#define W 400
#define H 80
static struct ol_display D;
static struct ol_pixel_target T;
static uint32_t PX[W * H];
static void *t_alloc(unsigned long n) { return malloc(n); }
static void t_free(void *p) { free(p); }
static void fill(uint32_t v) { for (int i = 0; i < W * H; i++) PX[i] = v; }

/* The legacy glyph path, landing on the same display. */
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{ ol_display_blit_glyph_a(&D, x, y, cov, w, h, w, color, 255); }

static int ch(uint32_t p, int sh) { return (int)((p >> sh) & 255); }
static int iabs(int v) { return v < 0 ? -v : v; }

static long cop(void *dst, const void *src, unsigned long n) { memcpy(dst, src, n); return 0; }
static int copyin(void *dst, const void *src, unsigned long n) { return (int)cop(dst, src, n); }

static long run2(struct logit_run2 *r, int pct)
{
    struct wm2_env e;
    memset(&e, 0, sizeof e);
    e.d = &D; e.scale_pct = pct; e.surf_w = W; e.surf_h = H; e.copyin = copyin;
    return wm2_text_run2(&e, (long)r, 0, 0);
}
/* x24, y24 and px24 are 24.8 points, as the ABI defines them. */
#define P24(v) ((v) * 256)
static void mkrun(struct logit_run2 *r, const char *s, int x24, int y24, int px24, int face,
                  unsigned color, int alpha, int weight)
{
    memset(r, 0, sizeof *r);
    r->size = sizeof *r; r->x = x24; r->y = y24; r->px = px24; r->face = face;
    r->color = color; r->s = s; r->len = (int)strlen(s); r->alpha = alpha; r->weight = weight;
}
static uint32_t *snap(void) { uint32_t *c = malloc(sizeof PX); memcpy(c, PX, sizeof PX); return c; }

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    root = argv[1];
    text_init();
    memset(&D, 0, sizeof D); memset(&T, 0, sizeof T);
    T.px = PX; T.w = W; T.h = H;
    D.allocate = t_alloc; D.release = t_free;
    ol_display_bind(&D, &T, 16, 8, 0);
    const char *str = "Hello, Wxyz 123";
    const uint32_t BG = 0x203040, FG = 0xE0C080;

    /* 1. alpha 255 is bit-identical to text_draw_run. */
    fill(BG);
    int e0 = text_draw_run(10, 10, str, (int)strlen(str), 20, 0, FG);
    uint32_t *legacy = snap();
    fill(BG);
    int e1 = text_draw_run_a(&D, 10, 10, str, (int)strlen(str), 20, 0, FG, 255);
    CHECK(e0 == e1 && memcmp(legacy, PX, sizeof PX) == 0, "alpha255-identical");
    int ink = 0; for (int i = 0; i < W * H; i++) ink += legacy[i] != BG;
    CHECK(ink > 100, "glyphs-drawn");

    /* 2. alpha 128: half src-over of the alpha-255 glyph over the background.
     * One glyph, so no two glyph boxes overlap and blend twice over one pixel. */
    const char *one = "W";
    fill(BG);
    text_draw_run(10, 10, one, 1, 40, 0, FG);
    free(legacy); legacy = snap();
    fill(BG);
    text_draw_run_a(&D, 10, 10, one, 1, 40, 0, FG, 128);
    int maxd = 0, partial = 0;
    for (int i = 0; i < W * H; i++) {
        for (int sh = 0; sh <= 16; sh += 8) {
            int full = ch(legacy[i], sh), bg = ch(BG, sh);
            int want = (full * 128 + bg * 127 + 127) / 255;
            int d = iabs(ch(PX[i], sh) - want);
            if (d > maxd) maxd = d;
        }
        if (PX[i] != BG && PX[i] != legacy[i]) partial++;
    }
    printf("alpha128 max delta %d, changed %d\n", maxd, partial);
    CHECK(maxd <= 1 && partial > 100, "alpha128");

    /* 3. alpha 0 draws nothing but still reports the advance. */
    fill(BG);
    int e2 = text_draw_run_a(&D, 10, 10, str, (int)strlen(str), 20, 0, FG, 0);
    int blank = 1; for (int i = 0; i < W * H; i++) if (PX[i] != BG) blank = 0;
    CHECK(blank && e2 == e0, "alpha0-blank-advance");
    free(legacy);

    /* 4. run2 at 100/125/150% lands where the legacy run does. */
    static const int pcts[] = {100, 125, 150};
    for (int k = 0; k < 3; k++) {
        int p = pcts[k];
        char nm[64];
        fill(BG);
        text_draw_run(5 * p / 100, 4 * p / 100, str, (int)strlen(str), 12 * p / 100, LOGIT_FACE_BOLD, FG);
        uint32_t *want = snap();
        fill(BG);
        struct logit_run2 r; mkrun(&r, str, P24(5), P24(4), P24(12), LOGIT_FACE_BOLD, FG, 255, 0);
        long rc = run2(&r, p);
        snprintf(nm, sizeof nm, "run2-places-like-legacy-%d", p);
        CHECK(rc == OL_OK && memcmp(want, PX, sizeof PX) == 0, nm);
        free(want);
    }

    /* 5. weight maps: >= 600 bold, otherwise regular; 0 defers to face. */
    {
        struct logit_run2 r; uint32_t *a, *b;
        fill(BG); text_draw_run(5, 5, str, (int)strlen(str), 16, LOGIT_FACE_BOLD, FG); a = snap();
        fill(BG); mkrun(&r, str, P24(5), P24(5), P24(16), 0, FG, 255, 700); run2(&r, 100); b = snap();
        CHECK(memcmp(a, b, sizeof PX) == 0, "weight700-is-bold"); free(a); free(b);
        fill(BG); text_draw_run(5, 5, str, (int)strlen(str), 16, 0, FG); a = snap();
        fill(BG); mkrun(&r, str, P24(5), P24(5), P24(16), LOGIT_FACE_BOLD, FG, 255, 400); run2(&r, 100); b = snap();
        CHECK(memcmp(a, b, sizeof PX) == 0, "weight400-overrides-bold"); free(a); free(b);
    }

    /* 6. invalid arguments. */
    {
        struct logit_run2 r;
        char big[1100]; memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = 0;
        fill(BG);
        mkrun(&r, str, P24(5), P24(5), P24(16), 0, FG, 255, 0); r.len = -1;
        CHECK(run2(&r, 100) == OL_ARGUMENT, "bad-len-negative");
        mkrun(&r, big, P24(5), P24(5), P24(16), 0, FG, 255, 0);
        CHECK(run2(&r, 100) == OL_ARGUMENT, "bad-len-too-long");
        mkrun(&r, str, P24(5), P24(5), P24(0), 0, FG, 255, 0);
        CHECK(run2(&r, 100) == OL_ARGUMENT, "bad-px-zero");
        mkrun(&r, str, P24(5), P24(5), P24(513), 0, FG, 255, 0);
        CHECK(run2(&r, 100) == OL_ARGUMENT, "bad-px-huge");
        mkrun(&r, str, P24(5), P24(5), P24(16), 0, FG, 255, 0); r.size = 4;
        CHECK(run2(&r, 100) == OL_ARGUMENT, "bad-size");
        int untouched = 1; for (int i = 0; i < W * H; i++) if (PX[i] != BG) untouched = 0;
        CHECK(untouched, "invalid-draws-nothing");
    }
    CHECK(wm2_text_info == LOGIT_GUI2_F_TEXT_RUN2, "info-bit");
    return bad ? 1 : 0;
}
