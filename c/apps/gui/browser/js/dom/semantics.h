#ifndef LOGIT_JS_SEMANTICS_H
#define LOGIT_JS_SEMANTICS_H
#include "quickjs.h"
struct node;
void js_semantics_install(JSContext *ctx);
/* Run ONLY the invoker default after a non-canceled native click. Returns 1
 * if command/popover handling owns this activation; never dispatches click
 * again or toggles native checkbox/form state. Mouse and keyboard share it. */
int js_semantics_activate_invoker(struct node *node);
/* Internal lookup of an already materialized .content fragment. Undefined
 * before install/first content read; never consults an overridden property. */
JSValue js_semantics_template_content(JSContext *ctx, JSValueConst node);
/* Before freeing the page context: releases the private JS callback. */
void js_semantics_close(JSContext *ctx);
#endif
