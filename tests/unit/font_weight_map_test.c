/* font_weight_map_test.c -- CSS weight 100..900 -> baked UI face slots.
 *
 * Links the real c/kernel/gui/text.c against the real fsroot/fonts (which now
 * carry ui.ttf 400, ui-medium.ttf 500, ui-semibold.ttf 600, ui-bold.ttf 700).
 * Checks, in order:
 *   - the slot table (nearest of 400/500/600/700, ties UP; weight 0 = legacy);
 *   - the stem of 'l' at 64 px grows strictly through 400, 500, 600, 700
 *     ("weight-order": the check -DTEXT_WEIGHT_BOOL must fail);
 *   - weight 650 resolves to the same face on every call (deterministic);
 *   - advances stay inside the regular..bold envelope and never shrink with weight;
 *   - what wm_draw2_text.c does for run2.weight=500 (text_face_for_weight) selects
 *     the medium face, and wm_draw2_text.c really calls it (argv[2] is its path).
 * argv[1] is the font root (a directory standing in for /).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "logit_abi.h"

int  text_measure(const char *s, int len, int px, int face);
int  text_draw_run(int x, int y, const char *s, int len, int px, int face, uint32_t color);
void text_init(void);
int  text_weight_slot(int weight);
unsigned text_face_for_weight(unsigned face, int weight);

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  kprintf(const char *f, ...) { (void)f; }
/* text.c takes text_lock around layout and carets; single-threaded here. */
uint64_t spin_lock_irqsave(void *l) { (void)l; return 0; }
void spin_unlock_irqrestore(void *l, uint64_t fl) { (void)l; (void)fl; }

static const char *g_root;

static char *slurp(const char *vpath, long *out)
{
    char full[1024];
    snprintf(full, sizeof full, "%s%s", g_root, vpath);
    FILE *f = fopen(full, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out = n;
    return b;
}
int vfs_size(const char *p)
{ long n; char *b = slurp(p, &n); if (!b) return -1; free(b); return (int)n; }
int vfs_read(const char *p, void *buf, int max)
{
    long n; char *b = slurp(p, &n);
    if (!b) return -1;
    if (n > max) n = max;
    memcpy(buf, b, (size_t)n);
    free(b);
    return (int)n;
}

/* Stem width of the last glyph blitted: coverage summed over its middle row,
 * in 1/255 px. 'l' is a plain vertical bar, so that row is the stem. */
static long g_stem, g_ink;
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{
    (void)x; (void)y; (void)color;
    if (!cov) return;
    long s = 0;
    for (int i = 0; i < w; i++) s += cov[(h / 2) * w + i];
    g_stem = s;
    for (int i = 0; i < w * h; i++) g_ink += cov[i];
}
void fb_target(void *s) { (void)s; }

static int fails;
static void check(int ok, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", ok ? "  ok  " : "  FAIL", what,
           detail && *detail ? "  --  " : "", detail ? detail : "");
    if (!ok) fails++;
}

static long stem_at(unsigned face)
{
    g_stem = 0;
    text_draw_run(0, 0, "l", 1, 64, (int)face, 0xffffffu);
    return g_stem;
}
static long ink_at(unsigned face)
{
    g_ink = 0;
    text_draw_run(0, 0, "Medium weight 123", 17, 32, (int)face, 0xffffffu);
    return g_ink;
}
static int adv_at(unsigned face)
{ return text_measure("Medium weight 123", 17, 32, (int)face); }

