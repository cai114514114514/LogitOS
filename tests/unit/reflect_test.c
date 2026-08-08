/* reflect_test.c -- the IDL attribute reflection COERCIONS.
 *
 * WHAT THIS TEST IS FOR, and it is not "does el.title work".
 *
 * The whole risk in c/apps/browser/js_reflect.c is that reflection looks right
 * while being a plain string pass-through. `el.title = "x"` round-trips either
 * way. Ordinary pages behave either way. What separates a correct
 * implementation from a convincing one is the TYPE: an enumerated attribute
 * that falls back to its invalid-value default (which is not its missing-value
 * default), an integer that rejects U+000B as leading whitespace, an unsigned
 * long that writes its own default when handed 2^32-1, a setter that throws
 * IndexSizeError, a URL that comes back absolute.
 *
 * So every assertion below is chosen to FAIL under a string pass-through, and
 * `make test-reflect-negctl` builds exactly that and requires this file to go
 * red. An assertion that both implementations satisfy is not evidence and is
 * not here.
 *
 * It runs off the TABLE, not off a copy of it: every attribute named below is
 * one tools/gen_reflect.py generated from the WPT corpus, so a table that stops
 * describing reality fails here too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "js_dom.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int g_fails;

static void run(JSContext *ctx, const char *src, const char *what)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<reflect_test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("FAIL %s: uncaught %s\n", what, m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        g_fails++;
    }
    JS_FreeValue(ctx, v);
}

/* A minimal URL constructor, so the url-typed assertions can run without
 * linking the whole web-API layer. It is deliberately small: this test is about
 * whether a reflected URL getter RESOLVES at all, not about URL parsing, which
 * has its own corpus and its own line. */
static const char URL_SHIM[] =
"globalThis.location = { href: 'http://example.test/dir/page.html' };\n"
"globalThis.URL = function (u, base) {\n"
"  u = String(u); base = String(base || location.href);\n"
"  var abs;\n"
"  if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(u)) abs = u;\n"
"  else if (u.slice(0, 2) === '//') abs = base.slice(0, base.indexOf(':') + 1) + u;\n"
"  else if (u.charAt(0) === '/') abs = base.slice(0, base.indexOf('/', 8)) + u;\n"
"  else if (u === '') abs = base;\n"
"  else abs = base.slice(0, base.lastIndexOf('/') + 1) + u;\n"
"  this.href = abs;\n"
   /* The decomposition members, because HTMLHyperlinkElementUtils reads them
    * off this object -- a shim that only produced .href would leave a.protocol
    * empty and the assertion would be about the shim, not about the code. */
"  var m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)\\/\\/([^\\/?#]*)([^?#]*)(\\?[^#]*)?(#.*)?$/.exec(abs);\n"
"  this.protocol = m ? m[1] : '';\n"
"  this.host     = m ? m[2] : '';\n"
"  this.hostname = m ? m[2].split(':')[0] : '';\n"
"  this.port     = m && m[2].indexOf(':') >= 0 ? m[2].split(':')[1] : '';\n"
"  this.pathname = m ? (m[3] || '/') : '';\n"
"  this.search   = m && m[4] ? m[4] : '';\n"
"  this.hash     = m && m[5] ? m[5] : '';\n"
"  this.origin   = m ? m[1] + '//' + m[2] : '';\n"
"};\n";

/* The assertions. `ck(cond, name)` collects rather than throws, so one wrong
 * answer does not hide the twenty after it. */
static const char CHECKS[] =
"var out = [];\n"
"function ck(c, n) { if (!c) out.push(n); }\n"
"function eq(a, b, n) { if (a !== b) out.push(n + ': expected ' + JSON.stringify(b) +\n"
"                                             ' got ' + JSON.stringify(a)); }\n"
"var D = document;\n"

/* ---- enumerated: the invalid-value default is NOT the missing-value default.
 * <input formmethod=BOGUS>.formMethod is "get"; <input>.formMethod is "". A
 * pass-through answers "BOGUS" and "" -- it gets the second one right, which is
 * why a test that only checks the unset case proves nothing. */
"var i = D.createElement('input');\n"
"eq(i.formMethod, '', 'enum missing-value default');\n"
"i.setAttribute('formmethod', 'BOGUS');\n"
"eq(i.formMethod, 'get', 'enum invalid-value default');\n"
"i.setAttribute('formmethod', 'POST');\n"
"eq(i.formMethod, 'post', 'enum canonical case');\n"
"eq(i.getAttribute('formmethod'), 'POST', 'enum leaves the content attribute alone');\n"

/* A keyword with a NUL after it is NOT that keyword. This is the assertion that
 * fails the moment an attribute value goes through a C string. */
