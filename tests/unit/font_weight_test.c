/* font_weight_test.c -- does a bold text run actually select a bold FACE?
 *
 * WHY THIS EXISTS AND WHAT IT IS NOT.  test-font already proves each .ttf on
 * the disk parses and that our outlines match fontTools command for command;
 * test-glyph-agree proves the rasteriser matches an independent oracle.
 * Neither can see the failure this file is about, because neither asks a
 * QUESTION ABOUT SELECTION: for four rounds of browser work every <h1> and
 * every <strong> on the web rendered at regular weight while both of those
 * gates were green, because `bold` was computed in css_engine.c, carried in
 * layout.h's display item, and then dropped -- there was no field for it in
 * struct logit_run and no bold face on the disk to put in one.
 *
 * So this links the REAL c/kernel/gui/text.c against the REAL fsroot/fonts and
 * asks four things a bold face answers differently from a regular one:
 *
 *   1. bold is WIDER in the proportional face.  This is the one that matters
 *      for correctness rather than looks: layout measures a run and the
 *      painter draws it, and if the two disagree about weight the text runs
 *      off the end of its own box.  Measured on the shipped pair: +4.94% at
 *      32 px for "Heading Bold Test 123", 324 px -> 340 px.
 *   2. bold has MORE INK.  A face that measured wider and drew the same
 *      outlines would be a metrics bug that looks like nothing.
 *   3. bold does NOT change the mono advance.  A monospaced bold whose
 *      advance moved would not be monospaced, and the Terminal's grid is built
 *      out of that number.
 *   4. line height is UNCHANGED across the pair.  c/kernel/gui/text.c puts the
 *      baseline at hhea.ascent scaled by px, taken from the FIRST font of the
 *      run's fallback set -- so a bold face whose hhea disagreed with its
 *      regular twin's would shift every bold line against the regular text
 *      beside it.  That is a property of the upstream sources, not of anything
 *      in this tree, so nothing else would notice it changing.
 *
 * `argv[1]` is a directory standing in for the filesystem root, so the
 * negative control can run the identical binary against a root with one font
 * missing and watch each check fail -- see test-font-weight-negctl.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "logit_abi.h"          /* LOGIT_FACE_MONO / LOGIT_FACE_BOLD */

int  text_measure(const char *s, int len, int px, int face);
int  text_draw_run(int x, int y, const char *s, int len, int px, int face, uint32_t color);
void text_init(void);
int  text_line_height(int px);

/* ---- the four things c/kernel/gui/text.c needs from the kernel ---- */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void  kprintf(const char *f, ...) { (void)f; }        /* text_init's misses are the control's subject */

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

/* The blit target: total coverage, not pixels. text.c hands the rasterised
 * glyph's alpha mask to fb_blit_glyph, so summing it is a direct measure of
 * how much ink the face puts down -- no frame buffer, no compositor. */
static long g_ink;
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{
    (void)x; (void)y; (void)color;
    if (!cov) return;
    for (int i = 0; i < w * h; i++) g_ink += cov[i];
}
void fb_target(void *s) { (void)s; }

/* ---------------------------------------------------------------------- */

#define SAMPLE "Heading Bold Test 123"
#define PX     32

static int fails;
static void check(int ok, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", ok ? "  ok  " : "  FAIL", what,
           detail && *detail ? "  --  " : "", detail ? detail : "");
    if (!ok) fails++;
}

struct row { int width; long ink; int lh; };

static struct row measure(int face)
{
    struct row r;
    int n = (int)strlen(SAMPLE);
    r.width = text_measure(SAMPLE, n, PX, face);
    g_ink = 0;
    text_draw_run(0, 0, SAMPLE, n, PX, face, 0xffffffu);
    r.ink = g_ink;
    r.lh = text_line_height(PX);
    return r;
}

int main(int argc, char **argv)
{
    g_root = argc > 1 ? argv[1] : "fsroot";
    text_init();

    struct row ur = measure(0);
    struct row ub = measure(LOGIT_FACE_BOLD);
    struct row mr = measure(LOGIT_FACE_MONO);
    struct row mb = measure(LOGIT_FACE_MONO | LOGIT_FACE_BOLD);

    printf("font_weight_test: root=%s  \"%s\" at %d px\n", g_root, SAMPLE, PX);
    printf("  %-14s %7s %10s %6s\n", "face", "width", "ink", "line_h");
    printf("  %-14s %7d %10ld %6d\n", "ui regular",   ur.width, ur.ink, ur.lh);
    printf("  %-14s %7d %10ld %6d\n", "ui bold",      ub.width, ub.ink, ub.lh);
    printf("  %-14s %7d %10ld %6d\n", "mono regular", mr.width, mr.ink, mr.lh);
    printf("  %-14s %7d %10ld %6d\n", "mono bold",    mb.width, mb.ink, mb.lh);

    char d[256];
    if (ur.width <= 0 || mr.width <= 0) {
        printf("  FAIL  the REGULAR faces did not load -- every check below would be vacuous\n");
        printf("font_weight_test: FAIL (fonts missing under %s)\n", g_root);
        return 2;
    }

    snprintf(d, sizeof d, "%d px vs %d px (%+.2f%%)",
             ub.width, ur.width, 100.0 * (ub.width - ur.width) / ur.width);
    check(ub.width > ur.width, "ui bold is WIDER than ui regular", d);

    snprintf(d, sizeof d, "%ld vs %ld (%+.2f%%)",
             ub.ink, ur.ink, 100.0 * (double)(ub.ink - ur.ink) / (double)ur.ink);
    check(ub.ink > ur.ink, "ui bold puts down MORE INK", d);

    snprintf(d, sizeof d, "%d px both", mb.width);
    check(mb.width == mr.width, "mono bold keeps the mono ADVANCE", d);

    snprintf(d, sizeof d, "%ld vs %ld (%+.2f%%)",
             mb.ink, mr.ink, 100.0 * (double)(mb.ink - mr.ink) / (double)mr.ink);
    check(mb.ink > mr.ink, "mono bold puts down MORE INK", d);

    snprintf(d, sizeof d, "%d / %d / %d / %d", ur.lh, ub.lh, mr.lh, mb.lh);
    check(ur.lh == ub.lh && mr.lh == mb.lh,
          "line height is the SAME for both weights", d);

    /* The pitch check, and the reason it is here: with mono-bold.ttf absent,
     * an earlier fallback order walked past the missing bold mono face all the
     * way to the PROPORTIONAL ui face, so bold <code> stopped being
     * monospaced. Losing a weight is a degradation; losing the pitch is a
     * different font. This holds whether or not any bold file exists. */
    snprintf(d, sizeof d, "mono bold %d px, ui bold %d px, mono regular %d px",
             mb.width, ub.width, mr.width);
    check(mb.width == mr.width && mb.width != ub.width,
          "a bold MONO run stayed monospaced", d);

    printf("font_weight_test: %s (%d failed)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
