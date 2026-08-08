/* Host test for the NODE half of the JS<->DOM bindings (c/apps/browser/js_dom.c):
 * the surface react-dom's host config calls and that js_dom_test.c does not
 * cover -- createTextNode / createComment / createDocumentFragment /
 * createElementNS, insertBefore / replaceChild, the whole navigation family,
 * nodeType / nodeValue / ownerDocument / namespaceURI, removeAttribute /
 * hasAttribute / className, document.head, and getBoundingClientRect.
 *
 * Why a SECOND js binding test rather than more cases in js_dom_test.c: this
 * one links layout.c. getBoundingClientRect reads the display list, so the only
 * way to assert a real number out of it is to run the real cascade and the real
 * layout over a real document -- which is also the only way to check that the
 * geometry a script measures agrees with the geometry that gets painted.
 * js_dom_test.c deliberately links neither, and dragging layout (and the image
 * codecs behind it) into that link would blur what it is testing.
 *
 * Every mutating case asserts TWO things: that the DOM changed, and that the
 * invalidation record says so. A binding that mutates the tree without marking
 * a scope is worse than one that does not work at all -- the DOM is right, the
 * screen is stale, and nothing reports an error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "quickjs.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "js_dom.h"

/* --- stubs: the kernel-only deps layout.c and the DOM reach for --- */
void *kmalloc(unsigned long n) { return malloc(n); }
void  kfree(void *p) { free(p); }
int text_measure(const char *s, int len, int px, int mono)
{ (void)s; (void)mono; return len * (px / 2); }
int res_fetch(const char *url, uint8_t **buf, int *len)
{ (void)url; (void)buf; (void)len; return -1; }
void img_free(struct image *o) { (void)o; }
int img_decode(const uint8_t *p, int n, struct image *out)
{ (void)p; (void)n; (void)out; return -1; }

static int fails, checks;
#define CK(c, m) do { checks++; if (!(c)) { printf("FAIL %s\n", m); fails = 1; } \
                      else printf("ok: %s\n", m); } while (0)

static JSRuntime *rt;
static JSContext *ctx;
static struct node *g_root;

/* Evaluate; a thrown exception is itself the failure report, because every
 * assertion below is written as a `throw` inside the script. That keeps the
 * expected value next to the expression that produced it instead of in a C
 * mirror of the same logic. */
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

/* The value of an expression, as a C string the caller must free. */
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
    int ok = got && !strcmp(got, want);
    if (!ok) printf("  %s -> '%s' (wanted '%s')\n", src, got ? got : "<exception>", want);
    free(got);
    return ok;
}

static struct node *byid(const char *id) { return dom_get_element_by_id(g_root->doc, id); }