"i.setAttribute('formmethod', 'get\\0');\n"
"eq(i.formMethod, 'get', 'enum: keyword+NUL is invalid, not a match');\n"
"eq(i.getAttribute('formmethod').length, 4, 'getAttribute keeps the NUL');\n"

/* The default is per ATTRIBUTE. input.type falls back to "text", form.method to
 * "get"; both are enumerated, and a type-wide default gets both wrong. */
"eq(i.type, 'text', 'input.type default');\n"
"eq(D.createElement('form').method, 'get', 'form.method default');\n"

/* ---- boolean ---------------------------------------------------------- */
"var d = D.createElement('div');\n"
"eq(d.hidden, false, 'boolean absent is false');\n"
"d.setAttribute('hidden', 'false');\n"
"eq(d.hidden, true, 'boolean PRESENT is true whatever the value says');\n"
"d.hidden = false;\n"
"eq(d.hasAttribute('hidden'), false, 'boolean false removes the attribute');\n"
"d.hidden = true;\n"
"eq(d.getAttribute('hidden'), '', 'boolean true writes the empty string');\n"

/* ---- long: the HTML whitespace set, which is not isspace() -------------- */
"var ol = D.createElement('ol');\n"
"eq(ol.start, 1, 'long: per-attribute default (ol.start is 1, not 0)');\n"
"ol.setAttribute('start', '\\t7');\n"
"eq(ol.start, 7, 'long: TAB is leading whitespace');\n"
"ol.setAttribute('start', '\\u000B7');\n"
"eq(ol.start, 1, 'long: U+000B is NOT leading whitespace');\n"
"ol.setAttribute('start', '\\u00A07');\n"
"eq(ol.start, 1, 'long: U+00A0 is NOT leading whitespace');\n"
"ol.setAttribute('start', '5%');\n"
"eq(ol.start, 5, 'long: a trailing non-digit does not fail the parse');\n"
"ol.setAttribute('start', '+100');\n"
"eq(ol.start, 100, 'long: a leading + parses');\n"
"ol.setAttribute('start', '4294967296');\n"
"eq(ol.start, 1, 'long: out of range falls back to the default');\n"

/* ---- clamped unsigned long: td.colSpan, [1, 1000], default 1 ------------ */
"var td = D.createElement('td');\n"
"eq(td.colSpan, 1, 'clamped: default');\n"
"td.setAttribute('colspan', 'abc');\n"
"eq(td.colSpan, 1, 'clamped: unparseable falls back to the default');\n"
"td.setAttribute('colspan', '0');\n"
"eq(td.colSpan, 1, 'clamped: below min clamps to min');\n"
"td.setAttribute('colspan', '99999');\n"
"eq(td.colSpan, 1000, 'clamped: above max clamps to max');\n"

/* ---- unsigned long: WebIDL ToUint32 runs BEFORE the range rule ---------- */
"var img = D.createElement('img');\n"
"img.width = -1;\n"
"eq(img.getAttribute('width'), '0', 'ulong: -1 wraps to 2^32-1, then out of range -> default');\n"
"img.width = 257;\n"
"eq(img.getAttribute('width'), '257', 'ulong: an in-range value is written as itself');\n"
"eq(img.width, 257, 'ulong: and reads back');\n"
"img.setAttribute('width', '-3');\n"
"eq(img.width, 0, 'ulong: a negative content value is not a non-negative integer');\n"

/* ---- limited unsigned long: input.size throws on 0, default 20 ---------- */
"var s = D.createElement('input');\n"
"eq(s.size, 20, 'limited ulong: input.size default is 20');\n"
"var threw = null;\n"
"try { s.size = 0; } catch (e) { threw = e; }\n"
"ck(threw !== null, 'limited ulong: setting 0 throws');\n"
"ck(threw && threw.name === 'IndexSizeError', 'limited ulong: throws IndexSizeError');\n"
"ck(threw && threw.code === 1, 'limited ulong: the legacy code is 1');\n"
"s.size = 4294967295;\n"
"eq(s.getAttribute('size'), '20', 'limited ulong: out of range writes the default');\n"

/* ---- limited long: negative throws ------------------------------------- */
"var ml = D.createElement('input');\n"
"eq(ml.maxLength, -1, 'limited long: maxLength default is -1');\n"
"threw = null;\n"
"try { ml.maxLength = -1; } catch (e) { threw = e; }\n"
"ck(threw && threw.name === 'IndexSizeError', 'limited long: negative throws IndexSizeError');\n"

