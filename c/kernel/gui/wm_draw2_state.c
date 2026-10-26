/* Clip and group stacks, BLIT2 (GUI ABI v2): STUBS until wm-draw2-state.
 * Each validates its struct size, then answers OL_UNSUPPORTED with
 * wm2_state_info clear. The per-window hooks are no-ops and the error
 * counter is 0: with no levels ever opened, none is ever auto-closed. */
#include "wm_draw2.h"

const unsigned wm2_state_info = 0;

long wm2_clip_push(struct wm2_env *e, long a, long b, long c)
{
    struct logit_clip2 clip;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &clip, a, sizeof clip);
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}

long wm2_clip_pop(struct wm2_env *e, long a, long b, long c)
{
    (void)e; (void)a; (void)b; (void)c;
    return OL_UNSUPPORTED;
}

long wm2_group_begin(struct wm2_env *e, long a, long b, long c)
{
    struct logit_group2 group;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &group, a, sizeof group);
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}

long wm2_group_end(struct wm2_env *e, long a, long b, long c)
{
    (void)e; (void)a; (void)b; (void)c;
    return OL_UNSUPPORTED;
}

long wm2_blit2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_blit2 blit;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &blit, a, sizeof blit);
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}

void wm2_state_win_reset(int slot) { (void)slot; }
void wm2_state_win_flush(struct wm2_env *e) { (void)e; }
unsigned wm2_state_err_count(int slot) { (void)slot; return 0; }
