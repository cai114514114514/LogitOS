/* js_urlbind.c -- the URL Standard bound to the DOM.
 *
 * Read js_urlbind.h first: it says what this is and, more usefully, what it is
 * NOT (a second URL parser -- there are already two opinions about URLs in this
 * browser and a third would be worse). Everything below calls js_url.c.
 *
 * Three things live here, and they are one thing seen from three sides: they
 * all need the DOCUMENT BASE URL, and before this file nothing computed it.
 *
 *   1. `document.baseURI` -- really Node.prototype.baseURI, because every node
 *      reports its node document's base URL and there is one document here.
 *   2. HTMLHyperlinkElementUtils: the eleven members on <a> and <area>.
 *   3. The APIs that TAKE a URL and must throw when it will not parse:
 *      XMLHttpRequest.open (SyntaxError), navigator.sendBeacon (TypeError),
 *      window.open (SyntaxError).
 *
 * WHAT THE THIRD GROUP IS ABOUT, since "add a throw" reads like paperwork.
 * url/failure.html asks one question 1,163 times: if a string is not a URL,
 * does EVERY entry point agree that it is not a URL? A browser that parses
 * strictly in `new URL()` and leniently in `xhr.open()` has not got a URL
 * parser, it has got two and one of them is a guess. 752 of those subtests are
 * the four non-constructor entry points; three of them are reachable from here.
 * (The fourth, `frame.contentWindow.location`, needs a nested browsing context
 * -- iframes -- which this file does not build and which is the single largest
 * remaining cause in dom/ as well.)
 *
 * ON REPLACING js_reflect.c's ACCESSORS. Its define_hlink() never clobbers an
 * own property, so the two files cannot both win by ordering alone; this one
 * defines over the top deliberately, and the header says why. The important
 * part is that href and the ten components now come out of ONE parse of ONE
 * string, so "href and the components agree" is structural instead of a
 * coincidence that holds until someone edits one of the two paths.
 */

#include <stdlib.h>
#include <string.h>

#include "js_urlbind.h"
#include "js_url.h"
#include "js_dom.h"
#include "dom.h"

#ifndef URL_CORE_ONLY

/* ======================================================================
 * The document base URL
 * ====================================================================== */

/* HTML: "the document base URL of a Document is, if there is no base element
 * with an href attribute, the document's fallback base URL; otherwise the
 * frozen base URL of the FIRST such element."  The fallback base URL here is
 * the document's own address, which is `location.href`.
 *
 * Note what is NOT cached: a page may insert or rewrite a <base> at any moment
 * (url/a-element.js does exactly that, once per subtest), so a cache would need
 * an invalidation hook in dom.c -- a change to a file this line does not own,
 * to save two parses of a short string. Measured: the whole a-element corpus,
 * 892 subtests x ~11 getters, is under a second with no cache at all. */

static char *loc_href(JSContext *ctx)
{
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue loc = JS_GetPropertyStr(ctx, g, "location");
    JSValue href = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "href") : JS_UNDEFINED;
    char *out = 0;
    if (JS_IsString(href)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, href);
        if (s) {
            out = (char *)malloc(n + 1);
            if (out) { memcpy(out, s, n); out[n] = 0; }
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, href);
    JS_FreeValue(ctx, loc);
    JS_FreeValue(ctx, g);
    return out;
}

/* First <base href> in tree order, or NULL. An href attribute that is present
 * and empty still counts -- the spec's test is on the ATTRIBUTE, not on its
 * value, and "" resolved against the fallback is a legitimate base. */
static struct node *find_base(struct node *n)
{
    for (; n; n = n->next) {
        if (n->type == N_ELEM && n->tag && !strcmp(n->tag, "base")) {
            int len = 0;
            if (js_dom_attr_len(n, "href", &len)) return n;
        }
        if (n->first_child) {
            struct node *r = find_base(n->first_child);
            if (r) return r;
        }
    }
    return 0;
}

