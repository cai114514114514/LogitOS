/* SPDX-License-Identifier: MIT */
#ifndef LOGIT_JS_CLONE_H
#define LOGIT_JS_CLONE_H

#include "quickjs.h"

/* Install after the realm's intrinsic constructors and DOMException exist,
 * before any author script. Native brands and methods are captured once. */
void js_clone_install(JSContext *ctx);

/* Optional private platform hooks, captured during installation. `hooks` is
 * an array of [brand, clone?] records: brand(value) must consult native/private
 * instance state, never constructor/prototype names. A branded value without
 * a clone function is refused. clone(value, subclone, remember) must call
 * remember(result) before subclone when its serialization can contain cycles.
 *
 * This bridge is needed for JS-backed WebIDL objects sharing Object's native
 * class. A native class is rejected automatically, but a WeakMap-backed Blob,
 * URL, or FormData is indistinguishable from an ordinary object without its
 * owner's private brand. Passing these hooks through a public author-writable
 * global would silently let a page redefine what the browser considers a
 * platform object; callers must pass closures directly at realm bootstrap. */
/* Returns zero with a pending exception if bootstrap cannot be installed. */
int js_clone_install_host(JSContext *ctx, JSValueConst hooks);

#endif
