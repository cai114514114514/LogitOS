#ifndef LOGIT_JS_URL_H
#define LOGIT_JS_URL_H

/* The WHATWG URL Standard -- the parser, the serializer, the setters, host
 * parsing, and URLSearchParams -- plus the `URL` / `URLSearchParams` globals.
 *
 * WHY THIS IS NOT c/net/http/url.c, AND WHY IT DOES NOT CALL IT.
 * That file is 105 lines and it is *correct for what it does*: it turns
 * "http://host[:port]/path" into a host, a port and a path so the fetch has
 * somewhere to connect. It requires an explicit http/https scheme, keeps the
 * query and fragment inside `path`, has no userinfo, no non-special scheme, no
 * file: scheme, no IPv6 literal, no percent-encoding, no dot-segment removal
 * and a 128-byte host. Every one of those is a component the URL Standard
 * defines and the WPT corpus asks about 891 times. There is no adapter from
 * "the four fields the socket needs" to "the eleven the standard defines" --
 * the missing information was never parsed. So this is a second parser, and
 * the honest cost of that is written down: the browser now has TWO opinions
 * about what a URL means, one used by fetch and one used by script. Closing
 * that gap means the net stack adopting THIS parser, which is a change to how
 * every page loads and is not this file's to make.
 *
 * WHY IT IS ONE FILE. c/apps/browser/js_*.c is a wildcard in both the
 * Makefile's BROWSER_JS_SRC and tests/wpt.mk's WPT_JS_SRC, on purpose -- so
 * that a new binding is linked by the browser and measured by the corpus with
 * no edit to either list. A second TU holding the parser would be picked up by
 * neither. Hence: the algorithm and its bindings live together, and
 * -DURL_CORE_ONLY compiles the algorithm alone, with no QuickJS at all, which
 * is what the host test against urltestdata.json links (in seconds).
 *
 * WHAT IS NOT FULLY HERE: IDNA. See the IDNA section in the .c -- UTS-46
 * mapping is a hand-built table of the classes that occur in practice, and
 * NFC normalization, CheckBidi and CheckJoiners are absent. Punycode itself
 * (RFC 3492, both directions) is complete. */

#include <stddef.h>

/* ---- the URL record ---------------------------------------------------- */

typedef struct urlrec urlrec;

/* Components, for url_get / url_set. The order matches the IDL. */
enum {
    URLC_HREF = 0, URLC_PROTOCOL, URLC_USERNAME, URLC_PASSWORD,
    URLC_HOST, URLC_HOSTNAME, URLC_PORT, URLC_PATHNAME,
    URLC_SEARCH, URLC_HASH, URLC_ORIGIN, URLC__N
};

/* Parse `input` (UTF-8, `len` bytes; len < 0 means NUL-terminated) against the
 * optional `base`. Returns NULL on failure -- which is what the constructor
 * turns into a TypeError. */
urlrec *url_parse_w(const char *input, int len, const urlrec *base);

void    url_free_w(urlrec *u);
urlrec *url_clone_w(const urlrec *u);

/* Serialize a component. Returns a malloc'd NUL-terminated string; the caller
 * frees. Never NULL for a live record (a null component serializes to ""). */
char   *url_get(const urlrec *u, int comp);

/* Run a component setter. Returns 0 if the record was left in a valid state
 * (which includes "the setter did nothing", the standard's behaviour for every
 * setter but href), -1 only for href with an unparseable value. */
int     url_set(urlrec *u, int comp, const char *value, int len);

/* Does the record's query differ from `q`? Used by the URL <-> searchParams
 * link. `q` is the serialized parameter list, or NULL for none. */
void    url_set_query_raw(urlrec *u, const char *q);
const char *url_query_raw(const urlrec *u);   /* NULL when the query is null */

/* ---- application/x-www-form-urlencoded --------------------------------- */

typedef struct uspair { char *name, *value; } uspair;
typedef struct usplist {
    uspair *v;
    int n, cap;
} usplist;

void  usp_init(usplist *l);
void  usp_clear(usplist *l);
void  usp_append(usplist *l, const char *name, const char *value);
void  usp_parse(usplist *l, const char *s, int len);   /* replaces the list */
char *usp_serialize(const usplist *l);                 /* malloc'd */
void  usp_sort(usplist *l);                            /* stable, UTF-16 order */

/* ---- percent-encoding, exposed because the setters and the tests want it - */
enum { PCT_C0 = 0, PCT_FRAGMENT, PCT_QUERY, PCT_SPECIAL_QUERY,
       PCT_PATH, PCT_USERINFO, PCT_COMPONENT, PCT_FORM };
char *url_percent_encode(const char *s, int len, int set);
char *url_percent_decode(const char *s, int len, int *outlen);

/* ---- host parsing, exposed for the host test ---------------------------- */
/* Returns the serialized host (malloc'd) or NULL on failure. `opaque` selects
 * the non-special-scheme rules. */
char *url_host_parse(const char *s, int len, int opaque);

/* ---- the JS globals ----------------------------------------------------- */
#ifndef URL_CORE_ONLY
#include "quickjs.h"
/* Weak under JS_URL_OPTIONAL, the convention js_webapi.h / js_events.h use:
 * js_page.c's own host tests link without this TU and must still link. */
#ifdef JS_URL_OPTIONAL
#  define URL_FN __attribute__((__weak__))
#else
#  define URL_FN
#endif
/* Install LAST, after js_webapi_install. js_webapi.c ships a JS-prelude URL
 * built on c/net/http/url.c; this replaces it outright, and replacing it means
 * running after it. */
URL_FN void js_url_install(JSContext *ctx);
#endif

#endif /* LOGIT_JS_URL_H */