/* The document base URL as a record, or NULL when neither the <base> nor the
 * document's own address parses (an about:blank document under a runner that
 * hands us something opaque). Callers must treat NULL as "no base", which is
 * what makes every relative URL on such a page fail to parse -- correctly. */
static urlrec *doc_base(JSContext *ctx)
{
    char *lh = loc_href(ctx);
    urlrec *fallback = lh ? url_parse_w(lh, -1, 0) : 0;
    free(lh);

    struct node *b = find_base(js_dom_root());
    if (b) {
        int len = 0;
        const char *v = js_dom_attr_len(b, "href", &len);
        if (v) {
            /* HTML "set the frozen base URL": parse against the fallback, and
             * on failure the frozen base URL IS the fallback. */
            urlrec *frozen = url_parse_w(v, len, fallback);
            if (frozen) { url_free_w(fallback); return frozen; }
        }
    }
    return fallback;
}

char *js_urlbind_base_href(JSContext *ctx)
{
    urlrec *u = doc_base(ctx);
    if (!u) return 0;
    char *s = url_get(u, URLC_HREF);
    url_free_w(u);
    return s;
}

static JSValue node_baseuri_get(JSContext *ctx, JSValueConst this_val)
{
    (void)this_val;
    char *s = js_urlbind_base_href(ctx);
    /* No parseable base: report the document's own address verbatim, which is
     * what a browser shows for `about:blank`. Never undefined -- js_reflect.c
     * and js_platform.c both branch on `document.baseURI || location.href`,
     * and an undefined here is how that branch silently kept the old, wrong
     * answer for a year. */
    if (!s) {
        char *lh = loc_href(ctx);
        JSValue v = JS_NewString(ctx, lh ? lh : "about:blank");
        free(lh);
        return v;
    }
    JSValue v = JS_NewString(ctx, s);
    free(s);
    return v;
}

/* ======================================================================
 * HTMLHyperlinkElementUtils
 * ====================================================================== */

/* "Reinitialize url": parse the href content attribute against the document
 * base URL. *have_attr says whether the attribute exists at all, which the
 * href getter needs and no other member does -- the standard distinguishes
 * "no href" (href is "") from "href that does not parse" (href echoes the
 * literal back). */
static urlrec *hl_url(JSContext *ctx, JSValueConst this_val,
                      struct node **out_n, const char **out_v, int *out_vlen)
{
    struct node *n = js_dom_node_from(this_val);
    if (out_n) *out_n = n;
    if (out_v) { *out_v = 0; *out_vlen = 0; }
    if (!n || n->type != N_ELEM) return 0;

    int len = 0;
    const char *v = js_dom_attr_len(n, "href", &len);
    if (out_v) { *out_v = v; *out_vlen = len; }
    if (!v) return 0;

    urlrec *base = doc_base(ctx);
    urlrec *u = url_parse_w(v, len, base);
    url_free_w(base);
    return u;
}

static JSValue hl_get_spec(JSContext *ctx, JSValueConst this_val, int magic)
{
    const char *v = 0;
    int vlen = 0;
    urlrec *u = hl_url(ctx, this_val, 0, &v, &vlen);

    if (!u) {
        /* Every getter answers "" for a null url, with exactly two exceptions,
         * and both of them are load-bearing:
         *   protocol -> ":"   the only way a page can tell that the href did
         *                     not parse (a-element.js tests precisely this)
         *   href     -> the literal attribute value, or "" when absent. */
        if (magic == URLC_PROTOCOL) return JS_NewString(ctx, ":");
        if (magic == URLC_HREF)
            return JS_NewStringLen(ctx, v ? v : "", (size_t)(v && vlen > 0 ? vlen : 0));
        return JS_NewString(ctx, "");
    }
    char *s = url_get(u, magic);
    url_free_w(u);
    JSValue out = JS_NewString(ctx, s ? s : "");
    free(s);
    return out;
}

