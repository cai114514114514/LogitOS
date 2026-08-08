/* selectors_test.c -- the query-side selector engine and DOMTokenList
 * (c/apps/browser/js_select.c + c/apps/browser/js_tokenlist.c).
 *
 * WHAT THIS MEASURES THAT NOTHING ELSE DOES. WPT measures these files far more
 * thoroughly than this ever will, and takes minutes to do it. This exists for
 * the other job a suite has to do: to be a thing that can be made to FAIL on
 * purpose, in seconds, so that the passing runs mean something.
 *
 * THE CONTROL IS CASE, AND IT IS CHOSEN BECAUSE IT IS INVISIBLE.
 * -DSELECT_CASE_SENSITIVE compares every NAME -- element names in type
 * selectors, attribute names in attribute selectors, the argument to
 * getElementsByTagName -- case-sensitively, and turns off the quirks-mode
 * folding of class and id. That is not a wrecking ball: it is what a careful
 * implementation looks like when nobody has told the author that HTML is
 * ASCII-case-insensitive about names. Every page written in lowercase markup
 * behaves EXACTLY as before. `div`, `.cls`, `#id`, `[data-x=1]`,
 * `getElementsByTagName('p')` -- all still right. What stops working is
 * `querySelector('DIV')`, `[ALIGN=left]` matching align="LEFT", and
 * `.Warning` matching class="warning" on a quirks-mode page.
 *
 * So group 1 below is the case rules and nothing else, and `make
 * test-selectors-negctl` passes only when THIS BINARY FAILS in that build.
 * Group 2 is the grammar and the token list: it must pass in both builds,
 * because an assertion that fails in both would let the control succeed for a
 * reason that has nothing to do with case.
 *
 * TWO DOCUMENTS, because quirks mode is a property of the document and not a
 * switch: the second page has no doctype, which is what puts html_tree.c into
 * QM_QUIRKS, and the class/id folding rules are asserted there against the
 * same selectors that must NOT fold on the first page.
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

/* Link stubs, the same set dom_iface_test.c carries and for the same reason:
 * this links the SHIPPING browser files, two of which reach for the fetcher
 * and the image registry, neither of which exists off the machine. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{ (void)ref; (void)out; (void)outlen; return 0; }

static int fails, checks, case_checks, case_fails;
static int in_case_group;
static JSContext *g_ctx;

static void ckjs(const char *expr, const char *what)
{
    checks++;
    if (in_case_group) case_checks++;
    JSValue v = JS_Eval(g_ctx, expr, strlen(expr), "<sel>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        printf("FAIL %s\n     threw: %s\n", what, m ? m : "?");
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        fails++; if (in_case_group) case_fails++;
    } else if (!JS_ToBool(g_ctx, v)) {
        printf("FAIL %s\n     expr: %s\n", what, expr);
        fails++; if (in_case_group) case_fails++;
    } else {
        printf("ok: %s\n", what);
    }
    JS_FreeValue(g_ctx, v);
}

/* A standards-mode document. The doctype is load-bearing: without it
 * html_tree.c reports QM_QUIRKS and half of group 1 inverts. */
static const char *HTML =
    "<!DOCTYPE html><html><head><title>t</title></head>"
    "<body id='b' class='x y'>"
    "<div id='d' class='Warning box' data-k='v' align='LEFT' lang='EN-us' title='hi'>text</div>"
    "<p id='p1' class='a  b   a'>one</p>"
    "<p id='p2'>two</p>"
    "<p id='p3'>three</p>"
    "<a id='lnk' href='/p' rel='NoFollow'>link</a>"
    "<input id='i1' type='checkbox' checked><input id='i2' type='text' disabled>"
    "<ul id='u'><li id='l1'>1</li><li id='l2' class='sel'>2</li><li id='l3'>3</li></ul>"
    "<span id='esc' class='a.b'></span><span id='q' data-v='a b-c'></span>"
    "<em id='empty'></em>"
    "</body></html>";

