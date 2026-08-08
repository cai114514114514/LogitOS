#ifndef LOGIT_JS_PLATFORM_H
#define LOGIT_JS_PLATFORM_H

#include "quickjs.h"

/* The parts of the web platform that are neither the DOM tree (js_dom.c) nor
 * the network (js_webapi.c): timing, the document lifecycle, task and message
 * queues, errors, cloning, entropy, the observers.
 *
 * WHY THIS FILE EXISTS, AND WHY ITS CONTENTS ARE NOT A GUESS
 * It was written from a measurement, not from a list of Web APIs. tests/unit/
 * webapi_probe.c loads seven real pages from committed bytes, runs their
 * scripts under a Proxy that records every global and every platform-object
 * property the runtime cannot answer, and ranks the misses by how many pages
 * need each one. The first run of that probe found something worth writing
 * down: the GLOBALS were almost fine. Seventeen names missed across seven
 * pages, and thirteen of them were the pages' own -- `_w`, `RLQ`, `_N_E` --
 * missing only because the script that defines them had already died. What the
 * pages actually die on is one level down: `performance.timing.navigationStart`
 * (bing), `performance.mark` (bing, wikipedia), `document.readyState` (bing,
 * deepseek), `localStorage.<name>` (bing), `navigator.scheduling` (deepseek).
 * Properties of objects that exist and are incomplete.
 *
 * So the order of this file is the order of that table, and every entry in it
 * is something a page in the corpus reached for. Where something is here that
 * the corpus did not reach -- Blob, FormData, crypto, the observers, Intl --
 * it is marked as such in its own comment, so the next person can tell the
 * measured half from the requested half.
 *
 * WHAT IS DELIBERATELY ABSENT, AND MUST STAY ABSENT
 * The probe also records misses that are CORRECT. `window.ActiveXObject`
 * (bing), `document.documentMode` (deepseek) and `window.MSApp` are how a page
 * detects Internet Explorer; `window.indexedDB` and `__REACT_DEVTOOLS_GLOBAL_
 * HOOK__` are feature detection whose false branch is the one we want. Defining
 * any of them would make pages take a path we cannot follow. A missing global
 * is not automatically a bug, and the probe's job is to tell you which kind you
 * are looking at -- not to be emptied.
 *
 * EVERYTHING INSTALLS ONLY IF ABSENT. Three lines are extending this runtime at
 * once (the DOM bindings, the module loader, this one). Whatever is already
 * there wins, so landing after another line's work cannot silently replace it.
 */

#ifdef JS_PLATFORM_OPTIONAL
#  define PLATFORM_FN __attribute__((__weak__))
#else
#  define PLATFORM_FN
#endif

/* Install into `ctx`. Call LAST -- after js_dom_init and js_webapi_install --
 * because it fills gaps in the objects they publish (document, navigator,
 * performance, localStorage) and because "only if absent" is only meaningful
 * once everyone else has had their turn. */
PLATFORM_FN void js_platform_install(JSContext *ctx);

/* Release everything held here before JS_FreeContext: the promise-rejection
 * tracker's context pointer and the observer registry. */
PLATFORM_FN void js_platform_close(JSContext *ctx);

/* The viewport an IntersectionObserver measures against. Same values the
 * embedder gives js_webapi_set_viewport. */
PLATFORM_FN void js_platform_set_viewport(int w, int h);

/* querySelectorAll / getElementsBy* / matches / closest -- js_select.c. A
 * separate entry point (and a separate file) because they are on loan from the
 * DOM bindings: they install only if absent and the whole file is meant to be
 * deleted the day js_dom.c grows its own. Call after js_dom_init. */
PLATFORM_FN void js_select_install(JSContext *ctx);

#endif /* LOGIT_JS_PLATFORM_H */