static JSValue hl_set_spec(JSContext *ctx, JSValueConst this_val, JSValueConst val,
                           int magic)
{
    struct node *n = 0;
    const char *v = 0;
    int vlen = 0;
    urlrec *u = hl_url(ctx, this_val, &n, &v, &vlen);
    if (!n || n->type != N_ELEM) { url_free_w(u); return JS_UNDEFINED; }
    if (magic == URLC_ORIGIN) { url_free_w(u); return JS_UNDEFINED; }  /* read-only */

    /* LENGTH, not strlen. U+0000 is a legal attribute value and a legal
     * component value; dom.c stores it and JS_ToCString would cut the string
     * at it in both directions. This is the same class of bug the reflection
     * line found across ~5,000 subtests. */
    size_t slen = 0;
    const char *s = JS_ToCStringLen(ctx, &slen, val);
    if (!s) { url_free_w(u); return JS_EXCEPTION; }

    if (magic == URLC_HREF) {
        /* href is a plain reflected content attribute: the setter STORES the
         * string, unparsed. Whether it parses is the getter's problem. */
        js_dom_attr_write(ctx, n, "href", s, (int)slen);
        JS_FreeCString(ctx, s);
        url_free_w(u);
        return JS_UNDEFINED;
    }

    /* Every other setter is a no-op when the url is null -- there is nothing
     * to modify, and the standard says so rather than inventing a base. */
    if (u) {
        url_set(u, magic, s, (int)slen);
        char *href = url_get(u, URLC_HREF);
        if (href) { js_dom_attr_write(ctx, n, "href", href, (int)strlen(href)); free(href); }
    }
    JS_FreeCString(ctx, s);
    url_free_w(u);
    return JS_UNDEFINED;
}

/* ======================================================================
 * THE NEGATIVE CONTROL -- -DURLELEM_SPLITTER
 * ======================================================================
 *
 * An eleven-property surface that answers correctly for every URL a human
 * types is not evidence of a URL parser, and the only way to show that the
 * test below is measuring one is to build the thing that ISN'T and watch the
 * test go red. So: split the href on ':', '/', '?' and '#', and put it back
 * together by concatenation. That is the implementation almost everyone
 * writes first, and it is not a straw man -- the URL line built the same
 * control for `URL` itself and it scored 251/891 on urltestdata.json, which
 * is to say it is right about three quarters of the way through the corpus
 * and wrong about everything that makes a URL parser hard:
 *
 *   normalization      "http://EXAMPLE.com/a/../b" stays exactly as typed
 *   default ports      ":80" is not dropped, so host and origin are wrong
 *   dot segments       "/a/./b/../c" is a path with five segments
 *   encoding sets      a space in the query is a space in the query
 *   backslashes        "http:\\\\host" is not a special-scheme authority
 *   failure            there is no such thing; everything "parses"
 *
 * That last one is the important one, and it is why the control also has to
 * exist for the ELEMENT surface and not only for `URL`: a splitter can never
 * report `protocol === ":"`, so every failure case in the corpus goes from
 * passing to failing, which is exactly the 266 subtests this file was written
 * for. `make test-urlelem-negctl` requires this build to FAIL.
 *
 * It is compiled only under the flag: dead code that ships is a second
 * implementation waiting to be called by mistake. */
#ifdef URLELEM_SPLITTER

/* The base, as a string, cut back to its last '/' -- naive relative
 * resolution by concatenation, which is the whole point. */
static char *split_base_dir(JSContext *ctx)
{
    char *b = js_urlbind_base_href(ctx);
    if (!b) return 0;
    char *slash = strrchr(b, '/');
    if (slash) slash[1] = 0;
    return b;
}

/* Concatenate the href onto the base unless it already has a "scheme:". */
static char *split_absolutize(JSContext *ctx, const char *v, int vlen)
{
    int has_scheme = 0;
    for (int i = 0; i < vlen; i++) {
        char c = v[i];
        if (c == ':') { has_scheme = i > 0; break; }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) break;
    }
    if (has_scheme) {
        char *out = (char *)malloc((size_t)vlen + 1);
        if (out) { memcpy(out, v, (size_t)vlen); out[vlen] = 0; }
        return out;
    }
    char *base = split_base_dir(ctx);
    int bl = base ? (int)strlen(base) : 0;
    char *out = (char *)malloc((size_t)(bl + vlen + 1));
    if (out) {
        if (bl) memcpy(out, base, (size_t)bl);
        memcpy(out + bl, v, (size_t)vlen);
        out[bl + vlen] = 0;
    }
    free(base);
    return out;
}

