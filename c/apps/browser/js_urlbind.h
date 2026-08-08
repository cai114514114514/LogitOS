#ifndef LOGIT_JS_URLBIND_H
#define LOGIT_JS_URLBIND_H

/* The URL Standard, bound to the DOM: the document base URL, the
 * HTMLHyperlinkElementUtils members on <a> and <area>, and the three APIs
 * whose first argument is a URL and which must REFUSE one they cannot parse.
 *
 * WHY THIS IS NOT js_url.c. That file is the algorithm -- the basic URL
 * parser, host parsing, the setters, URLSearchParams -- and it compiles with
 * -DURL_CORE_ONLY against no QuickJS and no DOM at all, which is what makes
 * `make test-url` a two-second host test over urltestdata.json. Everything
 * here needs a document: a <base> element to find, an href content attribute
 * to read and write, `location` to fall back to. Putting it in js_url.c would
 * drag the DOM into that link.
 *
 * WHY IT IS NOT js_reflect.c EITHER, which already had a version of the
 * hyperlink members. That one is a VIEW built out of the page's own `URL`
 * constructor: every getter constructs a URL object, reads one property off
 * it and throws it away. It was written before there was a URL parser worth
 * calling, its purpose there was to give WPT's reflection.js an oracle for the
 * url-typed attributes, and it gets two things wrong that 431 subtests in
 * url/a-element.html turn on:
 *
 *   - a null url must report `protocol` as ":" and NOT as "". That is the
 *     ONLY signal the standard gives a page that an <a>'s href did not parse
 *     -- `href` deliberately echoes the literal back -- and it is exactly what
 *     url/a-element.js tests: `if (url.protocol !== ':') assert_unreached()`.
 *     266 subtests, every failure case in the corpus, on one colon.
 *   - the base. A reflected URL resolves against the DOCUMENT BASE URL, which
 *     is the <base> element's if the document has one. Nothing in this tree
 *     computed that, `document.baseURI` did not exist, and the fallback was
 *     `location.href` -- so every one of the 165 remaining subtests, which
 *     drive the base through `document.getElementById("base").href = ...`,
 *     resolved against the runner's own address instead.
 *
 * So this file owns the document base URL (below), and js_reflect.c's
 * `resolve_url` picks it up for free through `document.baseURI` -- one
 * definition, reached by both.
 *
 * INSTALL LAST, after js_url_install, and js_url.c calls it there rather than
 * js_page.c adding a line: the accessors here REPLACE js_reflect.c's on
 * HTMLAnchorElement.prototype / HTMLAreaElement.prototype, and a replacement
 * has to run after the thing it replaces. Weak, like every other installer in
 * this directory, so the six .mk fragments that hand-list the browser's
 * sources keep linking without it and simply keep the older behaviour. */

#ifndef URL_CORE_ONLY
#include "quickjs.h"

#ifdef JS_URLBIND_OPTIONAL
#  define URLBIND_FN __attribute__((__weak__))
#else
#  define URLBIND_FN
#endif

URLBIND_FN void js_urlbind_install(JSContext *ctx);

/* The document base URL, serialized, or NULL when there is none that parses
 * (an `about:blank` document with no <base>). Caller frees. Exposed because
 * it is the one answer several files need and none of them should compute
 * twice. */
URLBIND_FN char *js_urlbind_base_href(JSContext *ctx);
#endif

#endif /* LOGIT_JS_URLBIND_H */
