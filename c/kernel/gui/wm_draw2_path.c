/* SYS_GUI_PATH (GUI ABI v2): STUB until wm-draw2-path. It validates the
 * struct sizes -- so the OL_ARGUMENT contract holds from day one -- and then
 * answers OL_UNSUPPORTED with wm2_path_info clear. No state, no pixels. */
#include "wm_draw2.h"

const unsigned wm2_path_info = 0;

long wm2_path(struct wm2_env *e, long a, long b, long c)
{
    struct logit_path2 path;
    struct logit_paint2 paint;
    long rc = wm2_copy_struct(e, &path, a, sizeof path);
    if (rc == OL_OK) rc = wm2_copy_struct(e, &paint, b, sizeof paint);
    if (rc == OL_OK && c) {
        struct logit_stroke2 stroke;
        rc = wm2_copy_struct(e, &stroke, c, sizeof stroke);
    }
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}
