/* SYS_GUI_SHADOW2 and SYS_GUI_GLASS2 (GUI ABI v2): STUBS until
 * wm-draw2-effects. Each validates its struct size, then answers
 * OL_UNSUPPORTED with wm2_fx_info clear. */
#include "wm_draw2.h"

const unsigned wm2_fx_info = 0;

long wm2_shadow2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_shadow2 shadow;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &shadow, a, sizeof shadow);
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}

long wm2_glass2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_glass2 glass;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &glass, a, sizeof glass);
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}