/* Cut `s` into the pieces the four delimiters suggest. Every field points into
 * `s`; nothing is decoded, normalized, lowercased or defaulted. */
struct sparts {
    const char *scheme; int slen;      /* without the ':' */
    const char *user;   int ulen;
    const char *pass;   int plen;
    const char *host;   int hlen;      /* without the port */
    const char *port;   int portlen;
    const char *path;   int pathlen;
    const char *query;  int qlen;      /* without the '?' */
    const char *frag;   int flen;      /* without the '#' */
};

static void split_parse(const char *s, struct sparts *p)
{
    memset(p, 0, sizeof *p);
    int n = (int)strlen(s);
    int i = 0;
    while (i < n && s[i] != ':' && s[i] != '/' && s[i] != '?' && s[i] != '#') i++;
    if (i < n && s[i] == ':') { p->scheme = s; p->slen = i; i++; }
    if (i + 1 < n && s[i] == '/' && s[i + 1] == '/') {
        i += 2;
        int a = i;
        while (i < n && s[i] != '/' && s[i] != '?' && s[i] != '#') i++;
        int aend = i, at = -1;
        for (int j = a; j < aend; j++) if (s[j] == '@') at = j;
        int hstart = a;
        if (at >= 0) {
            int colon = -1;
            for (int j = a; j < at; j++) if (s[j] == ':') { colon = j; break; }
            p->user = s + a; p->ulen = (colon >= 0 ? colon : at) - a;
            if (colon >= 0) { p->pass = s + colon + 1; p->plen = at - colon - 1; }
            hstart = at + 1;
        }
        int pcolon = -1;
        for (int j = hstart; j < aend; j++) if (s[j] == ':') pcolon = j;
        p->host = s + hstart;
        p->hlen = (pcolon >= 0 ? pcolon : aend) - hstart;
        if (pcolon >= 0) { p->port = s + pcolon + 1; p->portlen = aend - pcolon - 1; }
    }
    int pstart = i;
    while (i < n && s[i] != '?' && s[i] != '#') i++;
    p->path = s + pstart; p->pathlen = i - pstart;
    if (i < n && s[i] == '?') {
        int q = ++i;
        while (i < n && s[i] != '#') i++;
        p->query = s + q; p->qlen = i - q;
    }
    if (i < n && s[i] == '#') { p->frag = s + i + 1; p->flen = n - i - 1; }
}

static JSValue split_str(JSContext *ctx, const char *s, int n)
{ return JS_NewStringLen(ctx, s ? s : "", (size_t)(s && n > 0 ? n : 0)); }

