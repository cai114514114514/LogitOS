/* ui_aui_nav_icons.c -- the REAL vector symbols for ui_aui_nav_test.c's
 * review dumps (--dump only; the checks keep the harness's placeholder
 * icons). c/kernel/gui/icons.c rasterises a coverage tile and hands it to
 * fb_blit_glyph; the stub below blends that tile into the harness surface,
 * honouring the harness clip. Its own translation unit so the kernel headers
 * icons.c needs never meet aui.h. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aui_host.h"
#include "icons.h"
#include "spinlock.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
void spin_lock(spinlock_t *l) { (void)l; }
void spin_unlock(spinlock_t *l) { (void)l; }

static struct aui_host *g_h;

void fb_blit_glyph(int x, int y, const uint8_t *cov, int w, int h, uint32_t color)
{
    struct aui_host *H = g_h;
    if (!H || !cov) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int px = x + i, py = y + j, a = cov[j * w + i];
            if (!a || px < 0 || py < 0 || px >= H->w || py >= H->h) continue;
            if (H->clip_on && (px < H->clx0 || py < H->cly0 || px >= H->clx1 || py >= H->cly1)) continue;
            uint32_t d = H->px[(size_t)py * H->w + px], o = 0;
            for (int sh = 0; sh <= 16; sh += 8) {
                int sc = (int)((color >> sh) & 255), dc = (int)((d >> sh) & 255);
                o |= (uint32_t)((sc * a + dc * (255 - a) + 127) / 255) << sh;
            }
            H->px[(size_t)py * H->w + px] = o;
        }
}

/* Device coordinates, like wm.c's SYS_GUI_ICON after S(). Weight 2 (3.7.1). */
void nav_dump_icon(struct aui_host *h, int id, int x, int y, int px, unsigned color)
{
    g_h = h;
    icon_draw_w(id, x, y, px, color & 0xFFFFFFu, 2);
    g_h = 0;
}
