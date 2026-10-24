#ifndef LOGIT_BROWSER_ABORT_H
#define LOGIT_BROWSER_ABORT_H

#include "quickjs.h"

/* Installs one realm's constructors and returns private Fetch hooks. The timer
 * callbacks belong to that realm and must be discarded when it closes. */
JSValue js_abort_install(JSContext *ctx, JSValueConst later);

#endif
