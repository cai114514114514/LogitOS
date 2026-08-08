#ifndef LOGIT_JS_REFLECT_H
#define LOGIT_JS_REFLECT_H

#include "quickjs.h"

/* IDL attribute reflection -- `el.title` is the `title` content attribute, in
 * both directions, through a typed coercion.
 *
 * See the file comment in js_reflect.c for what that means and why it is one
 * mechanism rather than several hundred properties.
 *
 * js_dom.c calls this once per page, after the interface prototypes exist and
 * before any page script can run. `proto_for(ud, tag)` hands back the prototype
 * object for an element name ("input" -> HTMLInputElement.prototype), or
 * JS_UNDEFINED for a tag this build has no interface for; `html_proto` is
 * HTMLElement.prototype, where the attributes every HTML element reflects
 * (title, lang, dir, hidden, ...) are installed.
 *
 * The reference is WEAK in js_dom.c: a link that leaves this file out still
 * builds and simply has no reflected attributes. Several test fragments list
 * the browser's sources by hand and belong to other lines. */
typedef JSValueConst (*js_reflect_proto_fn)(void *ud, const char *tag);

void js_reflect_install(JSContext *ctx, JSValueConst html_proto,
                        js_reflect_proto_fn proto_for, void *ud);

/* How many accessor pairs the last install defined. For the self-test and for
 * anyone asking whether the table reached the prototypes at all. */
int js_reflect_installed(void);

#endif /* LOGIT_JS_REFLECT_H */
