#ifndef LOGIT_STREAM_CORE_H
#define LOGIT_STREAM_CORE_H
#include "quickjs.h"
/* Returns private consumer hooks; the page only receives the public classes. */
JSValue js_streams_install(JSContext *ctx, JSValueConst abort);
#endif

