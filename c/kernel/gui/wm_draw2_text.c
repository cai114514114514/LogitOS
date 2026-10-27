/* SYS_GUI_TEXT_RUN2 (GUI ABI v2): a text run with a global alpha and a CSS
 * weight, drawn to the display bound in the env through text_draw_run_a.
 *
 * UNITS: THE 24.8 CONTRACT logit_abi.h STATES FOR struct logit_run2. x, y (the
 * run's top-left) and px are all 24.8 logical points: 12 pt is px = 12 * 256.
 * That is what every producer sends -- aui's tx_run2 (aui_text.inc) passes
 * px * 256 and 24.8 placement, and the aui host harness (tests/unit/aui_host/
 * v2_text.c) answers the call at 24.8. This handler used to read all three as
 * INTEGER points and refuse px > 512, so on a guest every aui run2 text
 * (segmented labels, badges, dialog titles, list/table/select cells,
 * Spotlight's kind column, wrapped and alpha text) came back OL_ARGUMENT and
 * drew nothing, while both host gates stayed green. tests/ui-run2-units.mk
 * links this file against a recording text_draw_run_a (host) and counts the
 * ink on Gallery's and Monitor's labels (guest); -DUI_RUN2_NEGCTL restores the
 * old integer reading as their negative control.
 *
 * BOUNDS. 1 <= px <= 512 POINTS, i.e. 256 <= px24 <= 512 * 256, the same range
 * the legacy SYS_GUI_TEXT_RUN case accepts (px bounds the rasterizer's w*h);
 * anything else is OL_ARGUMENT. len and the copy-in are unchanged.
 *
 * CONVERSION to device pixels: floor(v24 * scale_pct / 25600) for x, y and px
 * (px at least 1). That is wm2_pt / fb_pt's floor carried down to 24.8, so
 *   - a whole-point run lands exactly where SYS_GUI_TEXT_RUN puts it
 *     (tests/unit/text_alpha_test.c "run2-places-like-legacy-*");
 *   - the glyph size is the one SYS_TEXT_MEASURE's metrics and carets queries
 *     measured (text_measure_dispatch.inc scales px by q.px * scale / 100,
 *     floored), so aui's ellipsis and wrap cuts fit what is drawn;
 *   - aui's own conversions already assume it: aui_text_in rounds its y24 UP
 *     so that the floor lands on the device row it computed.
 * A negative coordinate floors toward -infinity (wm2_scale.h explains why
 * truncation would shear at x = 0).
 *
 * THE SUB-PIXEL PEN IS NOT REACHED YET. text.c keeps a quarter-pixel pen
 * internally (layout_locked shapes at x * 256 and emit_blit splits the 24.8 pen
 * into a pixel and a phase), but its only entry point, text_draw_run_a, takes
 * an INTEGER device x. The fraction of x24 * scale below one device pixel is
 * therefore dropped here. Reaching it needs a 24.8-placement entry in text.c
 * (text.c is not part of this change). */
#include "wm_draw2.h"
#include "wm2_scale.h"
#include "text.h"

/* text.c: weight 100..900 -> face mask (nearest baked slot 400/500/600/700,
 * ties up). Declared here because text.h is not part of this change. */
unsigned text_face_for_weight(unsigned face, int weight);

#ifndef USER_TEXT_MAX
#define USER_TEXT_MAX 1024      /* same cap as the legacy SYS_GUI_TEXT_RUN case (wm.c) */
#endif

#define RUN2_PX24_MIN 256             /* 1 pt */
#define RUN2_PX24_MAX (512 * 256)     /* 512 pt, the legacy case's bound */

const unsigned wm2_text_info = LOGIT_GUI2_F_TEXT_RUN2;

#ifndef UI_RUN2_NEGCTL
/* 24.8 logical points -> whole device pixels, floored (see the header). */
static inline int run2_dev(int v24, int pct)
{
    return wm2_sat(wm2_floordiv((long long)v24 * pct, 25600));
}
#endif

long wm2_text_run2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_run2 run;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &run, a, sizeof run);
    if (rc != OL_OK) return rc;
#ifdef UI_RUN2_NEGCTL
    /* THE CONTROL: the old integer-point reading. aui's px * 256 is > 512 and
     * every run is refused, which is what the guest showed before the fix. */
    if (run.px < 1 || run.px > 512) return OL_ARGUMENT;
#else
    if (run.px < RUN2_PX24_MIN || run.px > RUN2_PX24_MAX) return OL_ARGUMENT;
#endif
    if (run.len < 0 || run.len > USER_TEXT_MAX - 1) return OL_ARGUMENT;
    if (run.len > 0 && !run.s) return OL_ARGUMENT;
    char tmp[USER_TEXT_MAX];
    if (run.len > 0 && e->copyin(tmp, run.s, (unsigned long)run.len) < 0) return OL_ARGUMENT;
    tmp[run.len] = 0;
    /* Face: only the two defined bits are trusted. weight == 0 keeps the bold
     * bit from face; otherwise the weight resolves to the nearest baked face
     * slot (400/500/600/700, ties up) and replaces the bold bit. */
    unsigned face = (unsigned)run.face & (LOGIT_FACE_MONO | LOGIT_FACE_BOLD);
    if (run.weight) {
        face &= ~(unsigned)LOGIT_FACE_BOLD;
        face = text_face_for_weight(face, run.weight);
    }
    int alpha = run.alpha < 0 ? 0 : run.alpha > 255 ? 255 : run.alpha;
#ifdef UI_RUN2_NEGCTL
    int x = wm2_pt(run.x, e->scale_pct), y = wm2_pt(run.y, e->scale_pct);
    int px = wm2_pt(run.px, e->scale_pct);
#else
    int x = run2_dev(run.x, e->scale_pct), y = run2_dev(run.y, e->scale_pct);
    int px = run2_dev(run.px, e->scale_pct);
    if (px < 1) px = 1;
#endif
    /* Raw app colour: keep only RGB (backdrop marker rule). */
    text_draw_run_a(e->d, x, y, tmp, run.len, px, face, run.color & 0xFFFFFFu, alpha);
    return OL_OK;
}
