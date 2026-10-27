/* aui-host v2 handler: SYS_GUI_PATH, served by the REAL kernel handler.
 *
 * c/kernel/gui/wm_draw2_path.c is #included (not re-implemented), so the host
 * renders every path exactly as wm.c's `case SYS_GUI_PATH` would: wm2_path()
 * on a wm2_env bound to the harness display, at the harness scale. Only the
 * env is the harness's: copyin is a memcpy (the app and the "kernel" share an
 * address space here), the clip hooks are absent (this file does not claim
 * LOGIT_GUI2_F_CLIP), and `d` is the display the legacy calls draw into.
 *
 * Registers through AUI_HOST_V2, so v2 mode reports LOGIT_GUI2_F_PATH in
 * SYS_GUI2_INFO. The harness itself is not edited. */
#include <stdlib.h>
#include <string.h>

#include "aui_host.h"
#include "wm_draw2_path.c"

static int host_copyin(void *dst, const void *usrc, unsigned long n)
{
    memcpy(dst, usrc, n);
    return 0;
}

static long v2_path_call(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    memset(&e, 0, sizeof e);
    e.d = &h->disp;
    e.scale_pct = h->pct;
    e.surf_w = h->w;
    e.surf_h = h->h;
    e.copyin = host_copyin;
    e.alloc = malloc;
    e.release = free;
    return wm2_path(&e, a, b, c);
}

static void v2_path_reg(struct aui_host *h)
{
    (void)h;
    aui_host_on(SYS_GUI_PATH, v2_path_call);
}

AUI_HOST_V2(path, LOGIT_GUI2_F_PATH, v2_path_reg)