/* Quirks: no doctype at all. */
static const char *QUIRKS_HTML =
    "<html><body><div id='QuirkBox' class='Warning'>q</div>"
    "<div id='plain' class='lower'></div></body></html>";

static void group_case_standards(void)
{
    in_case_group = 1;
    printf("== group 1: CASE -- what the negative control must break ==\n");

    /* The headline. A type selector folds for an HTML element in an HTML
     * document, so all four spellings find the same <div>. */
    ckjs("document.querySelector('DIV') === document.getElementById('d') &&"
         "document.querySelector('Div') === document.getElementById('d') &&"
         "document.querySelector('div') === document.getElementById('d')",
         "a type selector folds: DIV / Div / div all find the div");
    ckjs("document.querySelectorAll('P').length === 3 &&"
         "document.querySelectorAll('p').length === 3",
         "and it folds in querySelectorAll too");
    ckjs("document.getElementById('d').matches('DIV') === true &&"
         "document.getElementById('l2').closest('UL') === document.getElementById('u')",
         "matches() and closest() fold the same way");

    /* getElementsByTagName: the argument is ASCII-lowercased before it is
     * compared against an HTML element's qualified name. */
    ckjs("document.getElementsByTagName('P').length === 3 &&"
         "document.getElementsByTagName('LI').length === 3 &&"
         "document.getElementsByTagName('li').length === 3",
         "getElementsByTagName folds its argument for HTML elements");

    /* Attribute NAMES fold; attribute VALUES do not, unless the attribute is
     * one of HTML's 44 or the selector carries a flag. Both halves are here
     * because getting one right and the other wrong is the usual outcome. */
    ckjs("document.querySelector('[DATA-K]') === document.getElementById('d') &&"
         "document.querySelector('[Data-K=v]') === document.getElementById('d')",
         "attribute NAMES fold");
    ckjs("document.querySelector('[data-k=V]') === null &&"
         "document.querySelector('[data-k=v]') === document.getElementById('d')",
         "attribute VALUES do not fold by default");
    ckjs("document.querySelector('[align=left]') === document.getElementById('d') &&"
         "document.querySelector('[ALIGN=left]') === document.getElementById('d')",
         "align is on HTML's case-insensitive list, so align='LEFT' matches [align=left]");
    ckjs("document.querySelector('[lang|=en]') === document.getElementById('d') &&"
         "document.querySelector(\"[rel~='nofollow']\") === document.getElementById('lnk')",
         "and so are lang and rel -- |= and ~= fold with them");

    /* Standards mode: class and id are case-SENSITIVE. This is the assertion
     * that pins the quirks half down: it must hold here and invert below. */
    ckjs("document.querySelector('.Warning') === document.getElementById('d') &&"
         "document.querySelector('.warning') === null",
         "class is case-SENSITIVE in standards mode");
    ckjs("document.querySelector('#d') !== null && document.querySelector('#D') === null",
         "id is case-SENSITIVE in standards mode");
    ckjs("document.getElementsByClassName('Warning').length === 1 &&"
         "document.getElementsByClassName('warning').length === 0",
         "getElementsByClassName is case-SENSITIVE in standards mode");
    ckjs("document.compatMode === 'CSS1Compat'",
         "document.compatMode reports standards mode");
    in_case_group = 0;
}

