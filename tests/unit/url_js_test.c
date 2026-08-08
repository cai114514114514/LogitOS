/* url_js_test.c -- the JS surface of c/apps/browser/js_url.c.
 *
 * url_test.c drives the C algorithm against the corpus's data and is where the
 * 1,169 corpus cases live. This file asserts the things that data CANNOT see,
 * which is everything about being a Web IDL interface:
 *
 *   - the components are ACCESSORS on URL.prototype, not own data properties
 *   - the constructor THROWS a TypeError; it does not return null
 *   - URL.canParse / URL.parse, toJSON, toString
 *   - URLSearchParams built from a string, a sequence, a record, and another
 *     URLSearchParams
 *   - the iterator protocol: for..of, entries/keys/values, forEach
 *   - and the part that is easy to leave HALF done: the link between a URL and
 *     its searchParams is TWO-WAY. Mutating the params must change the URL's
 *     href, and assigning to the URL's search must change the params.
 *
 * It installs onto a BARE QuickJS context -- no DOM, no fetch, no page. That
 * is an assertion too: the URL globals must not need the rest of the browser,
 * because a Worker and the WPT runner both build contexts without one. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "quickjs.h"
#include "js_url.h"

static int g_pass, g_fail;

static void check(JSContext *ctx, const char *name, const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<url>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("  FAIL %-38s threw: %s\n", name, m ? m : "?");
        JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        g_fail++;
        return;
    }
    int ok = JS_ToBool(ctx, v);
    if (!ok) {
        /* re-evaluate as a string so the report shows what was produced */
        printf("  FAIL %-38s %s\n", name, src);
        g_fail++;
    } else g_pass++;
    JS_FreeValue(ctx, v);
}

/* A block of statements ending in an expression: wrapped so `check` can run it
 * as one value. */
#define T(name, body) check(ctx, name, "(function(){" body "})()")

