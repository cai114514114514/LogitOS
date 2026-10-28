/* ui-aui-controls: SYS_GUI_PATH served by the REAL kernel handler (as
 * tests/unit/aui_host/v2_path.c does), plus a spy hook so the test can see every
 * path, paint and stroke aui hands the kernel. Replaces v2_path.c in this
 * test's source list. */
#include <stdlib.h>
#include <string.h>

#include "aui_host.h"
#include "wm_draw2_path.c"

void (*uac_path_spy)(const struct logit_path2 *, const struct logit_paint2 *, const struct logit_stroke2 *);

static int host_copyin(void *dst, const void *usrc, unsigned long n)
{
    memcpy(dst, usrc, n);
    return 0;
}

static long uac_path_call(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    if (uac_path_spy)
        uac_path_spy((const struct logit_path2 *)a, (const struct logit_paint2 *)b, (const struct logit_stroke2 *)c);
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

static void uac_path_reg(struct aui_host *h)
{
    (void)h;
    aui_host_on(SYS_GUI_PATH, uac_path_call);
}

AUI_HOST_V2(uac_path, LOGIT_GUI2_F_PATH, uac_path_reg)
