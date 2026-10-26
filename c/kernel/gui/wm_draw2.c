/* GUI ABI v2 router: SYS_GUI2_INFO, and every other v2 number to the module
 * that owns it. See wm_draw2.h for the shape and the contracts. Stack only:
 * no state of its own, so nothing here needs a lock or a per-context copy. */
#include "wm_draw2.h"

static unsigned wm2_features(void)
{
    return wm2_path_info | wm2_state_info | wm2_text_info |
           wm2_fx_info | wm2_backdrop_info | wm2_carets_info;
}

static long wm2_info(struct wm2_env *e, long a)
{
    switch (a) {
    case 0: return ((long)LOGIT_GUI2_VERSION << 16) | (long)wm2_features();
    case 1: return (long)wm2_state_err_count(e->slot);
    }
    return OL_ARGUMENT;
}

long wm2_dispatch(long num, struct wm2_env *e, long a, long b, long c)
{
    if (!e || e->slot < 0 || e->slot >= WM2_MAX_WINDOWS) return OL_STATE;
    switch (num) {
    case SYS_GUI2_INFO:          return wm2_info(e, a);
    case SYS_GUI_PATH:           return wm2_path(e, a, b, c);
    case SYS_GUI_CLIP_PUSH:      return wm2_clip_push(e, a, b, c);
    case SYS_GUI_CLIP_POP:       return wm2_clip_pop(e, a, b, c);
    case SYS_GUI_GROUP_BEGIN:    return wm2_group_begin(e, a, b, c);
    case SYS_GUI_GROUP_END:      return wm2_group_end(e, a, b, c);
    case SYS_GUI_BLIT2:          return wm2_blit2(e, a, b, c);
    case SYS_GUI_TEXT_RUN2:      return wm2_text_run2(e, a, b, c);
    case SYS_GUI_SHADOW2:        return wm2_shadow2(e, a, b, c);
    case SYS_GUI_GLASS2:         return wm2_glass2(e, a, b, c);
    case SYS_GUI_BACKDROP:       return wm2_backdrop(e, a, b, c);
    case SYS_GUI_BACKDROP_CLEAR: return wm2_backdrop_clear(e, a, b, c);
    }
    return OL_ARGUMENT;
}

void wm2_win_reset(int slot)
{
    if (slot < 0 || slot >= WM2_MAX_WINDOWS) return;
    wm2_state_win_reset(slot);
    wm2_backdrop_win_reset(slot);
}

void wm2_win_flush(struct wm2_env *e)
{
    if (!e || e->slot < 0 || e->slot >= WM2_MAX_WINDOWS) return;
    wm2_state_win_flush(e);
}
