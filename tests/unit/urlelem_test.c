/* urlelem_test.c -- the URL Standard where it meets an element.
 *
 * WHAT THIS MEASURES THAT test-url DOES NOT. tests/url.mk drives
 * urltestdata.json straight into js_url.c's parser and has had it at 1201/1201
 * since it landed. That number says the ALGORITHM is right. It says nothing
 * about `<a href>`, which is a different question with three extra moving
 * parts, and all three were wrong while the parser was at 100%:
 *
 *   - the BASE. An element's href resolves against the document base URL --
 *     the <base> element's, not the document's own address. Nothing computed
 *     that, so every relative case resolved against the wrong thing.
 *   - the NULL URL. `protocol` must answer ":" when the href does not parse,
 *     because `href` deliberately echoes the literal back and that colon is
 *     the only signal a page gets. It answered "".
 *   - AGREEMENT. href and the ten components must describe the same URL. They
 *     came from two code paths, so they could disagree and did.
 *
 * So this harness is deliberately the corpus's own oracle: it does what
 * url/resources/a-element.js does -- set the <base>, create an <a>, setAttribute
 * the input, read all eleven properties -- against the SAME urltestdata.json,
 * plus setters_tests.json through the element rather than through `URL`. It
 * links the shipping browser files, not stubs, for the reason tests/wpt.mk
 * gives: a harness over stubs measures the stubs.
 *
 * NEGATIVE CONTROL: built again with -DURLELEM_SPLITTER, which swaps the
 * decomposition for a split on ':' '/' '?' '#' with reassembly by
 * concatenation -- see the block that implements it in js_urlbind.c for why
 * that is the honest control and not a straw man. `make test-urlelem-negctl`
 * requires this binary to FAIL in that build. A test that passes in both is
 * measuring neither.
 *
 * THE CORPUS IS OPTIONAL AND THE CAPABILITY IS NOT, the same rule tests/wpt.mk
 * states: with no checkout this prints why and exits 0, because a missing
 * corpus is not a regression in the code under test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "js_dom.h"
#include "js_page.h"

void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

/* The same link stubs tests/unit/dom_iface_test.c carries, for the same
 * reason: the shipping files reach for the fetcher and the image registry and
 * neither exists off the machine. Stubbing the network does not stub the DOM. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{ (void)ref; (void)out; (void)outlen; return 0; }

static unsigned long long fake_clock(void) { return 0; }

static char *slurp(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    char *p = (char *)malloc((size_t)n + 1);
    if (!p) { fclose(f); return 0; }
    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    p[got] = 0;
    if (len) *len = (int)got;
    return p;
}

/* The driver. It is JavaScript because the thing under test is a JavaScript
 * surface, and because running the corpus's own comparison rather than a
 * translation of it is the only way to be sure the oracle has not drifted. */
static const char *DRIVER =
"globalThis.__run = function () {\n"
"  var out = { pass: 0, fail: 0, first: '' };\n"
"  var el = function () { return document.createElement('a'); };\n"
"  function note(ok, what) {\n"
"    if (ok) { out.pass++; return; }\n"
"    out.fail++;\n"
"    if (!out.first) out.first = what;\n"
"  }\n"
"  function cmp(got, want, label, ctx) {\n"
"    if (got === want) return true;\n"
"    if (!out.first)\n"
"      out.first = ctx + ' -- ' + label + ': expected ' + JSON.stringify(want) +\n"
"                  ' got ' + JSON.stringify(got);\n"
"    return false;\n"
"  }\n"
   /* Part 1: urltestdata.json through <base> + <a>, i.e. a-element.js. */
