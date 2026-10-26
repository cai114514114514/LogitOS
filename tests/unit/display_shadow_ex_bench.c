/* Budget: ol_display_shadow_ex vs the legacy ol_display_shadow, same binary. */
#include <stdlib.h>
#include <string.h>
#include "openlogit_display_shadow.h"
#include "bench_budget.h"

int (*gfx_ctx_hook)(void);

static struct ol_display D;
static uint32_t *PX;
enum { CW = 560, CH = 460 };
static int RAD[4] = { 12, 12, 12, 12 };

/* The legacy blur is the whole falloff extent, a gaussian sigma reaches about
 * 3 sigma, so the equal-looking legacy call is blur = 3 * sigma = 24. */
static void legacy(void *a) { (void)a; ol_display_shadow(&D, 80, 80, 400, 300, 12, 4, 24, 90); }
static void legacy8(void *a) { (void)a; ol_display_shadow(&D, 80, 80, 400, 300, 12, 4, 8, 90); }
static void exact(void *a)
{
    (void)a;
    bb_sink += (unsigned long)ol_display_shadow_ex(&D, 80, 80, 400, 300, RAD, 0, 4 * 256, 8 * 256, 0, 0, 90, 0);
}

int main(void)
{
    PX = calloc((size_t)CW * CH, 4);
    D.target.px = PX; D.target.w = CW; D.target.h = CH;
    D.rpos = 16; D.gpos = 8; D.bpos = 0;
    D.allocate = (void *(*)(unsigned long))malloc; D.release = free;
    struct bb_stats ref = bb_measure(legacy, 0, 200);
    struct bb_stats ref8 = bb_measure(legacy8, 0, 200);
    struct bb_stats cand = bb_measure(exact, 0, 200);
    printf("legacy(blur 24) %.0f ns, legacy(blur 8) %.0f ns, shadow_ex(sigma 8) %.0f ns\n",
           ref.median_ns, ref8.median_ns, cand.median_ns);
    printf("info: shadow_ex / legacy(blur 8) = %.2f (band 2.5x wider)\n", cand.median_ns / ref8.median_ns);
    bb_ratio("shadow_ex_r12_s8_400x300", cand, ref, 1.5);
    return bb_exit();
}