int main(int argc, char **argv)
{
    g_root = argc > 1 ? argv[1] : "fsroot";
    const char *wmsrc = argc > 2 ? argv[2] : "c/kernel/gui/wm_draw2_text.c";
    text_init();
    char d[256];

    /* 1. the slot table */
    static const int tab[][2] = {
        {1,400},{100,400},{400,400},{449,400},{450,500},{500,500},{549,500},
        {550,600},{600,600},{649,600},{650,700},{700,700},{900,700},{5000,700},
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if (text_weight_slot(tab[i][0]) != tab[i][1]) {
            ok = 0;
            snprintf(d, sizeof d, "weight %d -> %d, expected %d", tab[i][0],
                     text_weight_slot(tab[i][0]), tab[i][1]);
            break;
        }
    check(ok, "slot-table nearest of 400/500/600/700, ties up", ok ? "" : d);
    check(text_face_for_weight(LOGIT_FACE_BOLD, 0) == LOGIT_FACE_BOLD &&
          text_face_for_weight(LOGIT_FACE_MONO, 0) == LOGIT_FACE_MONO,
          "weight 0 keeps the legacy face mask", "");
    check(text_face_for_weight(0, 700) == LOGIT_FACE_BOLD &&
          text_face_for_weight(LOGIT_FACE_MONO, 700) == (LOGIT_FACE_MONO | LOGIT_FACE_BOLD),
          "weight 700 is LOGIT_FACE_BOLD", "");

    /* 2. stem width strictly increases through the four slots */
    unsigned f400 = text_face_for_weight(0, 400), f500 = text_face_for_weight(0, 500);
    unsigned f600 = text_face_for_weight(0, 600), f700 = text_face_for_weight(0, 700);
    long s4 = stem_at(f400), s5 = stem_at(f500), s6 = stem_at(f600), s7 = stem_at(f700);
    snprintf(d, sizeof d, "'l' stem at 64 px: %ld < %ld < %ld < %ld (/255 px)", s4, s5, s6, s7);
    check(s4 > 0 && s4 < s5 && s5 < s6 && s6 < s7, "weight-order", d);

    /* 3. weight 650 resolves deterministically (a tie, goes up to 700) */
    unsigned a = text_face_for_weight(0, 650), b = text_face_for_weight(0, 650);
    snprintf(d, sizeof d, "face 0x%x twice; stem %ld vs bold %ld", a, stem_at(a), s7);
    check(a == b && a == f700 && stem_at(a) == s7 && stem_at(b) == s7,
          "weight 650 resolves deterministically to the 700 slot", d);

    /* 4. advances stay within spec */
    int w4 = adv_at(f400), w5 = adv_at(f500), w6 = adv_at(f600), w7 = adv_at(f700);
    snprintf(d, sizeof d, "%d <= %d <= %d <= %d px, bold %+.2f%%", w4, w5, w6, w7,
             100.0 * (w7 - w4) / w4);
    check(w4 <= w5 && w5 <= w6 && w6 <= w7 && w7 > w4 && (w7 - w4) * 100 < w4 * 8,
          "advances monotone and within 8% of regular", d);
    long i4 = ink_at(f400), i5 = ink_at(f500), i6 = ink_at(f600), i7 = ink_at(f700);
    snprintf(d, sizeof d, "%ld < %ld < %ld < %ld", i4, i5, i6, i7);
    check(i4 < i5 && i5 < i6 && i6 < i7, "ink grows with weight", d);
    unsigned m5 = text_face_for_weight(LOGIT_FACE_MONO, 500);
    check(adv_at(m5) == adv_at(LOGIT_FACE_MONO) && m5 == LOGIT_FACE_MONO,
          "mono at 500 stays the regular mono face", "");

    /* 5. run2 at weight 500 selects the medium face */
    FILE *f = fopen(wmsrc, "r");
    char *src = NULL; long n = 0;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
             src = calloc(1, (size_t)n + 1); if (src && fread(src, 1, (size_t)n, f) != (size_t)n) n = 0; fclose(f); }
    check(src && strstr(src, "text_face_for_weight(face, run.weight)") != NULL,
          "wm_draw2_text.c routes run2.weight through text_face_for_weight", wmsrc);
    snprintf(d, sizeof d, "stem %ld (400 %ld, 600 %ld)", s5, s4, s6);
    check(f500 != f400 && f500 != f700 && s5 > s4 && s5 < s6 && stem_at(f500) == s5,
          "run2 weight 500 selects the medium face", d);

    printf("font_weight_map_test: %s (%d failed)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
