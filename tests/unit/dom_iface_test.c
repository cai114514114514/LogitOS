/* dom_iface_test.c -- the DOM interface hierarchy (c/apps/browser/js_dom_iface.inc).
 *
 * WHAT THIS IS FOR. js_dom.c used to publish one wrapper class with one flat
 * prototype, and js_platform.c layered a JS facade on top: the interface NAMES
 * existed, `instanceof` was answered by a Symbol.hasInstance that compared
 * tagName, and everything else about the shape of the platform was wrong --
 * `Object.getPrototypeOf(el)` was one shared object for every tag, patching
 * `HTMLDialogElement.prototype` affected nothing, and any interface not in that
 * hand-written table simply did not exist (a live site died on
 * `ReferenceError: HTMLBodyElement is not defined`).
 *
 * So every assertion here is about SHAPE, not about a method's answer: which
 * object is a wrapper's prototype, what the chain above it is, and whether the
 * constructors on globalThis are the ones at the ends of that chain. That is
 * exactly what a facade cannot fake, which is what makes this a test of the
 * hierarchy rather than a test of the DOM.
 *
 * NEGATIVE CONTROL: built again with -DJSDOM_NO_INTERFACE_HIERARCHY, which
 * keeps every constructor NAME and every member but installs the members flat
 * and leaves the constructors' prototypes unchained -- i.e. the facade. The
 * SHAPE assertions must then fail. `make test-dom-iface-negctl` passes only
 * when this binary fails in that build. Assertions that would fail in BOTH
 * builds would make the control meaningless, so the two groups are separated
 * below and counted separately.
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

/* Link stubs, the same set tests/unit/wpt_test.c carries and for the same
 * reason: this links the SHIPPING browser files (js_page/js_dom/js_webapi/
 * js_platform/js_select/js_module), and two of them reach for the fetcher and
 * the image registry, neither of which exists off the machine. A runner over
 * stubbed DOM files would measure the stubs; stubbing the network does not. */
__attribute__((__weak__)) void img_register(void *d) { (void)d; }
__attribute__((__weak__)) void img_register_anim(void *a, void *b, void *c)
{ (void)a; (void)b; (void)c; }
int bfetch_resolve(const char *base, const char *ref, char *out, int max)
{ (void)base; if (!ref || !out || max <= 0) return 0; snprintf(out, (size_t)max, "%s", ref); return 1; }
int bfetch_sync(const char *ref, unsigned char **out, int *outlen)
{ (void)ref; (void)out; (void)outlen; return 0; }

static int fails, checks;

static JSContext *g_ctx;

/* Evaluate an expression and require it to be true. */
static void ckjs(const char *expr, const char *what)
{
    checks++;
    JSValue v = JS_Eval(g_ctx, expr, strlen(expr), "<iface>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        printf("FAIL %s\n     threw: %s\n     expr: %s\n", what, m ? m : "?", expr);
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        fails++;
    } else if (!JS_ToBool(g_ctx, v)) {
        printf("FAIL %s\n     expr: %s\n", what, expr);
        fails++;
    } else {
        printf("ok: %s\n", what);
    }
    JS_FreeValue(g_ctx, v);
}

static const char *HTML =
    "<!DOCTYPE html><html><head><title>t</title></head>"
    "<body id='b' class='x y'>"
    "<div id='d' data-k='v' title='hi'>text<!--c--></div>"
    "<a id='a' href='/p'>link</a><input id='i'><table id='t'><tr><td>c</td></tr></table>"
    "<my-thing id='ce'></my-thing><section id='sec'></section>"
    "</body></html>";