static void group_grammar(void)
{
    printf("\n== group 2: the grammar and the token list ==\n");

    /* The three shapes js_dom.c's three-selector stub answered null for. Each
     * one is a whole class of selector, and each was silently empty. */
    ckjs("document.querySelector('[data-k]') === document.getElementById('d')",
         "an attribute selector reaches document.querySelector at all");
    ckjs("document.querySelector('ul > li') === document.getElementById('l1') &&"
         "document.querySelectorAll('ul > li').length === 3",
         "a child combinator reaches document.querySelector");
    ckjs("document.querySelector('p.a') === document.getElementById('p1') &&"
         "document.querySelector('div.box') === document.getElementById('d')",
         "a compound (type + class) reaches document.querySelector");

    /* Combinators. */
    ckjs("document.querySelector('#l1 + li') === document.getElementById('l2') &&"
         "document.querySelectorAll('#l1 ~ li').length === 2 &&"
         "document.querySelectorAll('#u li').length === 3",
         "the four combinators");

    /* Attribute operators, each with a match and a near-miss. */
    ckjs("document.querySelector(\"[data-v^='a ']\") === document.getElementById('q') &&"
         "document.querySelector(\"[data-v$='b-c']\") === document.getElementById('q') &&"
         "document.querySelector(\"[data-v*=' b']\") === document.getElementById('q') &&"
         "document.querySelector(\"[data-v~='b-c']\") === document.getElementById('q') &&"
         /* |= is the one that surprises: data-v='a b-c' DOES match [data-v|='a b']
            because the value begins with the operand followed by '-'. The
            non-match has to be chosen so that neither branch of that rule
            fires. */
         "document.querySelector(\"[data-v|='a b']\") === document.getElementById('q') &&"
         "document.querySelector(\"[data-v|='b']\") === null &&"
         "document.querySelector(\"[data-v~='b']\") === null",
         "^= $= *= ~= |=");
    /* The empty value: ^= $= *= with '' match NOTHING, which is the rule most
     * often written as "match everything" by accident. */
    ckjs("document.querySelector(\"[data-v^='']\") === null &&"
         "document.querySelector(\"[data-v$='']\") === null &&"
         "document.querySelector(\"[data-v*='']\") === null",
         "an empty operand matches nothing, not everything");

    /* The i and s flags. */
    ckjs("document.querySelector('[data-k=V i]') === document.getElementById('d') &&"
         "document.querySelector('[data-k=V s]') === null &&"
         "document.querySelector('[align=left s]') === null",
         "the i flag forces folding on and the s flag forces it off");

    /* Escapes -- the whole reason ParentNode-querySelector-escapes.html was
     * 13/68. `.a\.b` is one class called "a.b", not ".a" descendant ".b". */
    ckjs("document.querySelector('.a\\\\.b') === document.getElementById('esc')",
         "a backslash escape in a class name");
    ckjs("document.querySelector('#\\\\64 ') === document.getElementById('d')",
         "a hex escape (\\64 = 'd') in an id");

    /* Structural pseudo-classes and An+B. */
    ckjs("document.querySelector('#u li:first-child') === document.getElementById('l1') &&"
         "document.querySelector('#u li:last-child') === document.getElementById('l3') &&"
         "document.querySelectorAll('#u li:nth-child(odd)').length === 2 &&"
         "document.querySelector('#u li:nth-child(2)') === document.getElementById('l2') &&"
         "document.querySelectorAll('#u li:nth-last-child(-n+2)').length === 2",
         ":first-child / :last-child / :nth-child(An+B)");
    ckjs("document.querySelector('#empty:empty') === document.getElementById('empty') &&"
         "document.querySelector('#u:empty') === null &&"
         "document.querySelector(':root') === document.documentElement",
         ":empty and :root");
    ckjs("document.querySelectorAll('#u li:nth-child(1 of .sel)').length === 1 &&"
         "document.querySelector('#u li:nth-child(1 of .sel)') === document.getElementById('l2')",
         ":nth-child(An+B of S) -- the selector picks the pool, then the index");

    /* :is / :where / :not / :has, all over selector LISTS. */
    ckjs("document.querySelectorAll(':is(#p1, #p2)').length === 2 &&"
         "document.querySelectorAll(':where(#p1, #p2)').length === 2 &&"
         "document.querySelectorAll('#u li:not(.sel)').length === 2 &&"
         "document.querySelectorAll('#u li:not(#l1, #l2)').length === 1",
         ":is / :where / :not take a selector list");
    ckjs("document.querySelector('ul:has(.sel)') === document.getElementById('u') &&"
         "document.querySelector('ul:has(> .sel)') === document.getElementById('u') &&"
         "document.querySelector('ul:has(.nope)') === null",
         ":has() runs forward, and takes a relative selector");

    /* Form-state pseudo-classes -- the ones with real state behind them. */
    ckjs("document.querySelector(':checked') === document.getElementById('i1') &&"
         "document.querySelector(':disabled') === document.getElementById('i2') &&"
         "document.querySelector('input:enabled') === document.getElementById('i1') &&"
         "document.querySelector(':any-link') === document.getElementById('lnk')",
         ":checked / :disabled / :enabled / :any-link");

    /* An unknown pseudo-class is a SyntaxError, not an empty result. An empty
     * result is indistinguishable from "no matches" and is how a selector bug
     * hides for a month. */
    ckjs("(function () { try { document.querySelector(':nonesuch'); return false; }"
         " catch (e) { return e.name === 'SyntaxError'; } })()",
         "an unknown pseudo-class throws SyntaxError");
    ckjs("(function () { var bad = ['', '>', 'div >', '[', '[a=]', '#', '.', 'a,,b', ':not(']; "
         " for (var i = 0; i < bad.length; i++) {"
         "   try { document.querySelector(bad[i]); return 'no throw for ' + JSON.stringify(bad[i]); }"
         "   catch (e) { if (e.name !== 'SyntaxError') return 'wrong error ' + e.name; } }"
         " return true; })() === true",
         "nine malformed selectors all throw SyntaxError");
    /* ...and a state-dependent pseudo-class we cannot evaluate is VALID and
     * matches nothing. Conflating the two makes every typo silent. */
    ckjs("document.querySelectorAll('a:hover').length === 0 &&"
         "document.querySelectorAll('a:visited').length === 0 &&"
         "document.querySelectorAll('div::before').length === 0",
         ":hover / :visited / ::before are valid and match nothing");

    /* Scope: a selector matches DESCENDANTS of the element, never the element. */
    ckjs("document.getElementById('u').querySelectorAll('li').length === 3 &&"
         "document.getElementById('u').querySelector('ul') === null &&"
         "document.getElementById('u').querySelector(':scope > li') === document.getElementById('l1')",
         "element.querySelectorAll searches descendants, and :scope is the element");

    /* The interfaces. */
    ckjs("document.querySelectorAll('p') instanceof NodeList &&"
         "document.getElementsByTagName('p') instanceof HTMLCollection &&"
         "document.getElementsByClassName('sel') instanceof HTMLCollection",
         "querySelectorAll is a NodeList, getElementsBy* are HTMLCollections");

    /* ---- DOMTokenList ---- */
    ckjs("(function () { var e = document.getElementById('p1');"
         " return e.classList.length === 2 && e.classList[0] === 'a' && e.classList[1] === 'b'"
         "     && e.classList.item(2) === null; })()",
         "classList is an ordered SET: class='a  b   a' has two tokens");
    ckjs("(function () { var e = document.createElement('div');"
         " e.setAttribute('class', '\\ra\\na\\tb\\f');"
         " return e.classList.contains('a') && e.classList.contains('b')"
         "     && e.classList.length === 2; })()",
         "all five ASCII whitespace characters separate tokens");
    ckjs("(function () { var e = document.createElement('div');"
         " try { e.classList.add(''); return false; } catch (x) { return x.name === 'SyntaxError'; } })()",
         "add('') throws SyntaxError");
    ckjs("(function () { var e = document.createElement('div');"
         " try { e.classList.add('a b'); return false; }"
         " catch (x) { return x.name === 'InvalidCharacterError'; } })()",
         "add('a b') throws InvalidCharacterError");
    ckjs("(function () { var e = document.createElement('div');"
         " e.setAttribute('class', 'a');"
         " try { e.classList.replace(' ', ''); } catch (x) {"
         "   return x.name === 'SyntaxError' && e.getAttribute('class') === 'a'; }"
         " return false; })()",
         "replace(' ', '') is a SyntaxError -- both empty checks run before either "
         "whitespace check -- and the attribute is untouched");
    ckjs("(function () { var e = document.createElement('div');"
         " e.setAttribute('class', 'a  b'); e.classList.add('a');"
         " return e.getAttribute('class') === 'a b'; })()",
         "a no-op add() still re-serialises the attribute");
    ckjs("(function () { var e = document.createElement('div');"
         " e.classList.remove('a'); return e.getAttribute('class') === null; })()",
         "remove() on an element with no class attribute does not create one");
    ckjs("(function () { var e = document.createElement('div');"
         " e.setAttribute('class', 'a b'); var r = e.classList.replace('b', 'a');"
         " return r === true && e.getAttribute('class') === 'a'; })()",
         "replace() is Infra's ordered-set replace, not remove-then-add");
    ckjs("(function () { var e = document.createElement('div');"
         " e.setAttribute('class', 'a  b'); var r = e.classList.toggle('a', true);"
         " return r === true && e.getAttribute('class') === 'a  b'; })()",
         "toggle(t, true) on a token already present writes nothing at all");
    ckjs("(function () { var e = document.createElement('div');"
         " var l = e.classList; e.setAttribute('class', 'z');"
         " return l.length === 1 && l[0] === 'z' && e.classList === l; })()",
         "the token list is LIVE, and the same object every time");
    ckjs("(function () { var e = document.createElement('div');"
         " try { e.classList.supports('a'); return false; } catch (x) { return x instanceof TypeError; } })()",
         "supports() is a TypeError: `class` has no supported-tokens list");
    ckjs("(function () { var e = document.createElement('div');"
         " e.classList = 'foo'; return e.getAttribute('class') === 'foo'; })()",
         "assigning to classList forwards to .value ([PutForwards])");
}

