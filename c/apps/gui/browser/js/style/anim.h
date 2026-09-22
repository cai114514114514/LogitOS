#ifndef LOGIT_JS_ANIM_H
#define LOGIT_JS_ANIM_H
#include "quickjs.h"
void js_anim_install(JSContext *ctx);
/* Release callback and target references before js_dom_cleanup/FreeContext. */
void js_anim_close(JSContext *ctx);
#endif
