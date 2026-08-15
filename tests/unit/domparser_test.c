/* Host gate for js_domparser.c: `new DOMParser().parseFromString(...)`.
 *
 * Two parts, following the same split test-dom-bindings/js_dom_test.c use for
 * a reason -- one part exercises the binding in isolation (no page runtime),
 * the other exercises it through the REAL js_page.c open/close lifecycle,
 * because the lifetime claim in js_domparser.c's header ("no crash after
 * js_page_close, because there is nothing left to reach a parsed document
 * through") can only be checked by actually opening and closing a page.
 *
 * PART A (bare context): parse a fragment, query it, read/write textContent,
 * traverse it, refuse XML loudly. Also the NEGATIVE CONTROL the task asked
 * for: `div.textContent = '...'` recycles `div`'s children back to the
 * document's free list (dom_destroy_children), so a JS wrapper held into a
 * child from BEFORE that write becomes a stale {node,serial} handle -- if
 * dp_of()'s serial check were missing or wrong, touching it afterwards reads
 * a recycled (and, after the next allocation, REUSED) `struct node` as if it
 * were still the `<span>`. The guard must turn that into "undefined", not a
 * wrong answer and not a crash; the test fails loudly if it doesn't.
 *
 * PART B (through js_page.c): open a real page runtime over a real page
 * document, run script that builds a DOMParser document, and check that
 * document.getElementById/querySelectorAll on the PAGE are unaffected by
 * anything the parsed document did (they are different `struct dom_doc`s
 * entirely, so this is really checking that this file never reaches for
 * js_dom.c's globals). Then js_page_close() with live JS references to the
 * parsed document still held in `globalThis`, and a SECOND js_page_open
 * afterwards, on a fresh document, to prove the process is still sound --
 * the sharpest place a bad finalizer (double free, wrong arena, dangling
 * dereference) would show up. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "js_page.h"

/* --- stubs: the kernel-only deps dom.c reaches for --- */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }

void js_domparser_install(JSContext *ctx);

static int fails, checks;
#define CK(c, m) do { checks++; if (!(c)) { printf("FAIL %s\n", m); fails = 1; } \
                      else printf("ok: %s\n", m); } while (0)

static JSRuntime *rt;
static JSContext *ctx;

static int run(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<t>", JS_EVAL_TYPE_GLOBAL);
    int ok = !JS_IsException(v);
    if (!ok) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("  JS exception: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, v);
    return ok;
}

static char *evalstr(const char *src)
{
    JSValue v = JS_Eval(ctx, src, strlen(src), "<t>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("  JS exception: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, v);
        return 0;
    }
    const char *s = JS_ToCString(ctx, v);
    char *out = s ? strdup(s) : 0;
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return out;
}

static int streq_eval(const char *src, const char *want)
{
    char *got = evalstr(src);
    int ok = got && strcmp(got, want) == 0;
    if (!ok) printf("  expr: %s\n  want: %s\n  got:  %s\n", src, want, got ? got : "(null)");
    free(got);
    return ok;
}

