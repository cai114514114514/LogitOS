/* aui-host v2 handler: SYS_GUI_CLIP_PUSH/POP, GROUP_BEGIN/END and BLIT2, served by
 * the REAL kernel handlers.
 *
 * c/kernel/gui/wm_draw2_state.c is #included (not re-implemented): the host renders
 * every clip level, opacity group and BLIT2 exactly as wm.c's cases would, on a
 * wm2_env bound to the harness display. The env's clip hooks are a copy of
 * fb_clip_push / fb_clip_pop acting on the HARNESS surface clip (h->clip_on,
 * clx0..cly1: the same state SYS_GUI_CLIP sets), with fb's 16-deep ledger; copyin
 * is a memcpy and alloc / release are malloc / free. Like fb_sdk(), every call
 * first rebinds the display from the surface, clip included, and rebinds again
 * on the way out so a legacy draw after a v2 call sees the surface's clip.
 *
 * It also answers SYS_GUI2_INFO(1) with the window's auto-close counter
 * (wm2_state_err_count) and closes whatever a frame left open at SYS_GUI_FLUSH /
 * FLUSH_RECT (wm2_state_win_flush), as the router does. Registered through
 * AUI_HOST_V2, so v2 mode reports CLIP | GROUP | BLIT2 in SYS_GUI2_INFO. The
 * harness itself is not edited.
 *
 * Test knobs: v2_state_limit_group / v2_state_limit_clip make the next calls of
 * that kind answer OL_LIMIT (the stack is untouched, as the real handler's
 * OL_LIMIT leaves it). */
#include <stdlib.h>
#include <string.h>

#include "aui_host.h"
#include "wm_draw2_state.c"

int v2_state_limit_group, v2_state_limit_clip;

#define HF_DEPTH 16
static struct { int on, x0, y0, x1, y1; } hstk[HF_DEPTH];
static int hdepth;

static void host_bind(struct aui_host *h)
{
    struct ol_pixel_target t = { h->px, h->w, h->h, h->clip_on, h->clx0, h->cly0, h->clx1, h->cly1 };
    ol_display_bind(&h->disp, &t, 16, 8, 0);
}

static int host_copyin(void *dst, const void *usrc, unsigned long n)
{
    memcpy(dst, usrc, n);
    return 0;
}

/* fb_clip_push (fb.c) on the harness surface. */
static int host_clip_push(void *o, int x0, int y0, int x1, int y1)
{
    struct aui_host *h = o;
    if (hdepth >= HF_DEPTH) return OL_LIMIT;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    hstk[hdepth].on = h->clip_on; hstk[hdepth].x0 = h->clx0; hstk[hdepth].y0 = h->cly0;
    hstk[hdepth].x1 = h->clx1; hstk[hdepth].y1 = h->cly1;
    hdepth++;
    if (h->clip_on) {
        if (x0 < h->clx0) x0 = h->clx0;
        if (y0 < h->cly0) y0 = h->cly0;
        if (x1 > h->clx1) x1 = h->clx1;
        if (y1 > h->cly1) y1 = h->cly1;
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    h->clip_on = 1; h->clx0 = x0; h->cly0 = y0; h->clx1 = x1; h->cly1 = y1;
    return OL_OK;
}

static void host_clip_pop(void *o)
{
    struct aui_host *h = o;
    if (hdepth <= 0) return;
    hdepth--;
    h->clip_on = hstk[hdepth].on; h->clx0 = hstk[hdepth].x0; h->cly0 = hstk[hdepth].y0;
    h->clx1 = hstk[hdepth].x1; h->cly1 = hstk[hdepth].y1;
}

static void host_env(struct aui_host *h, struct wm2_env *e)
{
    memset(e, 0, sizeof *e);
    e->d = &h->disp;
    e->scale_pct = h->pct;
    e->surf_w = h->w;
    e->surf_h = h->h;
    e->slot = 0;
    e->copyin = host_copyin;
    e->clip_push = host_clip_push;
    e->clip_pop = host_clip_pop;
    e->alloc = malloc;
    e->release = free;
    e->opaque = h;
}

typedef long (*v2s_fn)(struct wm2_env *, long, long, long);

static long v2s_call(struct aui_host *h, v2s_fn fn, long a, long b, long c)
{
    struct wm2_env e;
    host_env(h, &e);
    host_bind(h);
    long rc = fn(&e, a, b, c);
    host_bind(h);
    return rc;
}

static long v2s_clip_push(struct aui_host *h, long a, long b, long c)
{ return v2_state_limit_clip ? OL_LIMIT : v2s_call(h, wm2_clip_push, a, b, c); }
static long v2s_clip_pop(struct aui_host *h, long a, long b, long c)
{ return v2s_call(h, wm2_clip_pop, a, b, c); }
static long v2s_group_begin(struct aui_host *h, long a, long b, long c)
{ return v2_state_limit_group ? OL_LIMIT : v2s_call(h, wm2_group_begin, a, b, c); }
static long v2s_group_end(struct aui_host *h, long a, long b, long c)
{ return v2s_call(h, wm2_group_end, a, b, c); }
static long v2s_blit2(struct aui_host *h, long a, long b, long c)
{ return v2s_call(h, wm2_blit2, a, b, c); }

static long v2s_info(struct aui_host *h, long a, long b, long c)
{
    if (a == 1) return (long)wm2_state_err_count(0);
    return aui_host_sys_builtin(h, SYS_GUI2_INFO, a, b, c);
}

static long v2s_flush_hook(struct aui_host *h, long n, long a, long b, long c)
{
    struct wm2_env e;
    host_env(h, &e);
    host_bind(h);
    wm2_state_win_flush(&e);
    host_bind(h);
    return aui_host_sys_builtin(h, n, a, b, c);
}
static long v2s_flush(struct aui_host *h, long a, long b, long c)
{ return v2s_flush_hook(h, SYS_GUI_FLUSH, a, b, c); }
static long v2s_flush_rect(struct aui_host *h, long a, long b, long c)
{ return v2s_flush_hook(h, SYS_GUI_FLUSH_RECT, a, b, c); }

static void v2_state_reg(struct aui_host *h)
{
    (void)h;
    /* a fresh host is a fresh window: drop the module's arena, levels and counters,
     * and fb's ledger (the previous host's surface is gone) */
    wm2_state_win_reset(0);
    g_fbn = 0;
    hdepth = 0;
    v2_state_limit_group = v2_state_limit_clip = 0;
    aui_host_on(SYS_GUI_CLIP_PUSH, v2s_clip_push);
    aui_host_on(SYS_GUI_CLIP_POP, v2s_clip_pop);
    aui_host_on(SYS_GUI_GROUP_BEGIN, v2s_group_begin);
    aui_host_on(SYS_GUI_GROUP_END, v2s_group_end);
    aui_host_on(SYS_GUI_BLIT2, v2s_blit2);
    aui_host_on(SYS_GUI2_INFO, v2s_info);
    aui_host_on(SYS_GUI_FLUSH, v2s_flush);
    aui_host_on(SYS_GUI_FLUSH_RECT, v2s_flush_rect);
}

AUI_HOST_V2(state, LOGIT_GUI2_F_CLIP | LOGIT_GUI2_F_GROUP | LOGIT_GUI2_F_BLIT2, v2_state_reg)
