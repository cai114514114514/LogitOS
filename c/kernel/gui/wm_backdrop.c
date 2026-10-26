/* Backdrop regions (GUI ABI v2): STUBS until wm-backdrop-regions.
 *
 * The mechanism is decided (wm_draw2.h, "BACKDROP FOREGROUND SEPARATION"): a
 * marker-byte ink mask. Until it is implemented SYS_GUI_BACKDROP validates
 * its struct size and answers OL_UNSUPPORTED, BACKDROP_CLEAR validates its
 * id, no window has a region, and compositing a region draws nothing -- so
 * the compositor's output is exactly what it was. wm2_backdrop_info is 0. */
#include "wm_draw2.h"

const unsigned wm2_backdrop_info = 0;

long wm2_backdrop(struct wm2_env *e, long a, long b, long c)
{
    struct logit_backdrop2 bd;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &bd, a, sizeof bd);
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}

long wm2_backdrop_clear(struct wm2_env *e, long a, long b, long c)
{
    (void)e; (void)b; (void)c;
    if (a < -1 || a >= LOGIT_BACKDROP_MAX) return OL_ARGUMENT;
    return OL_UNSUPPORTED;
}

void wm2_backdrop_win_reset(int slot) { (void)slot; }

int wm_backdrop_regions(int slot, struct wm2_region *out, int max)
{
    (void)slot; (void)out; (void)max;
    return 0;
}

void wm_backdrop_composite(const struct drect *R, int win_slot, int dx, int dy,
                           struct ol_display *d)
{
    (void)R; (void)win_slot; (void)dx; (void)dy; (void)d;
}
