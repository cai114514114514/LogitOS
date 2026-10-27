/* aui-host v2 handlers: SYS_GUI_SHADOW2 and SYS_GUI_GLASS2, served by the REAL
 * kernel handlers.
 *
 * c/kernel/gui/wm_draw2_fx.c is #included (not re-implemented), so the host
 * renders every shadow and glass exactly as wm.c's `case SYS_GUI_SHADOW2` /
 * `SYS_GUI_GLASS2` would: wm2_shadow2() / wm2_glass2() on a wm2_env bound to
 * the harness display, at the harness scale. Only the env is the harness's:
 * copyin is a memcpy (the app and the "kernel" share an address space here).
 *
 * Every SHADOW2 request is also copied into aui_fx_log[] (before the handler
 * runs) so a test can assert the call SHAPE: how many, with which fields.
 * aui_fx_nshadow counts all of them, the log keeps the first AUI_FX_LOG.
 * aui-glass-controls reuses this file unchanged.
 *
 * Registers through AUI_HOST_V2, so v2 mode reports SHADOW2 | GLASS2 in
 * SYS_GUI2_INFO. The harness itself is not edited. */
#include <stdlib.h>
#include <string.h>

#include "aui_host.h"
#include "wm_draw2_fx.c"

#define AUI_FX_LOG 8
struct logit_shadow2 aui_fx_log[AUI_FX_LOG];
int aui_fx_nshadow;

static int host_copyin(void *dst, const void *usrc, unsigned long n)
{
    memcpy(dst, usrc, n);
    return 0;
}

static void fx_env(struct aui_host *h, struct wm2_env *e)
{
    memset(e, 0, sizeof *e);
    e->d = &h->disp;
    e->scale_pct = h->pct;
    e->surf_w = h->w;
    e->surf_h = h->h;
    e->copyin = host_copyin;
    e->alloc = malloc;
    e->release = free;
}

static long v2_shadow2_call(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    if (aui_fx_nshadow < AUI_FX_LOG && a) memcpy(&aui_fx_log[aui_fx_nshadow], (const void *)a, sizeof aui_fx_log[0]);
    aui_fx_nshadow++;
    fx_env(h, &e);
    return wm2_shadow2(&e, a, b, c);
}

static long v2_glass2_call(struct aui_host *h, long a, long b, long c)
{
    struct wm2_env e;
    fx_env(h, &e);
    return wm2_glass2(&e, a, b, c);
}

static void v2_fx_reg(struct aui_host *h)
{
    (void)h;
    aui_host_on(SYS_GUI_SHADOW2, v2_shadow2_call);
    aui_host_on(SYS_GUI_GLASS2, v2_glass2_call);
}

AUI_HOST_V2(fx, LOGIT_GUI2_F_SHADOW2 | LOGIT_GUI2_F_GLASS2, v2_fx_reg)