static void group_case_quirks(void)
{
    in_case_group = 1;
    printf("\n== group 1 (continued): CASE in a QUIRKS-mode document ==\n");
    ckjs("document.compatMode === 'BackCompat'",
         "a document with no doctype is in quirks mode");
    ckjs("document.querySelector('.Warning') !== null &&"
         "document.querySelector('.warning') !== null &&"
         "document.querySelector('.WARNING') !== null",
         "class folds in quirks mode: .warning matches class='Warning'");
    ckjs("document.querySelector('#quirkbox') !== null &&"
         "document.querySelector('#QUIRKBOX') !== null",
         "id folds in quirks mode");
    ckjs("document.getElementsByClassName('WARNING').length === 1",
         "getElementsByClassName folds in quirks mode");
    /* The half that must NOT change: folding class does not fold a VALUE. */
    ckjs("document.querySelector(\"[class='Warning']\") !== null &&"
         "document.querySelector(\"[class='warning']\") === null",
         "...but the class ATTRIBUTE's value still does not fold");
    in_case_group = 0;
}

int main(void)
{
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    if (!root) { printf("FAIL: parse\n"); return 1; }
    js_page_set_location("http://example.com/");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    g_ctx = js_page_ctx();
    /* One evaluation first: js_dom.c runs a compatibility bridge out of the job
     * queue, and a test that skipped it would be inspecting a state no page
     * ever sees. */
    js_page_eval("void 0;", 7, "<warmup>");

    group_case_standards();
    group_grammar();

    js_page_close();
    dom_free(root);

    /* Second document, quirks mode. */
    struct node *qroot = dom_parse(QUIRKS_HTML, (int)strlen(QUIRKS_HTML));
    if (!qroot) { printf("FAIL: parse (quirks)\n"); return 1; }
    js_page_set_location("http://example.com/q");
    if (!js_page_open(qroot)) { printf("FAIL: js_page_open (quirks)\n"); return 1; }
    g_ctx = js_page_ctx();
    js_page_eval("void 0;", 7, "<warmup>");
    group_case_quirks();
    js_page_close();
    dom_free(qroot);

    printf("\n%d checks (%d of them case rules), %d failed (%d case)\n",
           checks, case_checks, fails, case_fails);
    if (fails) { printf("SELECTORS TEST FAILED\n"); return 1; }
    printf("SELECTORS TEST PASSED\n");
    return 0;
}