/* ================================ PART A ================================= */
static void part_a(void)
{
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    js_domparser_install(ctx);

    CK(run("var p = new DOMParser();"), "new DOMParser()");
    CK(run("if (typeof p.parseFromString !== 'function') throw 'no parseFromString';"),
       "parseFromString exists");

    CK(run("var doc = p.parseFromString("
           "  '<html><body><div id=\"a\" class=\"x y\"><span id=\"b\">hi</span>"
           "<p class=\"x\">there</p></div></body></html>', 'text/html');"),
       "parseFromString(text/html) does not throw");

    CK(run("if (!doc.documentElement || doc.documentElement.tagName !== 'HTML') throw 'bad documentElement';"),
       "documentElement is <html>, uppercased tagName");
    CK(run("if (!doc.body || doc.body.tagName !== 'BODY') throw 'bad body';"),
       "document.body resolves");
    CK(run("if (!doc.getElementById('a')) throw 'getElementById miss';"),
       "getElementById finds a parsed id");
    CK(streq_eval("doc.getElementById('a').id", "a"), "Element.id readback");
    CK(streq_eval("doc.getElementById('b').textContent", "hi"), "textContent read (leaf span)");
    CK(streq_eval("doc.getElementById('a').textContent", "hithere"), "textContent read (concatenated subtree)");
    CK(run("if (!doc.querySelector('#b')) throw 'qs #id miss';"), "querySelector('#id')");
    CK(run("if (!doc.querySelector('.x')) throw 'qs .class miss';"), "querySelector('.class') (word match, not prefix)");
    CK(run("if (doc.querySelectorAll('.x').length !== 2) throw 'qsa .x count wrong: ' + doc.querySelectorAll('.x').length;"),
       "querySelectorAll('.class') over multiple matches");
    CK(run("if (doc.querySelectorAll('div > span').length !== 1) throw 'child combinator wrong';"),
       "querySelectorAll('div > span') child combinator");
    CK(run("if (doc.querySelectorAll('body span').length !== 1) throw 'descendant combinator wrong';"),
       "querySelectorAll('body span') descendant combinator");
    CK(run("if (doc.querySelectorAll('p.x').length !== 1) throw 'compound tag.class wrong';"),
       "querySelectorAll('p.x') compound selector");
    CK(run("if (doc.querySelector('#nope') !== null) throw 'expected null';"),
       "querySelector on a missing id returns null, not throw");

    CK(run("var b = doc.getElementById('b'); if (b.parentNode.id !== 'a') throw 'parentNode wrong';"),
       "parentNode navigation");
    CK(run("if (doc.getElementById('a').firstElementChild.id !== 'b') throw 'firstElementChild wrong';"),
       "firstElementChild navigation");
    CK(run("if (doc.getElementById('b').nextElementSibling.tagName !== 'P') throw 'nextElementSibling wrong';"),
       "nextElementSibling navigation");
    CK(run("var kids = doc.getElementById('a').children; if (kids.length !== 2) throw 'children length wrong';"),
       "children snapshot length");
    CK(run("if (doc.getElementById('b').getAttribute('id') !== 'b') throw 'getAttribute wrong';"),
       "getAttribute");
    CK(run("if (!doc.getElementById('b').hasAttribute('id')) throw 'hasAttribute false negative';"),
       "hasAttribute");
    CK(run("if (doc.getElementById('a').hasAttribute('nope')) throw 'hasAttribute false positive';"),
       "hasAttribute (absent)");

    /* textContent WRITE, and its return trip. */
    CK(run("doc.getElementById('b').textContent = 'bye';"), "textContent write");
    CK(streq_eval("doc.getElementById('b').textContent", "bye"), "textContent write readback");

    /* --- the negative control: a stale handle across a structural mutation --- */
    CK(run("var span = doc.getElementById('b');"), "hold a wrapper into <div id=a>'s child");
    CK(run("doc.getElementById('a').textContent = 'replaced';"),
       "mutate the PARENT's textContent -- recycles <span> underneath the held wrapper");
    CK(run("if (span.parentNode !== undefined) throw 'stale handle: parentNode should be undefined, got a live answer';"),
       "STALE HANDLE CAUGHT: recycled node reads back as detached, not as live <span>");
    CK(run("if (span.tagName !== undefined) throw 'stale handle: tagName should be undefined';"),
       "STALE HANDLE CAUGHT: tagName off a recycled node is undefined, not 'SPAN'");
    CK(streq_eval("doc.getElementById('a').textContent", "replaced"),
       "the mutation itself took effect correctly");

    /* application/xml: never throws, refuses loudly via a parsererror doc. */
    CK(run("var xdoc = p.parseFromString('<a><b/></a>', 'application/xml'); "
           "if (!xdoc || !xdoc.documentElement) throw 'xml path produced nothing';"),
       "application/xml never throws");
    CK(run("if (!xdoc.querySelector('parsererror')) throw 'no <parsererror> in the refusal document';"),
       "application/xml refuses loudly via a <parsererror> document");

    /* The PAGE has no document in this bare-context part -- there is none to
     * touch -- so the cross-document isolation claim is proven in Part B. */

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

/* ================================ PART B ================================= */
static unsigned long long fake_clock(void) { return 0; }

static void part_b(void)
{
    static const char page_html[] =
        "<html><body><div id=\"page-only\">real page</div></body></html>";
    struct node *root = dom_parse(page_html, (int)(sizeof page_html - 1));
    CK(root != 0, "page document parses");

    js_page_set_clock(fake_clock);
    CK(js_page_open(root), "js_page_open");
    ctx = js_page_ctx();   /* run()/evalstr() above use the file-scope `ctx` */

    CK(run("if (typeof DOMParser === 'undefined') throw 'DOMParser missing from a real page runtime';"),
       "DOMParser is installed by js_page_open (js_page.c's ONE call)");

    CK(run("globalThis.__parsed = new DOMParser().parseFromString("
           "'<div id=\"only-in-parsed\">x</div>', 'text/html');"),
       "build a DOMParser document and root it in a global (outlives this statement)");
    CK(run("globalThis.__el = __parsed.getElementById('only-in-parsed');"),
       "hold a second, deeper wrapper into the same arena");

    CK(run("if (document.getElementById('only-in-parsed') !== null) "
           "throw 'PAGE document was contaminated by the parsed one';"),
       "the PAGE document does not see the parsed document's id");
    CK(run("if (document.getElementById('page-only') === null) "
           "throw 'the page document lost its own content';"),
       "the PAGE document still has its own content, untouched");
    CK(run("if (__parsed.getElementById('page-only') !== null) "
           "throw 'the PARSED document sees the page id -- documents are not actually separate';"),
       "the parsed document does not see the page's id either -- truly separate dom_docs");

    /* Close with live references to the parsed document (and a node inside
     * it) still held in globalThis -- exactly the shape that would crash on
     * a wrong lifetime scheme (freeing the arena while JS objects still
     * pointed into it, or a finalizer touching memory the runtime already
     * dropped). */
    js_page_close();
    printf("ok: js_page_close() with live DOMParser wrappers in globalThis did not crash\n");
    checks++;

    /* And the process is still sound: open a second, unrelated page runtime
     * afterwards and prove it works normally. If the first close's finalizers
     * corrupted anything (double free, wrong arena pointer), THIS is where a
     * heap corruption or an ASan build would say so. */
    static const char page2_html[] = "<html><body><p id=\"z\">second page</p></body></html>";
    struct node *root2 = dom_parse(page2_html, (int)(sizeof page2_html - 1));
    CK(js_page_open(root2), "a second js_page_open after the first close succeeds");
    ctx = js_page_ctx();
    CK(run("if (typeof DOMParser === 'undefined') throw 'DOMParser missing on second open';"),
       "DOMParser reinstalled cleanly on the second page");
    CK(run("var d2 = new DOMParser().parseFromString('<i id=\"w\">w</i>', 'text/html'); "
           "if (d2.getElementById('w').textContent !== 'w') throw 'second page DOMParser broken';"),
       "DOMParser still works correctly after a prior close/open cycle");
    js_page_close();
    dom_free(root2);

    dom_free(root);
}

int main(void)
{
    part_a();
    part_b();
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails;
}