static JSValue hl_get_split(JSContext *ctx, JSValueConst this_val, int magic)
{
    struct node *n = js_dom_node_from(this_val);
    if (!n || n->type != N_ELEM) return JS_NewString(ctx, "");
    int vlen = 0;
    const char *v = js_dom_attr_len(n, "href", &vlen);
    if (!v) return JS_NewString(ctx, "");

    char *abs = split_absolutize(ctx, v, vlen);
    if (!abs) return JS_NewString(ctx, "");
    struct sparts p;
    split_parse(abs, &p);

    JSValue out;
    char buf[1024];
    switch (magic) {
    case URLC_HREF:     out = JS_NewString(ctx, abs); break;
    case URLC_PROTOCOL:
        snprintf(buf, sizeof buf, "%.*s:", p.slen, p.scheme ? p.scheme : "");
        out = JS_NewString(ctx, buf); break;
    case URLC_USERNAME: out = split_str(ctx, p.user, p.ulen); break;
    case URLC_PASSWORD: out = split_str(ctx, p.pass, p.plen); break;
    case URLC_HOST:
        if (p.port) snprintf(buf, sizeof buf, "%.*s:%.*s", p.hlen, p.host, p.portlen, p.port);
        else        snprintf(buf, sizeof buf, "%.*s", p.hlen, p.host ? p.host : "");
        out = JS_NewString(ctx, buf); break;
    case URLC_HOSTNAME: out = split_str(ctx, p.host, p.hlen); break;
    case URLC_PORT:     out = split_str(ctx, p.port, p.portlen); break;
    case URLC_PATHNAME: out = split_str(ctx, p.path, p.pathlen); break;
    case URLC_SEARCH:
        if (!p.query || p.qlen <= 0) { out = JS_NewString(ctx, ""); break; }
        snprintf(buf, sizeof buf, "?%.*s", p.qlen, p.query);
        out = JS_NewString(ctx, buf); break;
    case URLC_HASH:
        if (!p.frag || p.flen <= 0) { out = JS_NewString(ctx, ""); break; }
        snprintf(buf, sizeof buf, "#%.*s", p.flen, p.frag);
        out = JS_NewString(ctx, buf); break;
    default:
        snprintf(buf, sizeof buf, "%.*s://%.*s", p.slen, p.scheme ? p.scheme : "",
                 p.hlen, p.host ? p.host : "");
        out = JS_NewString(ctx, buf); break;
    }
    free(abs);
    return out;
}

