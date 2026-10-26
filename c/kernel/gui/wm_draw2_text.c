/* SYS_GUI_TEXT_RUN2 (GUI ABI v2): STUB until text-kernel-alpha. Validates
 * the struct size, then answers OL_UNSUPPORTED with wm2_text_info clear. */
#include "wm_draw2.h"

const unsigned wm2_text_info = 0;

long wm2_text_run2(struct wm2_env *e, long a, long b, long c)
{
    struct logit_run2 run;
    (void)b; (void)c;
    long rc = wm2_copy_struct(e, &run, a, sizeof run);
    return rc == OL_OK ? OL_UNSUPPORTED : rc;
}