int main(void)
{
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    /* Stand in for what is really on the context when this installs: an
     * EARLIER URL, with a static hung on it by another file. In the browser
     * that is js_platform.c's URL.createObjectURL, installed before this file
     * runs and lost outright if the replacement does not carry it. */
    {
        const char *pre =
            "globalThis.URL = function(){};"
            "globalThis.URL.createObjectURL = function(){ return 'data:,carried'; };"
            "globalThis.URL.revokeObjectURL = function(){};"
            "globalThis.URLSearchParams = function(){};"
            "globalThis.URLSearchParams.__legacy = 1;";
        JSValue v = JS_Eval(ctx, pre, strlen(pre), "<pre>", JS_EVAL_TYPE_GLOBAL);
        JS_FreeValue(ctx, v);
    }
    js_url_install(ctx);

    printf("url_js_test: the URL and URLSearchParams JS surface\n");

    /* ---- what the replacement had to carry ---- */
    T("URL.createObjectURL survives the replacement",
      "return typeof URL.createObjectURL === 'function'"
      "  && URL.createObjectURL() === 'data:,carried'"
      "  && typeof URL.revokeObjectURL === 'function';");
    T("a carried static does not shadow one of ours",
      "return typeof URL.canParse === 'function' && URLSearchParams.__legacy === 1;");
    T("the replacement really replaced",
      "return new URL('http://a:80/x/../y').href === 'http://a/y';");

    /* ---- the interface shape ---- */
    T("URL is a constructor",
      "return typeof URL === 'function' && URL.length === 1;");
    T("components are prototype accessors",
      "var d = Object.getOwnPropertyDescriptor(URL.prototype, 'href');"
      "var u = new URL('http://a/');"
      "return d && typeof d.get === 'function' && typeof d.set === 'function'"
      "  && !Object.prototype.hasOwnProperty.call(u, 'href');");
    T("origin has no setter",
      "var d = Object.getOwnPropertyDescriptor(URL.prototype, 'origin');"
      "return d && typeof d.get === 'function' && d.set === undefined;");
    T("constructor throws TypeError on failure",
      "try { new URL('not a url'); return false; }"
      "catch (e) { return e instanceof TypeError; }");
    T("bad base throws too",
      "try { new URL('/x', 'not a url'); return false; }"
      "catch (e) { return e instanceof TypeError; }");
    T("no arguments throws",
      "try { new URL(); return false; } catch (e) { return e instanceof TypeError; }");
    T("href setter throws on failure",
      "var u = new URL('http://a/');"
      "try { u.href = 'nonsense'; return false; }"
      "catch (e) { return e instanceof TypeError && u.href === 'http://a/'; }");

    /* ---- the statics ---- */
    T("URL.canParse", "return URL.canParse('http://a/') === true && URL.canParse('%%') === false;");
    T("URL.canParse with a base", "return URL.canParse('/p', 'http://a/') === true;");
    T("URL.parse returns null, not a throw",
      "return URL.parse('nonsense') === null && URL.parse('http://a/').href === 'http://a/';");
    T("toJSON and toString are href",
      "var u = new URL('http://a/b?c#d');"
      "return u.toJSON() === u.href && String(u) === u.href && JSON.stringify(u) === '\"'+u.href+'\"';");

    /* ---- values, spot checks over the algorithm through the bindings ---- */
    T("relative resolution",
      "return new URL('../c', 'http://a/x/y/z').href === 'http://a/x/c';");
    T("default port dropped",
      "return new URL('http://a:80/').href === 'http://a/' && new URL('http://a:81/').port === '81';");
    T("IPv6 host is bracketed",
      "var u = new URL('http://[2001:db8::1]:8080/');"
      "return u.hostname === '[2001:db8::1]' && u.host === '[2001:db8::1]:8080';");
    T("IPv4 in dotted-hex normalizes",
      "return new URL('http://0x7f.1/').hostname === '127.0.0.1';");
    T("opaque path is not normalized",
      "return new URL('data:x/..').pathname === 'x/..';");
    T("origin of a non-special scheme is null",
      "return new URL('nonspecial://a/').origin === 'null' && new URL('http://a/').origin === 'http://a';");

    /* ---- URLSearchParams construction ---- */
    T("from a string, leading ? stripped",
      "return new URLSearchParams('?a=1&b=2').toString() === 'a=1&b=2';");
    T("from a sequence",
      "return new URLSearchParams([['a','1'],['b','2']]).toString() === 'a=1&b=2';");
    T("a non-pair in a sequence throws",
      "try { new URLSearchParams([['a']]); return false; }"
      "catch (e) { return e instanceof TypeError; }");
    T("from a record",
      "return new URLSearchParams({a:'1', b:'2'}).toString() === 'a=1&b=2';");
    T("from another URLSearchParams",
      "var p = new URLSearchParams('a=1&a=2');"
      "return new URLSearchParams(p).toString() === 'a=1&a=2';");
    T("no argument is empty",
      "return new URLSearchParams().toString() === '' && new URLSearchParams().size === 0;");

    /* ---- the accessors ---- */
    T("get/getAll/has",
      "var p = new URLSearchParams('a=1&a=2&b=3');"
      "return p.get('a') === '1' && p.getAll('a').join(',') === '1,2'"
      "  && p.get('zz') === null && p.has('b') === true && p.has('zz') === false;");
    T("has with a value",
      "var p = new URLSearchParams('a=1&a=2');"
      "return p.has('a','2') === true && p.has('a','9') === false;");
    T("set replaces the first and drops the rest",
      "var p = new URLSearchParams('a=1&b=2&a=3');"
      "p.set('a','9'); return p.toString() === 'a=9&b=2';");
    T("set appends when absent",
      "var p = new URLSearchParams('a=1'); p.set('b','2');"
      "return p.toString() === 'a=1&b=2';");
    T("delete with a value",
      "var p = new URLSearchParams('a=1&a=2'); p.delete('a','1');"
      "return p.toString() === 'a=2';");
    T("append and size",
      "var p = new URLSearchParams('a=1'); p.append('a','2');"
      "return p.size === 2 && p.toString() === 'a=1&a=2';");
    T("sort is stable",
      "var p = new URLSearchParams('z=1&a=2&z=3&a=4'); p.sort();"
      "return p.toString() === 'a=2&a=4&z=1&z=3';");

    /* ---- iteration ---- */
    T("for..of yields pairs",
      "var p = new URLSearchParams('a=1&b=2'), out = [];"
      "for (var e of p) out.push(e[0] + ':' + e[1]);"
      "return out.join(',') === 'a:1,b:2';");
    T("entries/keys/values",
      "var p = new URLSearchParams('a=1&b=2');"
      "return Array.from(p.keys()).join(',') === 'a,b'"
      "  && Array.from(p.values()).join(',') === '1,2'"
      "  && Array.from(p.entries()).length === 2;");
    T("forEach gets (value, name, params)",
      "var p = new URLSearchParams('a=1'), seen = null;"
      "p.forEach(function(v,n,o){ seen = [v,n,o === p].join(','); });"
      "return seen === '1,a,true';");

    /* ---- the iterator is LIVE, not a snapshot ----
     * Four WPT subtests in urlsearchparams-foreach.any.js turn on this and a
     * materialised array passes none of them. */
    T("iterator: deleting the NEXT param skips it",
      "var u = new URL('http://a/?p0=0&p1=1&p2=2'), sp = u.searchParams, seen = [];"
      "for (var e of sp) { if (e[0] === 'p0') sp.delete('p1'); seen.push(e[0]); }"
      "return seen.join(',') === 'p0,p2';");
    T("iterator: deleting the CURRENT param shifts the next in",
      "var u = new URL('http://a/?p0=0&p1=1&p2=2'), sp = u.searchParams, seen = [];"
      "for (var e of sp) { if (e[0] === 'p0') sp.delete('p0'); else seen.push(e[0]); }"
      "return seen.join(',') === 'p2';");
    T("iterator: deleting every param seen leaves the odd ones",
      "var u = new URL('http://a/?p0=0&p1=1&p2=2'), sp = u.searchParams, seen = [];"
      "for (var e of sp) { seen.push(e[0]); sp.delete(e[0]); }"
      "return seen.join(',') === 'p0,p2' && String(sp) === 'p1=1';");
    T("iterator: assigning to url.search mid-loop is seen",
      "var u = new URL('http://a/?a=1&b=2&c=3&d=4'), sp = u.searchParams, c = [];"
      "for (var i of sp) { u.search = 'x=1&y=2&z=3'; c.push(i.join(':')); }"
      "return c.join(',') === 'a:1,y:2,z:3';");
    T("iterator: an empty list yields nothing",
      "var n = 0; for (var e of new URL('http://a/').searchParams) n++;"
      "return n === 0;");

    /* ---- the sequence conversion goes through the ITERATOR ----
     * There is no URLSearchParams member in the IDL union, so another params
     * object is drained through its Symbol.iterator like any other iterable --
     * which is why replacing that method is observable. */
    T("sequence init uses a custom [Symbol.iterator]",
      "var p = new URLSearchParams();"
      "p[Symbol.iterator] = function*(){ yield ['a','b']; };"
      "return new URLSearchParams(p).get('a') === 'b';");
    T("a 1- or 3-element sub-sequence throws",
      "var n = 0;"
      "try { new URLSearchParams([[1]]); } catch (e) { if (e instanceof TypeError) n++; }"
      "try { new URLSearchParams([[1,2,3]]); } catch (e) { if (e instanceof TypeError) n++; }"
      "return n === 2;");
    T("a Map is a valid sequence source",
      "return new URLSearchParams(new Map([['a','1'],['b','2']])).toString() === 'a=1&b=2';");

    /* ---- a record is a MAP: duplicate converted keys collapse ---- */
    T("record: unpaired surrogates collapse to one U+FFFD",
      "var p = new URLSearchParams({'\\uD835x':'1', 'xx':'2', '\\uD83Dx':'3'});"
      "var out = Array.from(p).map(function(e){return e.join(':');}).join(',');"
      "return out === '\\uFFFDx:3,xx:2';");
    T("record: three keys collapsing to one",
      "var p = new URLSearchParams({'x\\uDC53':'1','x\\uDC5C':'2','x\\uDC65':'3'});"
      "var out = Array.from(p).map(function(e){return e.join(':');}).join(',');"
      "return out === 'x\\uFFFD:3';");
    T("a lone surrogate is ONE U+FFFD, not three",
      "return new URLSearchParams('a=1').constructor === URLSearchParams"
      "  && new URL('http://a/?x=\\uD835').search === '?x=%EF%BF%BD';");

    /* ---- THE LIVE LINK, both directions ---- */
    T("searchParams is the same object each time",
      "var u = new URL('http://a/?x=1');"
      "return u.searchParams === u.searchParams;");
    T("params reflect the URL's query",
      "return new URL('http://a/?x=1&y=2').searchParams.get('y') === '2';");
    T("params -> url: append",
      "var u = new URL('http://a/');"
      "u.searchParams.append('x','1');"
      "return u.href === 'http://a/?x=1' && u.search === '?x=1';");
    T("params -> url: delete to empty nulls the query",
      "var u = new URL('http://a/?x=1');"
      "u.searchParams.delete('x');"
      "return u.href === 'http://a/' && u.search === '';");
    T("params -> url: set and sort",
      "var u = new URL('http://a/?b=2&a=1');"
      "u.searchParams.sort();"
      "return u.search === '?a=1&b=2';");
    T("url -> params: the search setter",
      "var u = new URL('http://a/?x=1');"
      "u.search = '?y=2';"
      "return u.searchParams.get('x') === null && u.searchParams.get('y') === '2';");
    T("url -> params: the href setter",
      "var u = new URL('http://a/?x=1');"
      "var p = u.searchParams;"
      "u.href = 'http://b/?z=9';"
      "return p.get('x') === null && p.get('z') === '9' && p === u.searchParams;");
    T("form encoding round trip through the URL",
      "var u = new URL('http://a/');"
      "u.searchParams.append('a b', 'c+d');"
      "return u.search === '?a+b=c%2Bd' && u.searchParams.get('a b') === 'c+d';");

    /* ---- U+0000, which is where a NUL-terminated binding quietly loses ----
     * A JS string may contain U+0000 and "%00" decodes to one. Everything
     * below passed with JS_ToCString and was wrong: the string was truncated
     * at the NUL on the way in and again on the way out. Four WPT subtests
     * (urlsearchparams-constructor "Parse \0" / "Parse %00",
     * urlsearchparams-stringifier "Serialize \0", url-setters-stripping
     * "Setting protocol with U+0000 before inserted colon") are exactly
     * these. */
    T("params: parse a literal NUL",
      "var p = new URLSearchParams('a=b\\0c');"
      "if (p.get('a') !== 'b\\0c') return false;"
      "p = new URLSearchParams('a\\0b=c');"
      "return p.get('a\\0b') === 'c';");
    T("params: parse %00",
      "var p = new URLSearchParams('a=b%00c');"
      "if (p.get('a') !== 'b\\0c') return false;"
      "p = new URLSearchParams('a%00b=c');"
      "return p.get('a\\0b') === 'c';");
    T("params: serialize a NUL",
      "var p = new URLSearchParams();"
      "p.append('a\\0b', 'c\\0d');"
      "return p.toString() === 'a%00b=c%00d';");
    T("params: a record key with a NUL",
      "var p = new URLSearchParams({'a\\0b': '42'});"
      "return p.toString() === 'a%00b=42' && p.get('a\\0b') === '42';");
    T("params: has/delete match past the NUL",
      "var p = new URLSearchParams('a%00b=1&a=2');"
      "return p.has('a\\0b') && p.has('a') && p.getAll('a').length === 1;");
    T("protocol setter: a leading NUL is not stripped, so it is a no-op",
      "var u = new URL('http://test/');"
      "u.protocol = '\\0https';"
      "return u.protocol === 'http:';");
    T("url: a NUL in a host is a failure, not a truncation",
      /* NOT a trailing NUL -- that is a C0 control and the parser strips
       * leading and trailing ones from the whole input before it starts, so
       * "http://hello\0" is just "http://hello". It has to be in the middle. */
      "return URL.canParse('http://hel\\0lo/') === false"
      "  && URL.canParse('http://ho%00st/') === false;");
    T("url: a NUL in a fragment is percent-encoded",
      "return new URL('https://x/#\\0y').href === 'https://x/#%00y';");

    /* ---- setters through the bindings ---- */
    T("protocol setter",
      "var u = new URL('http://a/'); u.protocol = 'https';"
      "return u.href === 'https://a/';");
    T("a special/non-special protocol change is refused",
      "var u = new URL('http://a/'); u.protocol = 'mailto';"
      "return u.protocol === 'http:';");
    T("hostname setter",
      "var u = new URL('http://a/'); u.hostname = 'b';"
      "return u.href === 'http://b/';");
    T("port setter, empty string nulls it",
      "var u = new URL('http://a:81/'); u.port = '';"
      "return u.href === 'http://a/' && u.port === '';");
    T("pathname setter re-parses",
      "var u = new URL('http://a/x'); u.pathname = '/y/../z';"
      "return u.pathname === '/z';");
    T("hash setter strips ONE leading #",
      /* '#' is not in the fragment percent-encode set, so the second one
       * survives verbatim: "##x" -> fragment "#x" -> hash "##x". */
      "var u = new URL('http://a/'); u.hash = '##x';"
      "return u.hash === '##x';");
    T("credentials setters",
      "var u = new URL('http://a/'); u.username = 'u'; u.password = 'p';"
      "return u.href === 'http://u:p@a/';");

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    printf("  %d/%d\n", g_pass, g_pass + g_fail);
    if (g_fail) { printf("url_js_test: FAILED (%d)\n", g_fail); return 1; }
    printf("url_js_test: ok\n");
    return 0;
}