/* ---- limited unsigned long WITH FALLBACK: 0 does not throw, it defaults - */
"var ta = D.createElement('textarea');\n"
"eq(ta.cols, 20, 'fallback ulong: textarea.cols default is 20');\n"
"ta.cols = 0;\n"
"eq(ta.getAttribute('cols'), '20', 'fallback ulong: 0 falls back instead of throwing');\n"

/* ---- limited double: <= 0 is IGNORED on set ---------------------------- */
"var pr = D.createElement('progress');\n"
"eq(pr.max, 1, 'limited double: progress.max default is 1');\n"
"pr.setAttribute('max', '2.5');\n"
"eq(pr.max, 2.5, 'limited double: parses a fraction');\n"
"pr.max = -1;\n"
"eq(pr.getAttribute('max'), '2.5', 'limited double: a non-positive set leaves the attribute');\n"
"pr.setAttribute('max', '1e2');\n"
"eq(pr.max, 100, 'limited double: exponent form parses');\n"
"pr.setAttribute('max', '1 e2');\n"
"eq(pr.max, 1, 'limited double: parsing stops at the space');\n"

/* ---- URL: the property is absolute, the attribute is not --------------- */
"var a = D.createElement('a');\n"
"a.setAttribute('href', 'x.html');\n"
"eq(a.getAttribute('href'), 'x.html', 'url: getAttribute is the literal');\n"
"eq(a.href, 'http://example.test/dir/x.html', 'url: the property is resolved');\n"
"eq(a.protocol, 'http:', 'url: decomposition -- protocol');\n"
"eq(a.pathname, '/dir/x.html', 'url: decomposition -- pathname');\n"
"var lk = D.createElement('link');\n"
"eq(lk.href, '', 'url: an ABSENT attribute is the empty string');\n"
"lk.setAttribute('href', '');\n"
"eq(lk.href, 'http://example.test/dir/page.html',\n"
"   'url: a PRESENT empty attribute resolves to the document URL');\n"

/* ---- form.action defaults to the document's URL ------------------------ */
"ck(typeof D.URL === 'string' && D.URL.length > 0, 'document.URL exists');\n"
"eq(D.createElement('form').action, D.URL, 'url: form.action defaults to document.URL');\n"

/* ---- the document-level reflections, which live on another element ----- */
"D.body.setAttribute('bgcolor', 'red');\n"
"eq(D.bgColor, 'red', 'document.bgColor reads the BODY attribute');\n"
"D.bgColor = null;\n"
"eq(D.body.getAttribute('bgcolor'), '',\n"
"   'document.bgColor: null becomes the empty string, not \"null\"');\n"
"D.documentElement.setAttribute('dir', 'RTL');\n"
"eq(D.dir, 'rtl', 'document.dir reads documentElement, canonically cased');\n"

/* ---- a plain DOMString is still a plain DOMString ---------------------- */
"d.title = 7;\n"
"eq(d.getAttribute('title'), '7', 'string: a number is stringified');\n"
"d.title = undefined;\n"
"eq(d.getAttribute('title'), 'undefined', 'string: undefined is \"undefined\", not \"\"');\n"
"d.setAttribute('title', 'a\\0b');\n"
"eq(d.title.length, 3, 'string: a NUL survives the round trip');\n"

"globalThis.__out = out;\n";

int main(void)
{
    static const char HTML[] =
        "<!doctype html><html><head><title>t</title></head><body></body></html>";
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    if (!root) { printf("FAIL reflect_test: dom_parse returned nothing\n"); return 1; }

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    js_dom_init(ctx, root);

    run(ctx, URL_SHIM, "url shim");
    run(ctx, CHECKS, "checks");

    JSValue g = JS_GetGlobalObject(ctx);
    JSValue out = JS_GetPropertyStr(ctx, g, "__out");
    uint32_t n = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, out, "length");
    JS_ToUint32(ctx, &n, lenv);
    JS_FreeValue(ctx, lenv);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, out, i);
        const char *m = JS_ToCString(ctx, e);
        printf("FAIL %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        g_fails++;
    }
    JS_FreeValue(ctx, out);
    JS_FreeValue(ctx, g);

    /* The table has to have REACHED the prototypes. Without this a build where
     * js_reflect_install silently did nothing would report a clean run of zero
     * assertions -- which is the failure mode tools/audit_tests.py exists to
     * find, and it would be this file's fault, not the harness's. */
    extern int js_reflect_installed(void);
    int installed = js_reflect_installed();
    if (installed < 300) {
        printf("FAIL reflect_test: only %d accessors installed (expected 300+)\n",
               installed);
        g_fails++;
    }

    js_dom_cleanup(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    if (g_fails) {
        printf("reflect_test: %d FAILED\n", g_fails);
        return 1;
    }
    printf("reflect_test: ALL PASS (%d accessors installed)\n", installed);
    return 0;
}
