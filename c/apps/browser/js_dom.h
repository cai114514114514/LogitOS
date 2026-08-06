#ifndef LOGIT_JS_DOM_H
#define LOGIT_JS_DOM_H

#include "quickjs.h"

struct node;

/* Install `document` + the Element class into ctx, bound to the live page DOM.
 * Call once per JS context before JS_Eval. */
void js_dom_init(JSContext *ctx, struct node *root);

/* 1 if JS mutated the DOM since the last clear (caller should re-layout). */
int  js_dom_dirty(void);
void js_dom_clear_dirty(void);

/* Number of addEventListener handlers recorded (no dispatch yet). */
int  js_dom_listener_count(void);

/* Release listener handler refs; call before JS_FreeContext/JS_FreeRuntime. */
void js_dom_cleanup(JSContext *ctx);

#endif /* LOGIT_JS_DOM_H */