"  var data = JSON.parse(__URLTESTDATA);\n"
"  for (var i = 0; i < data.length; i++) {\n"
"    var e = data[i];\n"
"    if (typeof e === 'string') continue;\n"
"    if (e.relativeTo === 'any-base') continue;\n"
"    if (e.base !== null && (e.base.indexOf('data:') === 0 ||\n"
"                            e.base.indexOf('javascript:') === 0)) continue;\n"
"    var base = e.base === null ? 'about:blank' : e.base;\n"
"    document.getElementById('base').setAttribute('href', base);\n"
"    var a = el();\n"
"    a.setAttribute('href', e.input);\n"
"    var ctx = '<' + e.input + '> against <' + base + '>';\n"
"    var ok = true;\n"
"    if (e.failure) {\n"
       /* The whole point of the ':' -- and the one thing a splitter can never
          produce, since to a splitter everything parses. */
"      ok = cmp(a.protocol, ':', 'protocol (failure)', ctx) && ok;\n"
"      ok = cmp(a.href, e.input, 'href (failure)', ctx) && ok;\n"
"    } else {\n"
"      ok = cmp(a.href, e.href, 'href', ctx) && ok;\n"
"      ok = cmp(a.protocol, e.protocol, 'protocol', ctx) && ok;\n"
"      ok = cmp(a.username, e.username, 'username', ctx) && ok;\n"
"      ok = cmp(a.password, e.password, 'password', ctx) && ok;\n"
"      ok = cmp(a.host, e.host, 'host', ctx) && ok;\n"
"      ok = cmp(a.hostname, e.hostname, 'hostname', ctx) && ok;\n"
"      ok = cmp(a.port, e.port, 'port', ctx) && ok;\n"
"      ok = cmp(a.pathname, e.pathname, 'pathname', ctx) && ok;\n"
"      ok = cmp(a.search, e.search, 'search', ctx) && ok;\n"
"      ok = cmp(a.hash, e.hash, 'hash', ctx) && ok;\n"
"    }\n"
"    note(ok, ctx);\n"
"  }\n"
   /* Part 2: setters_tests.json through the element. A setter that writes back
      through href is a different path from a setter on a URL object, and the
      corpus keeps a separate file for it (url-setters-a-area.window.js). */
"  var st = JSON.parse(__SETTERSDATA);\n"
"  for (var attr in st) {\n"
"    if (attr === 'comment') continue;\n"
"    var cases = st[attr];\n"
"    for (var j = 0; j < cases.length; j++) {\n"
"      var c = cases[j];\n"
"      var b = el();\n"
"      b.setAttribute('href', c.href);\n"
"      var ctx2 = 'set ' + attr + ' = ' + JSON.stringify(c.new_value) +\n"
"                 ' on <' + c.href + '>';\n"
"      try { b[attr] = c.new_value; } catch (ex) { note(false, ctx2 + ' threw ' + ex); continue; }\n"
"      var ok2 = true;\n"
"      for (var k in c.expected) ok2 = cmp(b[k], c.expected[k], k, ctx2) && ok2;\n"
"      note(ok2, ctx2);\n"
"    }\n"
"  }\n"
   /* Part 3: the base itself. Not from the corpus -- the corpus drives the
      base but never asks what it IS -- and it is the half of this work that
      lives outside the parser, so it gets its own assertions. */
"  function base_case(basehref, input, want, label) {\n"
"    var be = document.getElementById('base');\n"
"    if (basehref === null) be.removeAttribute('href');\n"
"    else be.setAttribute('href', basehref);\n"
"    var a = el();\n"
"    a.setAttribute('href', input);\n"
"    note(cmp(a.href, want, 'href', label), label);\n"
"  }\n"
"  base_case('http://example.org/one/two', 'three',\n"
"            'http://example.org/one/three', 'base: relative resolves against <base>');\n"
"  base_case('http://example.org/one/two', '/four',\n"
"            'http://example.org/four', 'base: absolute path keeps the base origin');\n"
"  base_case(null, 'five', 'http://logit.test/dir/five',\n"
"            'base: no <base> falls back to the document URL');\n"
"  base_case('http://example.org/a/b/../c/./d', 'e',\n"
"            'http://example.org/a/c/e', 'base: dot segments in the base are removed');\n"
"  base_case('http://EXAMPLE.org:80/x', 'y', 'http://example.org/y',\n"
"            'base: the base is normalized, not copied');\n"
"  var be2 = document.getElementById('base');\n"
"  be2.setAttribute('href', 'http://example.org/one/two');\n"
"  note(document.baseURI === 'http://example.org/one/two',\n"
"       'document.baseURI reports the <base>, got ' + document.baseURI);\n"
"  out.pass += 0;\n"
"  return out;\n"
"};\n";

