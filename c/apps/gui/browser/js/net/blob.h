#ifndef LOGIT_JS_BLOB_H
#define LOGIT_JS_BLOB_H
#include "quickjs.h"
/* Private factory(partBytes, decode, stream) -> immutable Blob/File hooks. */
JSValue js_blob_factory(JSContext *ctx);
#endif
