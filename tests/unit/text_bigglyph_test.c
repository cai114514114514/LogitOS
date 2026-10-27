/* text_bigglyph_test.c -- glyphs above ~190 px used to vanish (text.c's
 * rastbuf is 200*200). Links the real c/kernel/gui/text.c and the shipped
 * fonts; fb_blit_glyph is stubbed to accumulate every blitted coverage mask
 * into a canvas. Prints "FAIL <name>" per failing check, exits 1 on any.
 *   ink400   '8' at 400 px draws ink, and the box equals text_raster_extent
 *   banded   an uncached (kmalloc refused) render equals the cached render,
 *            and retains nothing
 *   budget   300 px glyphs are cached (second draw allocates nothing), cache
 *            bytes never exceed TEXT_GLYPH_BUDGET, and old entries are evicted
 * Built with -DTEXT_GLYPH_CAP_TEST -DTEXT_GLYPH_BUDGET=<small>. The control
 * adds -DTEXT_RASTBUF_LEGACY: "FAIL ink400" must appear. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "logit_abi.h"
#include "text.h"
#include "ttf.h"
#include "glyphras.h"
#include "spinlock.h"

static const char *root;
static int bad;
#define CHECK(c, name) do { if (!(c)) { printf("FAIL %s\n", name); bad = 1; } \
                            else printf("PASS %s\n", name); } while (0)

static unsigned long refuse_above;      /* kmalloc returns NULL above this (0 = off) */
static unsigned long max_alloc;
static int n_alloc;
void *kmalloc(unsigned long n)
{
    if (refuse_above && n > refuse_above) return NULL;
    if (n > max_alloc) max_alloc = n;
    n_alloc++;
    return malloc(n);
}
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

long text_gc_bytes(void);

#define CW 1400
#define CH 1400
#define OX 400
#define OY 400
static uint8_t canvas[CW * CH];
static int blits, bx0, by0, bx1, by1;
void fb_blit_glyph(int x, int y, const unsigned char *cov, int w, int h, uint32_t color)
{
    (void)color;
    blits++;
    if (blits == 1 || x < bx0) bx0 = x;
    if (blits == 1 || y < by0) by0 = y;
    if (blits == 1 || x + w > bx1) bx1 = x + w;
    if (blits == 1 || y + h > by1) by1 = y + h;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int cx = x + i + OX, cy = y + j + OY;
        if (cx < 0 || cy < 0 || cx >= CW || cy >= CH) continue;
        if (cov[j * w + i] > canvas[cy * CW + cx]) canvas[cy * CW + cx] = cov[j * w + i];
    }
}
static void clear(void) { memset(canvas, 0, sizeof canvas); blits = 0; }
static long ink(void) { long s = 0; for (int i = 0; i < CW * CH; i++) s += canvas[i]; return s; }

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    root = argv[1];
    text_init();
    static uint8_t ref[CW * CH];

    /* ink400 */
    clear();
    text_draw_run(0, 0, "8", 1, 400, 0, 0xFFFFFF);
    long i400 = ink();
    int w = bx1 - bx0, h = by1 - by0;
    CHECK(i400 > 0 && blits == 1, "ink400");
    {
        char path[2048]; snprintf(path, sizeof path, "%s/fonts/ui.ttf", root);
        FILE *ff = fopen(path, "rb"); long n = 0; uint8_t *buf = NULL;
        if (ff) { fseek(ff, 0, SEEK_END); n = ftell(ff); fseek(ff, 0, SEEK_SET);
                  buf = malloc((size_t)n); if (fread(buf, 1, (size_t)n, ff) != (size_t)n) n = 0; fclose(ff); }
        static struct ttf_font tf;
        int ex = 0, et = 0, ew = 0, eh = 0;
        int ok = buf && n && ttf_parse(buf, (int)n, &tf) == 0 &&
                 text_raster_extent(&tf, ttf_glyph_id(&tf, '8'), 400, &ex, &et, &ew, &eh) == 0;
        CHECK(ok && w == ew && h == eh && ew * eh > 200 * 200, "ink400-box");
    }

    /* banded == one-shot (banded first: nothing cached yet at this key) */
    long b0 = text_gc_bytes();
    clear();
    refuse_above = 1000;                    /* every glyph buffer refused */
    text_draw_run(0, 0, "W", 1, 350, 0, 0xFFFFFF);
    refuse_above = 0;
    memcpy(ref, canvas, sizeof ref);
    long ib = ink();
    CHECK(ib > 0, "banded-ink");
    CHECK(text_gc_bytes() == b0, "banded-uncached");
    clear();
    text_draw_run(0, 0, "W", 1, 350, 0, 0xFFFFFF);
    CHECK(memcmp(ref, canvas, sizeof ref) == 0 && ink() == ib, "banded-equals-oneshot");
    CHECK(text_gc_bytes() > b0, "oneshot-cached");

    /* budget */
    unsigned long budget =
#ifdef TEXT_GLYPH_BUDGET
        TEXT_GLYPH_BUDGET;
#else
        4u << 20;
#endif
    clear();
    int a0 = n_alloc;
    text_draw_run(0, 0, "M", 1, 300, 0, 0xFFFFFF);
    int a1 = n_alloc;
    text_draw_run(0, 0, "M", 1, 300, 0, 0xFFFFFF);
    CHECK(a1 > a0 && n_alloc == a1 && max_alloc > 200 * 200, "cached-300");
    int over = 0;
    const char *set = "WMBQGODRHKNUVXYZ@%&";
    for (const char *c = set; *c; c++) {
        text_draw_run(0, 0, c, 1, 300, 0, 0xFFFFFF);
        if ((unsigned long)text_gc_bytes() > budget) over = 1;
    }
    CHECK(!over && text_gc_bytes() > 0, "budget-held");
    int a2 = n_alloc;
    text_draw_run(0, 0, "M", 1, 300, 0, 0xFFFFFF);
    CHECK(n_alloc > a2, "evicted");
    return bad;
}