static int getint(JSContext *ctx, JSValueConst o, const char *k)
{
    JSValue v = JS_GetPropertyStr(ctx, o, k);
    int32_t n = 0;
    JS_ToInt32(ctx, &n, v);
    JS_FreeValue(ctx, v);
    return (int)n;
}

int main(int argc, char **argv)
{
    const char *root = getenv("WPT_ROOT");
    if (argc > 1) root = argv[1];
    if (!root || !*root) root = "third_party/wpt";

    char p1[1024], p2[1024];
    snprintf(p1, sizeof p1, "%s/url/resources/urltestdata.json", root);
    snprintf(p2, sizeof p2, "%s/url/resources/setters_tests.json", root);
    int l1 = 0, l2 = 0;
    char *d1 = slurp(p1, &l1), *d2 = slurp(p2, &l2);
    if (!d1 || !d2) {
        free(d1); free(d2);
        printf("urlelem: no corpus at %s -- nothing to measure.\n", root);
        printf("urlelem: this is not a failure. Point WPT_ROOT at a checkout, or\n"
               "         run `make wpt-fetch` to vendor it.\n");
        return 0;
    }

    static const char *DOC =
        "<!doctype html><html><head><base id=base></head>"
        "<body><div id=log></div></body></html>";
    struct node *root_node = dom_parse(DOC, (int)strlen(DOC));
    if (!root_node) { printf("FAIL: dom_parse returned NULL\n"); return 1; }

    js_page_set_clock(fake_clock);
    js_page_set_location("http://logit.test/dir/page.html");
    if (!js_page_open(root_node)) { printf("FAIL: js_page_open\n"); return 1; }
    JSContext *ctx = js_page_ctx();

    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__URLTESTDATA", JS_NewStringLen(ctx, d1, (size_t)l1));
    JS_SetPropertyStr(ctx, g, "__SETTERSDATA", JS_NewStringLen(ctx, d2, (size_t)l2));
    JS_FreeValue(ctx, g);
    free(d1); free(d2);

    if (!js_page_eval(DRIVER, (int)strlen(DRIVER), "<driver>")) {
        printf("FAIL: the driver did not evaluate\n");
        js_page_close(); dom_free(root_node); return 1;
    }

    JSValue r = JS_Eval(ctx, "__run()", 7, "<run>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, e);
        printf("FAIL: the driver threw: %s\n", s ? s : "?");
        JS_FreeCString(ctx, s); JS_FreeValue(ctx, e); JS_FreeValue(ctx, r);
        js_page_close(); dom_free(root_node); return 1;
    }
    int pass = getint(ctx, r, "pass"), fail = getint(ctx, r, "fail");
    JSValue fv = JS_GetPropertyStr(ctx, r, "first");
    const char *first = JS_ToCString(ctx, fv);

    printf("urlelem: %d/%d cases pass (<a>/<area> URL decomposition, corpus %s)\n",
           pass, pass + fail, root);
    if (fail) printf("  first failure: %s\n", first ? first : "?");

    JS_FreeCString(ctx, first);
    JS_FreeValue(ctx, fv);
    JS_FreeValue(ctx, r);
    js_page_close();
    dom_free(root_node);

    /* 100%, not a floor. Every case here passes today, and a floor is how a
     * suite stops noticing the one that stopped. */
    if (fail) { printf("FAIL: %d cases\n", fail); return 1; }
    if (pass < 1000) {
        printf("FAIL: only %d cases ran -- the corpus did not load\n", pass);
        return 1;
    }
    printf("ok\n");
    return 0;
}
