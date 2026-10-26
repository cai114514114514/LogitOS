/* SYS_GUI_TEXT_RUN2 (GUI ABI v2): a text run with a global alpha and a CSS
 * weight. Coordinates are logical points in the legacy SYS_GUI_TEXT_RUN sense
 * (integer points, scaled by wm2_pt exactly as fb_pt would); the draw goes to
 * the display bound in the env through text_draw_run_a. */
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "text.h"

#ifndef USER_TEXT_MAX
#define USER_TEXT_MAX 1024      /* same cap as the legacy SYS_GUI_TEXT_RUN case (wm.c) */
#endif

const unsigned wm2_text_info = LOGIT_GUI2_F_TEXT_RUN2;

long wm2_text_run2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_run2 run;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &run, a, sizeof run);
    if (rc != OL_OK) return rc;
    if (run.px < 1 || run.px > 512) return OL_ARGUMENT;   /* as the legacy case: px bounds the rasterizer's w*h */
    if (run.len < 0 || run.len > USER_TEXT_MAX - 1) return OL_ARGUMENT;
    if (run.len > 0 && !run.s) return OL_ARGUMENT;
    char tmp[USER_TEXT_MAX];
    if (run.len > 0 && e->copyin(tmp, run.s, (unsigned long)run.len) < 0) return OL_ARGUMENT;
    tmp[run.len] = 0;
    /* Face: only the two defined bits are trusted. weight == 0 keeps the bold
     * bit from face; otherwise >= 600 is bold and anything else regular (until
     * font-weights gives the engine more than two instances). */
    unsigned face = (unsigned)run.face & (LOGIT_FACE_MONO | LOGIT_FACE_BOLD);
    if (run.weight) {
        face &= ~(unsigned)LOGIT_FACE_BOLD;
        if (run.weight >= 600) face |= LOGIT_FACE_BOLD;
    }
    int alpha = run.alpha < 0 ? 0 : run.alpha > 255 ? 255 : run.alpha;
    /* Raw app colour: keep only RGB (backdrop marker rule). */
    text_draw_run_a(e->d, wm2_pt(run.x, e->scale_pct), wm2_pt(run.y, e->scale_pct),
                    tmp, run.len, wm2_pt(run.px, e->scale_pct), face,
                    run.color & 0xFFFFFFu, alpha);
    return OL_OK;
}
