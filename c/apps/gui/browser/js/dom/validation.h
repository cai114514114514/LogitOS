#ifndef LOGIT_JS_FORM_VALIDATION_H
#define LOGIT_JS_FORM_VALIDATION_H

#include "quickjs.h"

/* Called by js_forms_install/cleanup, so these native callbacks never retain
 * a page context after its ordinary forms bindings have been torn down. */
void js_validation_install(JSContext *ctx);
void js_validation_cleanup(void);

#endif
