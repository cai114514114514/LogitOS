#ifndef LOGIT_STREAM_TRANSFORM_H
#define LOGIT_STREAM_TRANSFORM_H
#include "quickjs.h"
JSValue js_transform_install(JSContext *ctx, JSValueConst shared, JSValueConst readable, JSValueConst writable);
#endif

