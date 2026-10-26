/* Budget: a THICK glass panel costs at most 2.5x a REGULAR one per panel
 * pixel (same panel, same scratch reuse). Also reports THIN and ULTRA. */
#include "openlogit_display.h"
#include "openlogit_glass_material.h"
#include "bench_budget.h"
#include <stdint.h>
#include <string.h>

int (*gfx_ctx_hook)(void);

enum { SW = 640, SH = 480, PX = 160, PY = 100, PW = 320, PH = 240 };
static uint32_t px[SW * SH];
static struct ol_display d;
static struct ol_pixel_target t;
static struct ol_glass_material mat[4];

static void render(void *arg)
{
    ol_display_liquid_glass_ex(&d, PX, PY, PW, PH, 24, 0, &mat[*(int *)arg]);
}

int main(void)
{
    uint32_t v = 7;
    for (int i = 0; i < SW * SH; i++) {
        v = v * 1664525u + 1013904223u;
        px[i] = (v >> 8) & 0xFFFFFF;
    }
    d.allocate = malloc;
    d.release = free;
    t.px = px;
    t.w = SW;
    t.h = SH;
    if (ol_display_bind(&d, &t, 16, 8, 0) != OL_OK) return 2;
    for (int i = 0; i < 4; i++) {
        mat[i].tint = 0x8090A0;
        mat[i].opacity = 100;
        mat[i].blur = (uint8_t)i;
    }
    int tier[4] = {0, 1, 2, 3};
    struct bb_stats reg = bb_measure(render, &tier[0], 4);
    struct bb_stats thin = bb_measure(render, &tier[1], 4);
    struct bb_stats thick = bb_measure(render, &tier[2], 4);
    struct bb_stats ultra = bb_measure(render, &tier[3], 4);
    printf("median ns per %dx%d panel: REGULAR %.0f THIN %.0f THICK %.0f ULTRA %.0f\n",
           PW, PH, reg.median_ns, thin.median_ns, thick.median_ns, ultra.median_ns);
    bb_ratio("glass THICK vs REGULAR", thick, reg, 2.5);
    ol_display_destroy(&d);
    return bb_exit();
}