int main(void)
{
    struct node *root = dom_parse(HTML, (int)strlen(HTML));
    if (!root) { printf("FAIL: parse\n"); return 1; }
    js_page_set_location("http://example.com/");
    if (!js_page_open(root)) { printf("FAIL: js_page_open\n"); return 1; }
    g_ctx = js_page_ctx();
    /* One evaluation so js_dom_run_jobs -- and with it the compatibility bridge
     * that moves js_select/js_platform's installs off HTMLDivElement.prototype
     * -- has run before anything below inspects a prototype. A page always gets
     * this; a test that skipped it would be measuring a state no page sees. */
    js_page_eval("void 0;", 7, "<warmup>");

    printf("== group 1: SHAPE -- what the negative control must break ==\n");

    /* ---- the constructors exist, by name, including the leaves ---- */
    ckjs("typeof EventTarget === 'function' && typeof Node === 'function' && "
         "typeof Element === 'function' && typeof HTMLElement === 'function' && "
         "typeof Document === 'function' && typeof DocumentFragment === 'function' && "
         "typeof CharacterData === 'function' && typeof Text === 'function' && "
         "typeof Comment === 'function' && typeof Attr === 'function' && "
         "typeof DOMException === 'function' && typeof NodeList === 'function' && "
         "typeof HTMLCollection === 'function' && typeof DOMTokenList === 'function'",
         "the core interface objects are all on globalThis");

    /* The one that took a live site down. It was absent from js_platform.c's
     * per-tag table, and absence is a ReferenceError, not a wrong answer. */
    ckjs("typeof HTMLBodyElement === 'function' && typeof HTMLHtmlElement === 'function' && "
         "typeof HTMLHeadElement === 'function' && typeof HTMLTableRowElement === 'function' && "
         "typeof HTMLTableCellElement === 'function' && typeof HTMLFieldSetElement === 'function' && "
         "typeof HTMLPictureElement === 'function' && typeof HTMLQuoteElement === 'function'",
         "HTMLBodyElement -- and seven other leaves the old table omitted -- exist");

    /* ---- the chain itself ---- */
    ckjs("Object.getPrototypeOf(HTMLDivElement.prototype) === HTMLElement.prototype && "
         "Object.getPrototypeOf(HTMLElement.prototype) === Element.prototype && "
         "Object.getPrototypeOf(Element.prototype) === Node.prototype && "
         "Object.getPrototypeOf(Node.prototype) === EventTarget.prototype && "
         "Object.getPrototypeOf(EventTarget.prototype) === Object.prototype",
         "HTMLDivElement -> HTMLElement -> Element -> Node -> EventTarget -> Object");

    ckjs("Object.getPrototypeOf(Text.prototype) === CharacterData.prototype && "
         "Object.getPrototypeOf(Comment.prototype) === CharacterData.prototype && "
         "Object.getPrototypeOf(CharacterData.prototype) === Node.prototype && "
         "Object.getPrototypeOf(Document.prototype) === Node.prototype && "
         "Object.getPrototypeOf(DocumentFragment.prototype) === Node.prototype",
         "CharacterData/Text/Comment/Document/DocumentFragment hang off Node");

    ckjs("Object.getPrototypeOf(HTMLVideoElement.prototype) === HTMLMediaElement.prototype && "
         "Object.getPrototypeOf(HTMLMediaElement.prototype) === HTMLElement.prototype",
         "HTMLVideoElement inherits HTMLMediaElement, not HTMLElement directly");

    /* ---- a live wrapper's prototype IS the leaf interface's ---- */
    ckjs("Object.getPrototypeOf(document.createElement('body')) === HTMLBodyElement.prototype",
         "createElement('body') gets HTMLBodyElement.prototype");
    ckjs("Object.getPrototypeOf(document.getElementById('d')) === HTMLDivElement.prototype && "
         "Object.getPrototypeOf(document.getElementById('a')) === HTMLAnchorElement.prototype && "
         "Object.getPrototypeOf(document.getElementById('i')) === HTMLInputElement.prototype && "
         "Object.getPrototypeOf(document.getElementById('t')) === HTMLTableElement.prototype",
         "parsed elements get their own leaf prototype, per tag");
    ckjs("Object.getPrototypeOf(document.createTextNode('x')) === Text.prototype && "
         "Object.getPrototypeOf(document.createComment('x')) === Comment.prototype && "
         "Object.getPrototypeOf(document.createDocumentFragment()) === DocumentFragment.prototype",
         "text, comment and fragment wrappers get theirs too");

    /* ---- instanceof, answered by the chain and not by a tagName compare ---- */
    ckjs("var b = document.createElement('body');"
         "b instanceof HTMLBodyElement && b instanceof HTMLElement && "
         "b instanceof Element && b instanceof Node && b instanceof EventTarget",
         "a <body> is an instanceof every interface above it");
    ckjs("!(document.createElement('div') instanceof HTMLInputElement) && "
         "!(document.createTextNode('x') instanceof Element) && "
         "!(document.createElement('div') instanceof Document)",
         "and NOT of interfaces it does not derive from");

    /* Two names the spec assigns by rule, not by table: an element the HTML
     * spec defines but gives no dedicated interface is a plain HTMLElement; an
     * element it does not define at all is an HTMLUnknownElement. Getting these
     * backwards is invisible until a page branches on it. */
    ckjs("Object.getPrototypeOf(document.getElementById('sec')) === HTMLElement.prototype",
         "<section> is a plain HTMLElement (defined, no dedicated interface)");
    ckjs("document.getElementById('ce') instanceof HTMLUnknownElement && "
         "document.getElementById('ce') instanceof HTMLElement",
         "<my-thing> is an HTMLUnknownElement, which is still an HTMLElement");

    /* ---- document is a real Node ---- */
    ckjs("document instanceof Document && document instanceof Node && "
         "document instanceof EventTarget && !(document instanceof Element)",
         "`document` is an instance of Document and of Node, and is not an Element");
    ckjs("document.documentElement.parentNode === document && "
         "document.body.ownerDocument === document",
         "and it is the SAME object the tree points at");

    /* ---- a patch of a per-tag prototype takes effect on a real element ----
     * This is the thing the facade structurally could not do: its per-tag
     * prototypes were fresh objects no element inherited from, so the patch
     * landed somewhere nothing could see it. */
    ckjs("HTMLAnchorElement.prototype.__probe = 41 + 1;"
         "document.getElementById('a').__probe === 42 && "
         "document.getElementById('d').__probe === undefined",
         "patching HTMLAnchorElement.prototype reaches <a> and only <a>");

    /* ---- collections ---- */
    ckjs("Object.getPrototypeOf(document.body.childNodes) === NodeList.prototype && "
         "Object.getPrototypeOf(document.body.children) === HTMLCollection.prototype && "
         "Object.getPrototypeOf(document.body.classList) === DOMTokenList.prototype",
         "childNodes / children / classList carry their own interface prototypes");

    /* ---- interface objects are non-enumerable and deletable (WPT tests both) ---- */
    ckjs("(function(){ for (var p in globalThis) if (p === 'Node' || p === 'Element' || "
         "p === 'HTMLBodyElement') return false; return true; })()",
         "interface objects are not enumerable on the global");

    /* Both of these are SHAPE dressed as members: an element only sees the
     * constants because Node.prototype is in its chain, and `t instanceof Text`
     * is the chain answering. They belong here, not below. */
    ckjs("document.body.ELEMENT_NODE === 1 && document.body.TEXT_NODE === 3",
         "an element inherits Node's constants through the chain");
    ckjs("new Text('hi') instanceof Text && new Text('hi') instanceof CharacterData && "
         "new Text('hi') instanceof Node",
         "a constructed Text is an instanceof Text, CharacterData and Node");

    /* A ProcessingInstruction only works when it has a prototype of its own:
     * its `data` and CharacterData's are different properties, and on one flat
     * prototype the second would overwrite the first for every text node. */
    ckjs("var p = document.createProcessingInstruction('xml-stylesheet', 'href=\"a\"');"
         "p.nodeType === 7 && p.target === 'xml-stylesheet' && p.data === 'href=\"a\"' && "
         "p instanceof ProcessingInstruction && p instanceof CharacterData && "
         "document.createTextNode('t').data === 't'",
         "createProcessingInstruction, without breaking Text.data");

    printf("== group 2: MEMBERS -- true in both builds, so not part of the control ==\n");

    /* The single largest item in the HTML-parser line's ranked failure list:
     * 232 of its 387 remaining subtests are this one property. */
    ckjs("document.getElementById('a').outerHTML === '<a id=\"a\" href=\"/p\">link</a>'",
         "outerHTML serialises the element itself");
    ckjs("var p = document.createElement('div');"
         "p.innerHTML = '<b>x</b>';"
         "p.firstChild.outerHTML = '<i>y</i>';"
         "p.innerHTML === '<i>y</i>'",
         "outerHTML= replaces the element in its parent");
    ckjs("var p = document.createElement('div');"
         "p.innerHTML = '<b>x</b>';"
         "p.firstChild.insertAdjacentHTML('beforebegin', '<u>A</u>');"
         "p.firstChild.insertAdjacentHTML('beforeend', '<s>B</s>');"
         "p.innerHTML === '<u>A<s>B</s></u><b>x</b>'",
         "insertAdjacentHTML, two of the four positions");

    ckjs("document.getElementById('d').tagName === 'DIV' && "
         "document.getElementById('d').nodeName === 'DIV' && "
         "document.createTextNode('x').nodeName === '#text' && "
         "document.createComment('x').nodeName === '#comment'",
         "tagName / nodeName are UPPERCASE for HTML elements and only for those");
    ckjs("var s = document.createElementNS('http://www.w3.org/2000/svg', 'clipPath');"
         "s.tagName === 'clipPath'",
         "and an SVG local name keeps its case");

    ckjs("var svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');"
         "var cp = document.createElementNS('http://www.w3.org/2000/svg', 'clipPath');"
         "svg.appendChild(cp);"
         "svg.getElementsByTagNameNS('http://www.w3.org/2000/svg', 'clipPath').length === 1 && "
         "svg.getElementsByTagNameNS('*', '*').length === 1 && "
         "svg.getElementsByTagNameNS('http://www.w3.org/1999/xhtml', 'clipPath').length === 0",
         "getElementsByTagNameNS, which did not exist at all");

    /* MEASURED on real sites: jQuery 3.5.1's $.parseHTML calls this
     * unconditionally, and without it anthropic.com does not load. */
    ckjs("var d2 = document.implementation.createHTMLDocument('T');"
         "d2.nodeType === 9 && d2.body && d2.head && "
         "d2.documentElement.tagName === 'HTML' && "
         "d2.body !== document.body && d2.head !== document.head",
         "document.implementation.createHTMLDocument builds its own html/head/body");
    ckjs("var d2 = document.implementation.createHTMLDocument('');"
         "d2.body.innerHTML = '<p id=\"moved\">hi</p>';"
         "var host = document.createElement('div');"
         "host.appendChild(d2.body.firstChild);"
         "host.innerHTML === '<p id=\"moved\">hi</p>'",
         "and a node parsed in it can be MOVED into the page (the point of it)");

    /* WINDOW NAMED ACCESS -- the largest single item the corpus ranks: ~480
     * files across dom/ and css/ die on a bare `ReferenceError: 'target' is not
     * defined` before their first assertion. */
    ckjs("typeof d === 'object' && d === document.getElementById('d') && "
         "typeof sec === 'object' && typeof t === 'object'",
         "an element's id is a global");
    ckjs("(function(){ var e = document.createElement('div'); e.id = 'lateName';"
         "document.body.appendChild(e);"
         "return globalThis.lateName === e; })()",
         "live: a node inserted later claims its name");
    /* Its own element: removing one the assertions above still name would make
     * this test order-dependent, which is a bug in the test and not the code. */
    ckjs("(function(){ var e = document.createElement('div'); e.id = 'goesAway';"
         "document.body.appendChild(e);"
         "var before = globalThis.goesAway;"
         "e.remove();"
         "return before === e && globalThis.goesAway === undefined; })()",
         "live: and the name stops resolving once the node leaves the document");
    ckjs("typeof document === 'object' && document.nodeType === 9 && "
         "typeof Node === 'function'",
         "a real global is never shadowed by an element name");
    ckjs("(function(){ var e = document.createElement('div'); e.id = 'shadowMe';"
         "document.body.appendChild(e);"
         "globalThis.shadowMe = 7;"
         "return globalThis.shadowMe === 7; })()",
         "an assignment to a named global replaces it permanently");

    ckjs("var e = document.createEvent ? document.createEvent('Event') : null;"
         "e === null || (function(){ e.initEvent('zap', true, true); return e.type === 'zap'; })()",
         "Event.type is settable, so initEvent needs no re-dispatch workaround");

    ckjs("Node.ELEMENT_NODE === 1 && Node.TEXT_NODE === 3 && Node.COMMENT_NODE === 8 && "
         "Node.DOCUMENT_NODE === 9 && Node.DOCUMENT_FRAGMENT_NODE === 11",
         "the nodeType constants as statics on Node");
    ckjs("Node.DOCUMENT_POSITION_FOLLOWING === 4 && Node.DOCUMENT_POSITION_CONTAINED_BY === 16",
         "the document-position constants too");

    ckjs("document.body.isConnected === true && "
         "document.createElement('div').isConnected === false",
         "isConnected");

    ckjs("var d = document.getElementById('d');"
         "var c = d.cloneNode(true);"
         "c !== d && c.tagName === d.tagName && c.getAttribute('data-k') === 'v' && "
         "c.parentNode === null && c.textContent === 'text' && "
         "d.cloneNode(false).firstChild === null",
         "cloneNode deep and shallow");

    ckjs("var d = document.getElementById('d'), a = document.getElementById('a');"
         "d.compareDocumentPosition(a) === 4 && a.compareDocumentPosition(d) === 2 && "
         "(d.compareDocumentPosition(d.firstChild) & 16) !== 0 && "
         "(d.firstChild.compareDocumentPosition(d) & 8) !== 0",
         "compareDocumentPosition names the direction AND the containment");

    ckjs("var d = document.getElementById('d');"
         "var n = d.getAttributeNames(); n.indexOf('id') >= 0 && n.indexOf('data-k') >= 0 && "
         "d.attributes.length === 3 && d.attributes['title'].value === 'hi' && "
         "d.attributes[0].nodeType === 2 && d.hasAttributes() === true && "
         "document.createElement('div').hasAttributes() === false",
         "getAttributeNames / attributes / hasAttributes");

    ckjs("var e = document.createElement('div');"
         "e.toggleAttribute('hidden') === true && e.hasAttribute('hidden') && "
         "e.toggleAttribute('hidden') === false && !e.hasAttribute('hidden') && "
         "e.toggleAttribute('hidden', false) === false && !e.hasAttribute('hidden') && "
         "e.toggleAttribute('hidden', true) === true && e.hasAttribute('hidden')",
         "toggleAttribute, including the two-argument form");

    ckjs("var p = document.createElement('div');"
         "p.append('a', document.createElement('span'), 'b');"
         "p.prepend('z');"
         "p.childNodes.length === 4 && p.firstChild.nodeValue === 'z' && "
         "p.childElementCount === 1",
         "append / prepend take nodes and strings, and childElementCount counts elements");

    ckjs("var p = document.createElement('div');"
         "var k = document.createElement('i'); p.appendChild(k);"
         "k.before('L'); k.after('R');"
         "p.childNodes.length === 3 && p.firstChild.nodeValue === 'L' && "
         "p.lastChild.nodeValue === 'R'",
         "before / after");

    ckjs("var p = document.createElement('div');"
         "var k = document.createElement('i'); p.appendChild(k);"
         "k.remove(); p.childNodes.length === 0",
         "remove()");

    ckjs("var p = document.createElement('div');"
         "p.append('a', 'b'); p.replaceChildren('c');"
         "p.childNodes.length === 1 && p.textContent === 'c'",
         "replaceChildren");

    ckjs("var t = document.createTextNode('abc');"
         "t.length === 3 && t.substringData(1, 2) === 'bc' && "
         "(t.appendData('d'), t.data === 'abcd')",
         "CharacterData length / substringData / appendData");

    ckjs("var e = new DOMException('boom', 'NotFoundError');"
         "e instanceof DOMException && e instanceof Error && e.name === 'NotFoundError' && "
         "e.message === 'boom' && e.code === 8 && DOMException.NOT_FOUND_ERR === undefined",
         "DOMException is constructible and is an Error");

    ckjs("new Text('hi').data === 'hi' && new Comment('c').data === 'c' && "
         "new DocumentFragment().nodeType === 11",
         "Text / Comment / DocumentFragment are constructible");

    ckjs("(function(){ try { new HTMLDivElement(); } catch (e) { return e instanceof TypeError; } "
         "return false; })()",
         "an element constructor called directly throws TypeError");

    /* THE SEAM. js_select.c and js_platform.c install onto
     * `Object.getPrototypeOf(document.createElement('div'))`, which this change
     * turned into HTMLDivElement.prototype. If the bridge did not run, every
     * one of these would be missing on every tag that is not a <div> -- a total
     * and silent regression, which is why it is asserted on an <a>. */
    ckjs("var a = document.getElementById('a');"
         "typeof a.querySelector === 'function' && typeof a.closest === 'function' && "
         "typeof a.matches === 'function' && typeof a.getElementsByTagName === 'function'",
         "seam: js_select.c's selector methods reach a non-div element");
    ckjs("var d = document.getElementById('d');"
         "typeof d.dataset === 'object' && d.dataset.k === 'v' && "
         "typeof document.getElementById('i').dataset === 'object'",
         "seam: js_platform.c's dataset reaches a non-div element");
    ckjs("document.getElementById('a').href === 'http://example.com/p'",
         "seam: js_platform.c's reflected .href reaches <a>");

    js_page_close();
    dom_free(root);

    printf("\n%d checks, %d failed\n", checks, fails);
    if (fails) { printf("DOM-IFACE TEST FAILED\n"); return 1; }
    printf("DOM-IFACE TEST PASSED\n");
    return 0;
}