static const char *alltext(struct node *n, char *buf, int cap)
{
    int o = 0;
    buf[0] = 0;
    struct node *k = n;
    while (k) {
        if (k->type == N_TEXT && k->text) {
            int l = k->textlen;
            if (o + l < cap) { memcpy(buf + o, k->text, (size_t)l); o += l; buf[o] = 0; }
        }
        if (k->first_child) { k = k->first_child; continue; }
        while (k && k != n && !k->next) k = k->parent;
        if (!k || k == n) break;
        k = k->next;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* 1. node creation + types                                            */
/* ------------------------------------------------------------------ */
static void test_create(void)
{
    CK(run("var t = document.createTextNode('hello');"
           "var c = document.createComment('note');"
           "var f = document.createDocumentFragment();"
           "var e = document.createElement('span');"
           "var s = document.createElementNS('http://www.w3.org/2000/svg', 'clipPath');"),
       "createTextNode / createComment / createDocumentFragment / createElementNS all return");

    CK(streq_eval("[t.nodeType, c.nodeType, f.nodeType, e.nodeType, document.nodeType,"
                  " document.body.nodeType].join(',')", "3,8,11,1,9,1"),
       "nodeType: TEXT=3 COMMENT=8 FRAGMENT=11 ELEMENT=1 DOCUMENT=9");
    CK(streq_eval("[t.nodeName, c.nodeName, e.nodeName].join(',')", "#text,#comment,SPAN"),
       /* An HTML element name comes back UPPERCASE as of the interface-hierarchy
        * work; "#text" / "#comment" do not, which is the half worth asserting. */
       "nodeName of text / comment / element");
    CK(streq_eval("t.nodeValue", "hello"), "createTextNode's data reads back through nodeValue");
    CK(streq_eval("t.data", "hello"), "and through .data (CharacterData's spelling)");
    CK(streq_eval("t.textContent", "hello"), "and through textContent");
    CK(streq_eval("c.nodeValue", "note"), "createComment's data reads back");
    CK(streq_eval("String(e.nodeValue)", "null"), "an element's nodeValue is null, not \"\"");

    /* The SVG local name must survive VERBATIM: dom_create_element lowercases,
     * so a createElementNS that forwarded to it would produce 'clippath' and no
     * SVG renderer would ever match it. */
    CK(streq_eval("s.tagName", "clipPath"),
       "createElementNS keeps an SVG local name's case (clipPath, not clippath)");
    CK(streq_eval("s.namespaceURI", "http://www.w3.org/2000/svg"),
       "and reports the SVG namespace");
    CK(streq_eval("e.namespaceURI", "http://www.w3.org/1999/xhtml"),
       "createElement is in the HTML namespace");
    CK(streq_eval("document.createElementNS('http://www.w3.org/1999/xhtml','DIV').localName", "div"),
       /* Read through localName, because tagName now uppercases an HTML name
        * back again -- the STORED name being lowercase is the property. */
       "an HTML-namespace createElementNS still lowercases");
    CK(streq_eval("document.createElementNS('http://www.w3.org/2000/svg','svg:rect').tagName", "rect"),
       "a qualified name's prefix is dropped, the local name kept");

    /* createTextNode() with no argument: React creates the node first and
     * writes it afterwards, so this must not be an exception. */
    CK(streq_eval("document.createTextNode().nodeValue", ""),
       "createTextNode() with no argument makes an empty text node");
}

/* ------------------------------------------------------------------ */
/* 2. text nodes: appended, read, written, and REPAINTED               */
/* ------------------------------------------------------------------ */
static void test_text_nodes(void)
{
    js_dom_clear_dirty();
    CK(run("var h = document.getElementById('host');"
           "h.textContent = '';"
           "var tn = document.createTextNode('alpha');"
           "h.appendChild(tn);"),
       "a createTextNode node is accepted by appendChild");
    struct node *host = byid("host");
    char buf[256];
    CK(host && !strcmp(alltext(host, buf, sizeof buf), "alpha"),
       "the text reached the live DOM");
    CK(host && host->first_child && host->first_child->type == N_TEXT,
       "and it is a real N_TEXT node, not an element");
    CK(js_dom_inval_level() == INVAL_LAYOUT, "appending a text node asks for layout");

    /* The mutation React performs most often: rewriting the SAME text node. */
    js_dom_clear_dirty();
    CK(run("tn.nodeValue = 'beta-and-longer';"), "nodeValue= on a live text node");
    CK(host && !strcmp(alltext(host, buf, sizeof buf), "beta-and-longer"),
       "the rewritten data reached the live DOM");
    CK(js_dom_inval_level() == INVAL_LAYOUT,
       "rewriting a text node's data asks for LAYOUT (a longer string reflows)");
    CK(js_dom_inval_roots() == 1 && js_dom_inval_root(0, 0) == host,
       "and the marked scope is the text node's ELEMENT parent, not the whole document");

    /* Same node object, not a replacement: a wrapper held across the write
     * must still resolve, or React's textInstance goes stale after one update. */
    CK(streq_eval("[tn.parentNode === h, h.firstChild === tn, h.childNodes.length,"
                  " tn.nodeType, h.id].join(',')", "true,true,1,3,host"),
       "the text node's identity survives the rewrite");

    js_dom_clear_dirty();
    CK(run("tn.textContent = 'gamma';"), "textContent= on a text node writes its data");
    CK(host && !strcmp(alltext(host, buf, sizeof buf), "gamma"),
       "textContent= on a text node did NOT nest a text node inside a text node");
    CK(js_dom_inval_level() == INVAL_LAYOUT, "and marked layout");

    /* A DETACHED character-data node must not dirty anything: React writes text
     * before it inserts, and a whole-document re-style per created node is the
     * cost this deliberately avoids. */
    js_dom_clear_dirty();
    CK(run("var loose = document.createTextNode('x'); loose.nodeValue = 'y';"),
       "writing a detached text node");
    CK(js_dom_inval_level() == INVAL_NONE,
       "a detached text node's write dirties nothing (nothing on screen shows it)");
}

/* ------------------------------------------------------------------ */
/* 3. navigation                                                       */
/* ------------------------------------------------------------------ */
static void test_navigation(void)
{
    CK(run("var n = document.getElementById('nav');"), "nav fixture");
    CK(streq_eval("n.childNodes.length", "5"),
       "childNodes counts text nodes too (elem,text,elem,text,elem)");
    CK(streq_eval("n.children.length", "3"), "children counts only elements");
    CK(streq_eval("[n.firstChild.nodeType, n.lastChild.nodeType].join(',')", "1,1"),
       "firstChild / lastChild");
    CK(streq_eval("n.firstElementChild.id + ',' + n.lastElementChild.id", "a,c"),
       "firstElementChild / lastElementChild skip the text nodes");
    CK(streq_eval("document.getElementById('a').nextSibling.nodeType", "3"),
       "nextSibling reaches the whitespace text node between two elements");
    CK(streq_eval("document.getElementById('a').nextElementSibling.id", "b"),
       "nextElementSibling skips it");
    CK(streq_eval("document.getElementById('c').previousElementSibling.id", "b"),
       "previousElementSibling");
    CK(streq_eval("String(document.getElementById('a').previousElementSibling)", "null"),
       "previousElementSibling of the first element is null");
    CK(streq_eval("document.getElementById('b').parentNode === n", "true"), "parentNode");
    CK(streq_eval("document.getElementById('b').parentElement === n", "true"), "parentElement");

    /* The two identity properties a framework uses to decide whether a node is
     * still "its own": both must be the SAME object every time. */
    CK(streq_eval("n.ownerDocument === document", "true"), "ownerDocument === document");
    CK(streq_eval("document.documentElement.parentNode === document", "true"),
       "documentElement.parentNode is the document object itself");
    CK(streq_eval("String(document.documentElement.parentElement)", "null"),
       "and its parentElement is null (the document is not an element)");
    CK(streq_eval("n.firstChild.parentNode === n && n.firstChild === n.childNodes[0]", "true"),
       "wrappers are cached: the same node is the same JS object through any path");

    CK(streq_eval("n.contains(document.getElementById('b'))", "true"), "contains(descendant)");
    CK(streq_eval("n.contains(n)", "true"), "contains(self) -- inclusive, per the DOM");
    CK(streq_eval("document.getElementById('b').contains(n)", "false"), "contains(ancestor) is false");
    CK(streq_eval("n.hasChildNodes()", "true"), "hasChildNodes");
    CK(streq_eval("document.createElement('i').hasChildNodes()", "false"), "hasChildNodes on an empty element");
}

/* ------------------------------------------------------------------ */
/* 4. insertBefore / replaceChild / fragments                          */
/* ------------------------------------------------------------------ */
static void test_insertion(void)
{
    js_dom_clear_dirty();
    CK(run("var p = document.getElementById('ins'); p.textContent = '';"
           "var x = document.createElement('i'); x.id = 'x'; x.textContent = 'X';"
           "var y = document.createElement('i'); y.id = 'y'; y.textContent = 'Y';"
           "p.appendChild(x); p.insertBefore(y, x);"),
       "insertBefore(new, ref)");
    CK(streq_eval("Array.prototype.map.call(p.children, function(c){return c.id;}).join('')", "yx"),
       "the new node landed BEFORE the reference node");
    CK(js_dom_inval_level() == INVAL_LAYOUT, "insertBefore asks for layout");

    /* react-dom appends by calling insertBefore(parent, child, null). */
    CK(run("var z = document.createElement('i'); z.id = 'z'; p.insertBefore(z, null);"),
       "insertBefore(new, null) appends");
    CK(streq_eval("Array.prototype.map.call(p.children, function(c){return c.id;}).join('')", "yxz"),
       "a null reference node means 'at the end'");

    /* Moving an existing child is the same call; the DOM says it is a move. */
    CK(run("p.insertBefore(z, y);"), "insertBefore of a node already in the tree MOVES it");
    CK(streq_eval("Array.prototype.map.call(p.children, function(c){return c.id;}).join('')", "zyx"),
       "and it is not duplicated");
    CK(streq_eval("p.children.length", "3"), "still three children after the move");

    /* replaceChild */
    js_dom_clear_dirty();
    CK(run("var w = document.createElement('i'); w.id = 'w';"
           "var gone = p.replaceChild(w, document.getElementById('y'));"),
       "replaceChild(new, old)");
    CK(streq_eval("Array.prototype.map.call(p.children, function(c){return c.id;}).join('')", "zwx"),
       "the new node took the old one's POSITION");
    CK(streq_eval("String(document.getElementById('y'))", "null"),
       "the replaced node left the document");
    CK(js_dom_inval_level() == INVAL_LAYOUT, "replaceChild asks for layout");
    /* The documented deviation, asserted so it cannot regress silently into a
     * use-after-free: the removed node is recycled, so its wrapper reads stale
     * rather than dangling. */
    CK(streq_eval("String(gone.nodeType)", "undefined"),
       "KNOWN: replaceChild recycles the old node, so its wrapper goes stale (not dangling)");

    /* ---- fragments: the whole point is that the fragment does NOT enter ---- */
    js_dom_clear_dirty();
    CK(run("var q = document.getElementById('frag'); q.textContent = '';"
           "var f = document.createDocumentFragment();"
           "for (var i = 0; i < 3; i++) {"
           "  var d = document.createElement('b'); d.id = 'f' + i;"
           "  d.appendChild(document.createTextNode('T' + i));"
           "  f.appendChild(d);"
           "}"
           "if (f.childNodes.length !== 3) throw 'fragment did not collect: ' + f.childNodes.length;"
           "q.appendChild(f);"),
       "a fragment collects children and is then appended");
    struct node *q = byid("frag");
    char buf[256];
    CK(q && !strcmp(alltext(q, buf, sizeof buf), "T0T1T2"), "all three children moved into the DOM");
    CK(streq_eval("q.children.length", "3"), "the parent has three element children...");
    CK(streq_eval("f.childNodes.length", "0"), "...and the fragment is now empty, as the DOM says");
    CK(streq_eval("Array.prototype.map.call(q.children,function(c){return c.tagName;}).join(',')",
                  "B,B,B"),
       "the fragment itself did NOT become a child (no '#document-fragment' in the tree)");
    CK(js_dom_inval_level() == INVAL_LAYOUT && js_dom_inval_roots() == 1
       && js_dom_inval_root(0, 0) == q,
       "a fragment insert marks exactly the destination subtree -- filling the "
       "fragment claimed nothing, because a fragment is never in the document");

    /* insertBefore of a fragment, the same way React flushes a batch. */
    CK(run("var f2 = document.createDocumentFragment();"
           "var m = document.createElement('b'); m.id = 'm'; f2.appendChild(m);"
           "q.insertBefore(f2, q.firstChild);"),
       "insertBefore of a fragment");
    CK(streq_eval("q.children[0].id", "m"), "its children landed at the reference position");

    /* Hierarchy: a cycle would make every iterative tree walk in the engine
     * spin forever, so it must be refused rather than merely discouraged. */
    CK(streq_eval("String(document.getElementById('f0').appendChild(q))", "null"),
       "appending an ancestor into its own descendant is refused");
    CK(streq_eval("document.getElementById('f0').parentNode === q", "true"),
       "and the tree was not corrupted by the refusal");
}

/* ------------------------------------------------------------------ */
/* 4b. the shape react-dom actually commits in                         */
/* ------------------------------------------------------------------ */
static void test_detached_build(void)
{
    /* Build a whole subtree DETACHED and attach it in one call -- react-dom's
     * commit shape, and the one that exercises every binding added here at
     * once. Sixty-odd mutations happen before anything is connected, and only
     * the final attach may claim a scope. If each off-document mutation claimed
     * one the record would overflow JS_DOM_MAX_DIRTY and fall back to a
     * whole-document re-style on every commit -- and it would go unnoticed,
     * because the SCREEN would still be right. (js_dom_test.c's
     * measure_commit() puts numbers on what that costs: 6145 elements /
     * 7.14 ms per commit against 21 / 0.07 ms.) */
    CK(run("var mount = document.getElementById('mnt'); mount.textContent = '';"),
       "mount point emptied");
    js_dom_clear_dirty();                       /* that clear was a real mutation */
    CK(run("var frag = document.createDocumentFragment();"
           "for (var i = 0; i < 12; i++) {"
           "  var row = document.createElement('div');"
           "  row.className = 'row r' + i;"
           "  row.id = 'row' + i;"
           "  row.setAttribute('data-idx', String(i));"
           "  var label = document.createElement('span');"
           "  label.appendChild(document.createTextNode('cell ' + i));"
           "  row.appendChild(label);"
           "  frag.appendChild(row);"
           "}"),
       "twelve rows built entirely off-document");
    CK(js_dom_inval_level() == INVAL_NONE,
       "building off-document dirties NOTHING -- no scope is spent before the attach");

    CK(run("mount.appendChild(frag);"), "one appendChild attaches the batch");
    CK(js_dom_inval_level() == INVAL_LAYOUT, "the attach asks for layout");
    CK(js_dom_inval_roots() == 1 && js_dom_inval_root(0, 0) == byid("mnt"),
       "and claims exactly ONE scope: the mount point "
       "(zero roots would mean the record overflowed to the whole document)");

    struct node *mnt = byid("mnt");
    char buf[512];
    CK(mnt && strstr(alltext(mnt, buf, sizeof buf), "cell 11") != 0,
       "all twelve rows' text is in the live DOM");
    CK(streq_eval("mount.children.length", "12"), "twelve element children");
    CK(streq_eval("document.getElementById('row7').className", "row r7"),
       "el.id= entered the document's id index and className= stuck");
    CK(streq_eval("document.getElementById('row7').getAttribute('data-idx')", "7"),
       "and the attributes came with it");
    CK(streq_eval("document.getElementById('row7').firstChild.firstChild.nodeValue", "cell 7"),
       "the text nodes are reachable by navigation, two levels down");
}

/* ------------------------------------------------------------------ */
/* 5. attributes + className                                           */
/* ------------------------------------------------------------------ */
static void test_attributes(void)
{
    js_dom_clear_dirty();
    CK(run("var a = document.getElementById('attr');"
           "a.setAttribute('data-state', 'on');"),
       "setAttribute");
    CK(streq_eval("a.hasAttribute('data-state')", "true"), "hasAttribute finds it");
    CK(streq_eval("a.hasAttribute('data-nope')", "false"), "hasAttribute is false for an absent one");

    js_dom_clear_dirty();
    CK(run("a.removeAttribute('data-state');"), "removeAttribute");
    CK(streq_eval("a.hasAttribute('data-state')", "false"), "hasAttribute now says absent");
    CK(streq_eval("String(a.getAttribute('data-state'))", "null"), "getAttribute now returns null");
    struct node *an = byid("attr");
    CK(an && dom_attr(an, "data-state") == 0,
       "and the DOM reports it ABSENT, not present-with-an-empty-value "
       "(an [attr] selector must stop matching)");
    CK(js_dom_inval_level() == INVAL_STYLE,
       "removeAttribute asks for a re-style: any attribute can be a selector input");

    /* Removing `id` has to un-index the node, or getElementById keeps
     * answering with an element that no longer carries the id. */
    CK(run("var r = document.getElementById('rmid');"), "id-removal fixture");
    CK(run("r.removeAttribute('id');"), "removeAttribute('id')");
    CK(streq_eval("String(document.getElementById('rmid'))", "null"),
       "the document's id index dropped the element too");
    CK(byid("rmid") == 0, "and dom_get_element_by_id agrees from C");

    /* className: the property React actually writes. */
    js_dom_clear_dirty();
    CK(run("a.className = 'one two';"), "className=");
    CK(streq_eval("a.className", "one two"), "className reads back");
    CK(streq_eval("a.getAttribute('class')", "one two"), "and it IS the class attribute");
    CK(streq_eval("[a.classList.length, a.classList.contains('two')].join(',')", "2,true"),
       "classList sees what className wrote");
    CK(js_dom_inval_level() == INVAL_STYLE, "className= asks for a re-style");

    js_dom_clear_dirty();
    CK(run("a.className = 'one two';"), "className= with the identical value");
    CK(js_dom_inval_level() == INVAL_NONE,
       "a no-op className write dirties nothing (the arena is bump-allocated: "
       "re-writing per frame would grow it forever)");

    js_dom_clear_dirty();
    CK(run("a.classList.add('three'); a.className;"), "classList.add after className=");
    CK(streq_eval("a.className", "one two three"), "the two spellings share one writer");

    CK(run("a.removeAttribute('class');"), "removeAttribute('class')");
    CK(streq_eval("a.className", ""), "className is empty after the attribute is removed");
    CK(streq_eval("a.classList.length", "0"), "and so is classList");
    struct node *an2 = byid("attr");
    CK(an2 && an2->nclass == 0, "the interned class list was cleared in the DOM too");
}

/* ------------------------------------------------------------------ */
/* 6. document.head                                                    */
/* ------------------------------------------------------------------ */
static void test_head(void)
{
    CK(streq_eval("document.head.tagName", "HEAD"), "document.head");
    CK(streq_eval("document.head.parentNode === document.documentElement", "true"),
       "and it is the document element's child");
    CK(run("var st = document.createElement('style'); document.head.appendChild(st);"),
       "appending to document.head works (this is how a CSS-in-JS runtime installs styles)");
    CK(streq_eval("document.head.lastElementChild.tagName", "STYLE"),
       "the style element is in the head");
}

/* ------------------------------------------------------------------ */
/* 7. getBoundingClientRect -- against the REAL layout                 */
/* ------------------------------------------------------------------ */
static void test_rect(void)
{
    /* A box that paints its own background, so layout emits an IT_RECT for the
     * whole border box and the rect is exact rather than ink bounds. */
    css_apply(g_root, 0, 0);
    layout_page(g_root, 400);

    /* Find what layout actually produced for #box, straight out of the display
     * list, and require the binding to agree with it. Two independent readings
     * of the same fact: if they disagree, the binding is wrong, and if they
     * agree but are both absurd, the geometry assertions below catch it. */
    struct node *box = byid("box");
    const struct item *it = layout_items();
    int n = layout_count(), fx = -1, fy = -1, fw = -1, fh = -1;
    for (int i = 0; i < n; i++)
        if (it[i].node == box && it[i].type == IT_RECT)
        { fx = it[i].x; fy = it[i].y; fw = it[i].w; fh = it[i].h; break; }
    CK(fx >= 0, "layout emitted a background rect for #box");

    CK(run("var r = document.getElementById('box').getBoundingClientRect();"),
       "getBoundingClientRect returns");
    char q[128];
    snprintf(q, sizeof q, "%d,%d,%d,%d", fx, fy, fw, fh);
    CK(streq_eval("[r.x, r.y, r.width, r.height].join(',')", q),
       "the rect matches the box layout painted, to the pixel");
    CK(streq_eval("[r.left, r.top].join(',') === [r.x, r.y].join(',')", "true"),
       "left/top alias x/y");
    CK(streq_eval("[r.right - r.left, r.bottom - r.top].join(',') === "
                  "[r.width, r.height].join(',')", "true"),
       "right/bottom are left+width / top+height");
    CK(streq_eval("typeof r.toJSON", "function"), "the rect has toJSON, like a DOMRect");
    CK(fw == 300, "and the width is the CSS width (300px), which is the value under test");

    /* The scroll offset turns document coordinates into client coordinates.
     * This is the hook the embedder pushes browser.c's `scroll` through; with
     * it unset the two coincide, which is what every case above assumed. */
    js_dom_set_scroll(0, 40);
    CK(run("var r2 = document.getElementById('box').getBoundingClientRect();"),
       "getBoundingClientRect after a scroll");
    snprintf(q, sizeof q, "%d", fy - 40);
    CK(streq_eval("String(r2.y)", q), "a scrolled page reports CLIENT coordinates (y - scroll)");
    CK(streq_eval("r2.height === r.height && r2.width === r.width", "true"),
       "and scrolling moves the box without resizing it");
    js_dom_set_scroll(0, 0);

    /* A detached element has no box, and the DOM says every field is zero --
     * NOT an exception, because measuring before insertion is a normal thing
     * for a library to do. */
    CK(streq_eval("(function(){var d = document.createElement('div');"
                  "var q = d.getBoundingClientRect();"
                  "return [q.x,q.y,q.width,q.height].join(',');})()", "0,0,0,0"),
       "a detached element measures as an all-zero rect, not an exception");

    layout_free();
}

int main(void)
{
    /* One document for the whole run: these bindings are about a LIVE tree, and
     * building a fresh one per case would hide exactly the bugs (stale index
     * entries, recycled slots, cached wrappers) that only appear on the second
     * mutation of the same document. */
    const char *html =
        "<!doctype html><html><head><title>t</title></head><body>"
        "<div id='host'>seed</div>"
        "<div id='nav'><i id='a'>A</i> <i id='b'>B</i> <i id='c'>C</i></div>"
        "<div id='ins'>seed</div>"
        "<div id='frag'>seed</div>"
        "<div id='attr'>seed</div>"
        "<div id='rmid'>seed</div>"
        "<div id='mnt'>seed</div>"
        "<div id='box' style='width:300px;height:50px;background:#123456'></div>"
        "</body></html>";
    g_root = dom_parse(html, (int)strlen(html));
    if (!g_root) { printf("FAIL: fixture did not parse\n"); return 1; }

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    js_dom_init(ctx, g_root);

    test_create();
    test_text_nodes();
    test_navigation();
    test_insertion();
    test_detached_build();
    test_attributes();
    test_head();
    test_rect();

    js_dom_cleanup(ctx);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    dom_free(g_root);

    if (fails) printf("\ndom_bindings_test: FAILURES (%d checks run)\n", checks);
    else       printf("\ndom_bindings_test: ALL PASS (%d checks)\n", checks);
    return fails;
}
