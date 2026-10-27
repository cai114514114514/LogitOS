/* aui-host v2 handler: SYS_GUI_BACKDROP and SYS_GUI_BACKDROP_CLEAR, served by
 * the REAL kernel handlers (added by ui-aui-core for aui_backdrop).
 *
 * c/kernel/gui/wm_backdrop.c is #included (not re-implemented), so a region is
 * recorded and its pixels marked exactly as wm.c's `case SYS_GUI_BACKDROP`
 * does: wm2_backdrop() on a wm2_env bound to the harness display, window slot
 * 0, at the harness scale. The harness renders ONE window and no desktop, so a
 * marked pixel shows its fill_rgb -- which is what the compositor itself shows
 * for a region while it drags the window, and what Reduce Transparency shows.
 * The desktop composite (wm_backdrop_composite) is tested by
 * test-wm-backdrop-regions, not here.
 *
 * Registers through AUI_HOST_V2, so v2 mode reports LOGIT_GUI2_F_BACKDROP in
 * SYS_GUI2_INFO. The harness itself is not edited. BLIT2 needs no file of its
 * own: v2_state.c serves it with wm_draw2_state.c. */
#include <stdlib.h>
#include <string.h>

#include "aui_host.h"
#include "wm_backdrop.c"

static int host_copyin(void *dst, const void *usrc, unsigned long n)
{
    memcpy(dst, usrc, n);
    return 0;
}

static void bd_env(struct aui_host *h, struct wm2_env *e)
{
    memset(e, 0, sizeof *e);
    e->d = &h->disp;
    e->scale_pct = h->pct;
    e->surf_w = h->w;
    e->surf_h = h->h;
    e->slot = 0;
    e->copyin = host_copyin;
    e->alloc = malloc;
    e->release = free;
}

static long v2_backdrop_call(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    bd_env(h, &e);
    return wm2_backdrop(&e, a, b, c);
}

static long v2_backdrop_clear_call(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    bd_env(h, &e);
    return wm2_backdrop_clear(&e, a, b, c);
}

static void v2_backdrop_reg(struct aui_host *h)
{
    (void)h;
    wm2_backdrop_win_reset(0);          /* a fresh host is a fresh window */
    aui_host_on(SYS_GUI_BACKDROP, v2_backdrop_call);
    aui_host_on(SYS_GUI_BACKDROP_CLEAR, v2_backdrop_clear_call);
}

AUI_HOST_V2(backdrop, LOGIT_GUI2_F_BACKDROP, v2_backdrop_reg)
