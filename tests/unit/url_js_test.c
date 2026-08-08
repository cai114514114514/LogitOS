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
    js_url_install(ctx);

    printf("url_js_test: the URL and URLSearchParams JS surface\n");

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