/* Reassembly by concatenation: rebuild the string with one piece swapped. */
static JSValue hl_set_split(JSContext *ctx, JSValueConst this_val, JSValueConst val,
                            int magic)
{
    struct node *n = js_dom_node_from(this_val);
    if (!n || n->type != N_ELEM || magic == URLC_ORIGIN) return JS_UNDEFINED;
    size_t slen = 0;
    const char *s = JS_ToCStringLen(ctx, &slen, val);
    if (!s) return JS_EXCEPTION;
    if (magic == URLC_HREF) {
        js_dom_attr_write(ctx, n, "href", s, (int)slen);
        JS_FreeCString(ctx, s);
        return JS_UNDEFINED;
    }
    int vlen = 0;
    const char *v = js_dom_attr_len(n, "href", &vlen);
    char *abs = v ? split_absolutize(ctx, v, vlen) : 0;
    if (!abs) { JS_FreeCString(ctx, s); return JS_UNDEFINED; }
    struct sparts p;
    split_parse(abs, &p);

#define PC(field, len) (magic == want ? s : (p.field ? p.field : "")), \
                       (magic == want ? (int)slen : p.len)
    char out[2048];
    const char *sc = p.scheme ? p.scheme : ""; int scl = p.slen;
    const char *us = p.user   ? p.user   : ""; int usl = p.ulen;
    const char *pw = p.pass   ? p.pass   : ""; int pwl = p.plen;
    const char *ho = p.host   ? p.host   : ""; int hol = p.hlen;
    const char *po = p.port   ? p.port   : ""; int pol = p.portlen;
    const char *pa = p.path   ? p.path   : ""; int pal = p.pathlen;
    const char *qu = p.query  ? p.query  : ""; int qul = p.qlen;
    const char *fr = p.frag   ? p.frag   : ""; int frl = p.flen;
    switch (magic) {
    case URLC_PROTOCOL: sc = s; scl = (int)slen; break;
    case URLC_USERNAME: us = s; usl = (int)slen; break;
    case URLC_PASSWORD: pw = s; pwl = (int)slen; break;
    case URLC_HOSTNAME: ho = s; hol = (int)slen; break;
    case URLC_HOST:     ho = s; hol = (int)slen; po = ""; pol = 0; break;
    case URLC_PORT:     po = s; pol = (int)slen; break;
    case URLC_PATHNAME: pa = s; pal = (int)slen; break;
    case URLC_SEARCH:   qu = s; qul = (int)slen; break;
    case URLC_HASH:     fr = s; frl = (int)slen; break;
    }
#undef PC
    int o = snprintf(out, sizeof out, "%.*s://", scl, sc);
    if (usl > 0 || pwl > 0) {
        o += snprintf(out + o, sizeof out - (size_t)o, "%.*s", usl, us);
        if (pwl > 0) o += snprintf(out + o, sizeof out - (size_t)o, ":%.*s", pwl, pw);
        o += snprintf(out + o, sizeof out - (size_t)o, "@");
    }
    o += snprintf(out + o, sizeof out - (size_t)o, "%.*s", hol, ho);
    if (pol > 0) o += snprintf(out + o, sizeof out - (size_t)o, ":%.*s", pol, po);
    o += snprintf(out + o, sizeof out - (size_t)o, "%.*s", pal, pa);
    if (qul > 0) o += snprintf(out + o, sizeof out - (size_t)o, "?%.*s", qul, qu);
    if (frl > 0) o += snprintf(out + o, sizeof out - (size_t)o, "#%.*s", frl, fr);
    js_dom_attr_write(ctx, n, "href", out, o);

    free(abs);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
#endif /* URLELEM_SPLITTER */

/* The one seam the control switches. Everything else in this file -- the base
 * URL, the install, the three refusing entry points -- is identical in both
 * builds, so a red run names the decomposition and nothing else. */
static JSValue hl_get(JSContext *ctx, JSValueConst this_val, int magic)
{
#ifdef URLELEM_SPLITTER
    return hl_get_split(ctx, this_val, magic);
#else
    return hl_get_spec(ctx, this_val, magic);
#endif
}

static JSValue hl_set(JSContext *ctx, JSValueConst this_val, JSValueConst val,
                      int magic)
{
#ifdef URLELEM_SPLITTER
    return hl_set_split(ctx, this_val, val, magic);
#else
    return hl_set_spec(ctx, this_val, val, magic);
#endif
}

/* The IDL order, which is also js_url.h's URLC_* order -- so `magic` is the
 * component index and there is no second table to keep in step. */
static const char *const HL_NAMES[URLC__N] = {
    "href", "protocol", "username", "password", "host", "hostname",
    "port", "pathname", "search", "hash", "origin"
};

static void define_hyperlink(JSContext *ctx, JSValueConst proto)
{
    if (!JS_IsObject(proto)) return;
    for (int i = 0; i < URLC__N; i++) {
        JSAtom a = JS_NewAtom(ctx, HL_NAMES[i]);
        JSValue get = JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)hl_get,
                                           HL_NAMES[i], 0, JS_CFUNC_getter_magic, i);
        JSValue set = (i == URLC_ORIGIN)
            ? JS_UNDEFINED
            : JS_NewCFunctionMagic(ctx, (JSCFunctionMagic *)hl_set,
                                   HL_NAMES[i], 1, JS_CFUNC_setter_magic, i);
        JS_DefinePropertyGetSet(ctx, proto, a, get, set,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
}

static JSValue proto_of(JSContext *ctx, JSValueConst g, const char *ctor_name)
{
    JSValue c = JS_GetPropertyStr(ctx, g, ctor_name);
    JSValue p = JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
    JS_FreeValue(ctx, c);
    return p;
}

/* ======================================================================
 * The entry points that must refuse a URL they cannot parse
 * ======================================================================
 *
 * IN JAVASCRIPT, and that is a decision rather than the easy road. Each of
 * these three has to run the URL parser and then hand off to an implementation
 * that already exists somewhere else -- js_webapi.c's XHR is itself a JS
 * prelude, `sendBeacon` is one line in js_platform.c's. Doing the parse in C
 * would mean either duplicating those bodies here or reaching into another
 * file's private state; wrapping them in JS keeps each owner's implementation
 * intact and adds exactly the check. The parser reached is still this file's
 * -- `new URL()` IS js_url.c by the time we install, which is precisely why
 * this runs last.
 *
 * `window.open` has no implementation to defer to and returns null: this
 * browser opens no auxiliary browsing context, and null is the answer real
 * browsers give when they refuse one (a blocked popup), so a page that guards
 * its return value takes a branch it already has. What it must NOT do is what
 * it did before -- not exist, so that `self.open(bad)` threw TypeError and a
 * page could not tell a refusal from a missing feature. */
static const char *const URLBIND_PRELUDE =
"(function (G) {\n"
"  var DE = G.DOMException;\n"
"  function baseurl() { return (G.document && G.document.baseURI) || (G.location && G.location.href); }\n"
"  function check(u, what) {\n"
"    try { return new G.URL(String(u), baseurl()); }\n"
"    catch (e) {\n"
"      throw DE ? new DE(\"Failed to execute '\" + what + \"': the URL is invalid.\", 'SyntaxError')\n"
"               : new SyntaxError('invalid URL'); }\n"
"  }\n"
   /* XMLHttpRequest.open: SyntaxError, per xhr's "parse url, and if that
    * returns failure, throw a SyntaxError exception". The original body is
    * kept and called with the ORIGINAL arguments -- async, user, password and
    * everything else it grows later stay its owner's business. */
"  var P = G.XMLHttpRequest && G.XMLHttpRequest.prototype;\n"
"  if (P && typeof P.open === 'function' && !P.open.__urlchecked) {\n"
"    var xopen = P.open;\n"
"    P.open = function (m, u) {\n"
"      if (arguments.length > 1) check(u, 'open');\n"
"      return xopen.apply(this, arguments);\n"
"    };\n"
"    try { P.open.__urlchecked = true; } catch (e) {}\n"
"  }\n"
   /* navigator.sendBeacon: a TypeError, not a DOMException -- the beacon spec
    * parses the url and "throws a TypeError" on failure. Returning false
    * afterwards stays honest: nothing is sent. */
"  var nav = G.navigator;\n"
"  if (nav) {\n"
"    var beacon = nav.sendBeacon;\n"
"    try {\n"
"      Object.defineProperty(nav, 'sendBeacon', { configurable: true, writable: true,\n"
"        value: function (u) {\n"
"          if (arguments.length < 1) throw new TypeError('sendBeacon requires a URL');\n"
"          try { new G.URL(String(u), baseurl()); }\n"
"          catch (e) { throw new TypeError('sendBeacon: the URL is invalid.'); }\n"
"          return typeof beacon === 'function' ? beacon.apply(this, arguments) : false;\n"
"        } });\n"
"    } catch (e) {}\n"
"  }\n"
"  if (typeof G.open !== 'function') {\n"
"    G.open = function (u) {\n"
"      if (arguments.length > 0 && String(u) !== '') check(u, 'open');\n"
"      return null;\n"
"    };\n"
"  }\n"
"})(globalThis);\n";

/* ======================================================================
 * install
 * ====================================================================== */

void js_urlbind_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue g = JS_GetGlobalObject(ctx);

    /* baseURI on Node.prototype: `document.baseURI`, `element.baseURI` and
     * `text.baseURI` are the same answer and the DOM says so. */
    JSValue np = proto_of(ctx, g, "Node");
    if (JS_IsObject(np)) {
        JSAtom a = JS_NewAtom(ctx, "baseURI");
        JSValue get = JS_NewCFunction2(ctx, (JSCFunction *)node_baseuri_get,
                                       "get baseURI", 0, JS_CFUNC_getter, 0);
        JS_DefinePropertyGetSet(ctx, np, a, get, JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, np);

    /* <a> and <area>, and NOT <link>: its href is a plain reflected URL with
     * no decomposition, which is a distinction the standard draws on purpose
     * and which js_reflect.c's table already gets right. */
    JSValue ap = proto_of(ctx, g, "HTMLAnchorElement");
    define_hyperlink(ctx, ap);
    JS_FreeValue(ctx, ap);
    JSValue rp = proto_of(ctx, g, "HTMLAreaElement");
    define_hyperlink(ctx, rp);
    JS_FreeValue(ctx, rp);

    JS_FreeValue(ctx, g);

    JSValue r = JS_Eval(ctx, URLBIND_PRELUDE, strlen(URLBIND_PRELUDE),
                        "<js_urlbind>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeValue(ctx, r);
}

#endif /* URL_CORE_ONLY */
