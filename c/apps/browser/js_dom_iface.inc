/* js_dom_iface.inc -- the DOM interface hierarchy, and the globals that name it.
 *
 * INCLUDED BY js_dom.c, not compiled on its own. That is deliberate and not
 * laziness: tests/wpt.mk, tests/loader.mk, tests/webapi_platform.mk and eight
 * Makefile rules each list the browser's sources by hand, and three of those
 * files belong to other lines. A new translation unit would have to be added to
 * every one of them in the same commit or the build breaks for whoever pulls
 * next -- including `make test-wpt-negctl`, the target that measures this very
 * change. An `.inc` is the pattern this tree already uses for exactly that
 * reason (html_tags.inc, roots_bundle.inc), and -MMD tracks it, so editing this
 * file rebuilds js_dom.o.
 *
 * WHAT WAS HERE BEFORE. js_dom.c had ONE wrapper class with ONE prototype
 * carrying every member, and js_platform.c layered a JS-level facade over it:
 * `Node`, `Element`, `HTMLElement` all resolved to that single shared object,
 * and each per-tag name got a fresh empty prototype plus a `Symbol.hasInstance`
 * that string-compared tagName. That facade answers `x instanceof
 * HTMLInputElement` and nothing else. It cannot answer
 * `Object.getPrototypeOf(el)`, it cannot make a patch of
 * `HTMLDialogElement.prototype` take effect on a real <dialog>, and it does not
 * publish the names it does not list -- which is how a live site died on
 * `ReferenceError: HTMLBodyElement is not defined`: `body` simply was not in
 * that table. js_platform.c's own comment names the fix and its owner: "Fixing
 * that properly needs per-tag classes in js_dom.c, which is the DOM line's
 * file." This is that.
 *
 * WHAT IT IS NOW. Real prototype objects, chained
 *
 *   Object.prototype
 *     EventTarget.prototype
 *       Node.prototype                       (+ the nodeType constants)
 *         Document.prototype                 (`document` is an instance)
 *         DocumentFragment.prototype
 *         DocumentType.prototype
 *         CharacterData.prototype
 *           Text.prototype  Comment.prototype  CDATASection.prototype
 *         Attr.prototype
 *         Element.prototype
 *           SVGElement.prototype -> SVGSVGElement.prototype
 *           HTMLElement.prototype
 *             HTMLDivElement.prototype, HTMLBodyElement.prototype, ... (68)
 *               HTMLVideoElement/HTMLAudioElement under HTMLMediaElement
 *
 * and wrap() gives a new wrapper the prototype its node's kind and tag name
 * select. So `instanceof` and `Object.getPrototypeOf` are answered by the
 * language, with no hasInstance trick, and a page that patches a per-tag
 * prototype patches the object its elements actually inherit from.
 *
 * THE SEAM WITH THE OTHER LINES, stated plainly because it is the one hazard
 * this change creates. js_select.c, js_platform.c and js_media.c all reach the
 * shared element prototype the only way they could before this existed:
 *
 *     EP = Object.getPrototypeOf(document.createElement('div'))
 *
 * That used to be the prototype EVERY element shared. With a real hierarchy it
 * is HTMLDivElement.prototype, so querySelector, matches, closest, dataset,
 * getRootNode and the MutationObserver hooks would land on <div> and on nothing
 * else -- a silent, total regression on every other tag. Those three files are
 * not this line's to edit, so iface_bridge() below moves anything that lands
 * there down onto HTMLElement.prototype once, after the installers have run. It
 * is a compatibility shim with an expiry date: the day those files ask for
 * `HTMLElement.prototype` -- which now EXISTS, which it did not when they were
 * written -- the bridge can be deleted with nothing else changing.
 *
 * NEGATIVE CONTROL: -DJSDOM_NO_INTERFACE_HIERARCHY keeps every member and every
 * constructor NAME but installs the members flat on one prototype and leaves
 * each constructor's prototype unchained -- i.e. exactly the facade this
 * replaced. `make test-wpt-negctl` and `make test-dom-iface-negctl` require the
 * suite to fail in that build.
 */

/* ---------------------------------------------------------------------------
 * 1. The interface table
 * ------------------------------------------------------------------------- */

enum {
    IF_EVENTTARGET = 0,
    IF_NODE,
    IF_DOCUMENT,
    IF_DOCUMENTFRAGMENT,
    IF_DOCUMENTTYPE,
    IF_CHARACTERDATA,
    IF_TEXT,
    IF_COMMENT,
    IF_CDATASECTION,
    IF_PROCESSINGINSTRUCTION,
    IF_ATTR,
    IF_ELEMENT,
    IF_SVGELEMENT,
    IF_SVGSVGELEMENT,
    IF_HTMLELEMENT,
    IF_HTMLUNKNOWNELEMENT,
    IF_HTMLMEDIAELEMENT,

    /* per-tag HTML interfaces, all under HTMLElement unless noted */
    IF_HTMLHTML, IF_HTMLHEAD, IF_HTMLTITLE, IF_HTMLBASE, IF_HTMLLINK,
    IF_HTMLMETA, IF_HTMLSTYLE, IF_HTMLBODY, IF_HTMLHEADING, IF_HTMLPARAGRAPH,
    IF_HTMLHR, IF_HTMLPRE, IF_HTMLQUOTE, IF_HTMLOLIST, IF_HTMLULIST,
    IF_HTMLMENU, IF_HTMLLI, IF_HTMLDLIST, IF_HTMLDIV, IF_HTMLANCHOR,
    IF_HTMLDATA, IF_HTMLTIME, IF_HTMLSPAN, IF_HTMLBR, IF_HTMLMOD,
    IF_HTMLPICTURE, IF_HTMLSOURCE, IF_HTMLIMAGE, IF_HTMLIFRAME, IF_HTMLEMBED,
    IF_HTMLOBJECT, IF_HTMLPARAM, IF_HTMLVIDEO, IF_HTMLAUDIO, IF_HTMLTRACK,
    IF_HTMLMAP, IF_HTMLAREA, IF_HTMLTABLE, IF_HTMLTABLECAPTION,
    IF_HTMLTABLECOL, IF_HTMLTABLESECTION, IF_HTMLTABLEROW, IF_HTMLTABLECELL,
    IF_HTMLFORM, IF_HTMLLABEL, IF_HTMLINPUT, IF_HTMLBUTTON, IF_HTMLSELECT,
    IF_HTMLDATALIST, IF_HTMLOPTGROUP, IF_HTMLOPTION, IF_HTMLTEXTAREA,
    IF_HTMLOUTPUT, IF_HTMLPROGRESS, IF_HTMLMETER, IF_HTMLFIELDSET,
    IF_HTMLLEGEND, IF_HTMLDETAILS, IF_HTMLDIALOG, IF_HTMLSCRIPT,
    IF_HTMLTEMPLATE, IF_HTMLSLOT, IF_HTMLCANVAS, IF_HTMLFONT,
    IF_HTMLDIRECTORY, IF_HTMLMARQUEE, IF_HTMLFRAME, IF_HTMLFRAMESET,

    /* named but never a node's prototype */
    IF_NODELIST, IF_HTMLCOLLECTION, IF_NAMEDNODEMAP, IF_DOMTOKENLIST,
    IF_DOMSTRINGMAP, IF_DOMEXCEPTION, IF_DOMIMPLEMENTATION,

    IF__COUNT
};

/* parent == -1 means Object.prototype. `tags` is a space-separated list of the
 * element names whose wrapper gets this prototype; NULL for an interface that
 * is only ever a name (or whose instances are made some other way). */
struct iface_def { const char *name; short parent; const char *tags; };

static const struct iface_def IFACES[IF__COUNT] = {
 [IF_EVENTTARGET]       = { "EventTarget",       -1,                 0 },
 [IF_NODE]              = { "Node",              IF_EVENTTARGET,     0 },
 [IF_DOCUMENT]          = { "Document",          IF_NODE,            0 },
 [IF_DOCUMENTFRAGMENT]  = { "DocumentFragment",  IF_NODE,            0 },
 [IF_DOCUMENTTYPE]      = { "DocumentType",      IF_NODE,            0 },
 [IF_CHARACTERDATA]     = { "CharacterData",     IF_NODE,            0 },
 [IF_TEXT]              = { "Text",              IF_CHARACTERDATA,   0 },
 [IF_COMMENT]           = { "Comment",           IF_CHARACTERDATA,   0 },
 [IF_CDATASECTION]      = { "CDATASection",      IF_TEXT,            0 },
 [IF_PROCESSINGINSTRUCTION] = { "ProcessingInstruction", IF_CHARACTERDATA, 0 },
 [IF_ATTR]              = { "Attr",              IF_NODE,            0 },
 [IF_ELEMENT]           = { "Element",           IF_NODE,            0 },
 [IF_SVGELEMENT]        = { "SVGElement",        IF_ELEMENT,         0 },
 [IF_SVGSVGELEMENT]     = { "SVGSVGElement",     IF_SVGELEMENT,      0 },

 /* Every HTML element the spec does NOT give a dedicated interface hangs
  * directly off HTMLElement. The list is the spec's, not a guess: getting it
  * wrong the other way (calling <section> an HTMLUnknownElement) is exactly the
  * failure mode this table exists to avoid. */
 [IF_HTMLELEMENT]       = { "HTMLElement",       IF_ELEMENT,
   "abbr acronym address article aside b basefont bdi bdo bgsound big center "
   "cite code dd dfn dt em figcaption figure footer header hgroup i kbd main "
   "mark nav nobr noembed noframes noscript rb rp rt rtc ruby s samp search "
   "section small strike strong sub summary sup tt u var wbr" },
 [IF_HTMLUNKNOWNELEMENT]= { "HTMLUnknownElement", IF_HTMLELEMENT,    0 },
 [IF_HTMLMEDIAELEMENT]  = { "HTMLMediaElement",  IF_HTMLELEMENT,     0 },

 [IF_HTMLHTML]      = { "HTMLHtmlElement",       IF_HTMLELEMENT, "html" },
 [IF_HTMLHEAD]      = { "HTMLHeadElement",       IF_HTMLELEMENT, "head" },
 [IF_HTMLTITLE]     = { "HTMLTitleElement",      IF_HTMLELEMENT, "title" },
 [IF_HTMLBASE]      = { "HTMLBaseElement",       IF_HTMLELEMENT, "base" },
 [IF_HTMLLINK]      = { "HTMLLinkElement",       IF_HTMLELEMENT, "link" },
 [IF_HTMLMETA]      = { "HTMLMetaElement",       IF_HTMLELEMENT, "meta" },
 [IF_HTMLSTYLE]     = { "HTMLStyleElement",      IF_HTMLELEMENT, "style" },
 [IF_HTMLBODY]      = { "HTMLBodyElement",       IF_HTMLELEMENT, "body" },
 [IF_HTMLHEADING]   = { "HTMLHeadingElement",    IF_HTMLELEMENT, "h1 h2 h3 h4 h5 h6" },
 [IF_HTMLPARAGRAPH] = { "HTMLParagraphElement",  IF_HTMLELEMENT, "p" },
 [IF_HTMLHR]        = { "HTMLHRElement",         IF_HTMLELEMENT, "hr" },
 [IF_HTMLPRE]       = { "HTMLPreElement",        IF_HTMLELEMENT, "pre listing xmp" },
 [IF_HTMLQUOTE]     = { "HTMLQuoteElement",      IF_HTMLELEMENT, "blockquote q" },
 [IF_HTMLOLIST]     = { "HTMLOListElement",      IF_HTMLELEMENT, "ol" },
 [IF_HTMLULIST]     = { "HTMLUListElement",      IF_HTMLELEMENT, "ul" },
 [IF_HTMLMENU]      = { "HTMLMenuElement",       IF_HTMLELEMENT, "menu" },
 [IF_HTMLLI]        = { "HTMLLIElement",         IF_HTMLELEMENT, "li" },
 [IF_HTMLDLIST]     = { "HTMLDListElement",      IF_HTMLELEMENT, "dl" },
 [IF_HTMLDIV]       = { "HTMLDivElement",        IF_HTMLELEMENT, "div" },
 [IF_HTMLANCHOR]    = { "HTMLAnchorElement",     IF_HTMLELEMENT, "a" },
 [IF_HTMLDATA]      = { "HTMLDataElement",       IF_HTMLELEMENT, "data" },
 [IF_HTMLTIME]      = { "HTMLTimeElement",       IF_HTMLELEMENT, "time" },
 [IF_HTMLSPAN]      = { "HTMLSpanElement",       IF_HTMLELEMENT, "span" },
 [IF_HTMLBR]        = { "HTMLBRElement",         IF_HTMLELEMENT, "br" },
 [IF_HTMLMOD]       = { "HTMLModElement",        IF_HTMLELEMENT, "ins del" },
 [IF_HTMLPICTURE]   = { "HTMLPictureElement",    IF_HTMLELEMENT, "picture" },
 [IF_HTMLSOURCE]    = { "HTMLSourceElement",     IF_HTMLELEMENT, "source" },
 [IF_HTMLIMAGE]     = { "HTMLImageElement",      IF_HTMLELEMENT, "img" },
 [IF_HTMLIFRAME]    = { "HTMLIFrameElement",     IF_HTMLELEMENT, "iframe" },
 [IF_HTMLEMBED]     = { "HTMLEmbedElement",      IF_HTMLELEMENT, "embed" },
 [IF_HTMLOBJECT]    = { "HTMLObjectElement",     IF_HTMLELEMENT, "object" },
 [IF_HTMLPARAM]     = { "HTMLParamElement",      IF_HTMLELEMENT, "param" },
 [IF_HTMLVIDEO]     = { "HTMLVideoElement",      IF_HTMLMEDIAELEMENT, "video" },
 [IF_HTMLAUDIO]     = { "HTMLAudioElement",      IF_HTMLMEDIAELEMENT, "audio" },
 [IF_HTMLTRACK]     = { "HTMLTrackElement",      IF_HTMLELEMENT, "track" },
 [IF_HTMLMAP]       = { "HTMLMapElement",        IF_HTMLELEMENT, "map" },
 [IF_HTMLAREA]      = { "HTMLAreaElement",       IF_HTMLELEMENT, "area" },
 [IF_HTMLTABLE]     = { "HTMLTableElement",      IF_HTMLELEMENT, "table" },
 [IF_HTMLTABLECAPTION] = { "HTMLTableCaptionElement", IF_HTMLELEMENT, "caption" },
 [IF_HTMLTABLECOL]  = { "HTMLTableColElement",   IF_HTMLELEMENT, "col colgroup" },
 [IF_HTMLTABLESECTION] = { "HTMLTableSectionElement", IF_HTMLELEMENT, "tbody thead tfoot" },
 [IF_HTMLTABLEROW]  = { "HTMLTableRowElement",   IF_HTMLELEMENT, "tr" },
 [IF_HTMLTABLECELL] = { "HTMLTableCellElement",  IF_HTMLELEMENT, "td th" },
 [IF_HTMLFORM]      = { "HTMLFormElement",       IF_HTMLELEMENT, "form" },
 [IF_HTMLLABEL]     = { "HTMLLabelElement",      IF_HTMLELEMENT, "label" },
 [IF_HTMLINPUT]     = { "HTMLInputElement",      IF_HTMLELEMENT, "input" },
 [IF_HTMLBUTTON]    = { "HTMLButtonElement",     IF_HTMLELEMENT, "button" },
 [IF_HTMLSELECT]    = { "HTMLSelectElement",     IF_HTMLELEMENT, "select" },
 [IF_HTMLDATALIST]  = { "HTMLDataListElement",   IF_HTMLELEMENT, "datalist" },
 [IF_HTMLOPTGROUP]  = { "HTMLOptGroupElement",   IF_HTMLELEMENT, "optgroup" },
 [IF_HTMLOPTION]    = { "HTMLOptionElement",     IF_HTMLELEMENT, "option" },
 [IF_HTMLTEXTAREA]  = { "HTMLTextAreaElement",   IF_HTMLELEMENT, "textarea" },
 [IF_HTMLOUTPUT]    = { "HTMLOutputElement",     IF_HTMLELEMENT, "output" },
 [IF_HTMLPROGRESS]  = { "HTMLProgressElement",   IF_HTMLELEMENT, "progress" },
 [IF_HTMLMETER]     = { "HTMLMeterElement",      IF_HTMLELEMENT, "meter" },
 [IF_HTMLFIELDSET]  = { "HTMLFieldSetElement",   IF_HTMLELEMENT, "fieldset" },
 [IF_HTMLLEGEND]    = { "HTMLLegendElement",     IF_HTMLELEMENT, "legend" },
 [IF_HTMLDETAILS]   = { "HTMLDetailsElement",    IF_HTMLELEMENT, "details" },
 [IF_HTMLDIALOG]    = { "HTMLDialogElement",     IF_HTMLELEMENT, "dialog" },
 [IF_HTMLSCRIPT]    = { "HTMLScriptElement",     IF_HTMLELEMENT, "script" },
 [IF_HTMLTEMPLATE]  = { "HTMLTemplateElement",   IF_HTMLELEMENT, "template" },
 [IF_HTMLSLOT]      = { "HTMLSlotElement",       IF_HTMLELEMENT, "slot" },
 [IF_HTMLCANVAS]    = { "HTMLCanvasElement",     IF_HTMLELEMENT, "canvas" },
 [IF_HTMLFONT]      = { "HTMLFontElement",       IF_HTMLELEMENT, "font" },
 [IF_HTMLDIRECTORY] = { "HTMLDirectoryElement",  IF_HTMLELEMENT, "dir" },
 [IF_HTMLMARQUEE]   = { "HTMLMarqueeElement",    IF_HTMLELEMENT, "marquee" },
 [IF_HTMLFRAME]     = { "HTMLFrameElement",      IF_HTMLELEMENT, "frame" },
 [IF_HTMLFRAMESET]  = { "HTMLFrameSetElement",   IF_HTMLELEMENT, "frameset" },

 [IF_NODELIST]       = { "NodeList",       -1, 0 },
 [IF_HTMLCOLLECTION] = { "HTMLCollection", -1, 0 },
 [IF_NAMEDNODEMAP]   = { "NamedNodeMap",   -1, 0 },
 [IF_DOMTOKENLIST]   = { "DOMTokenList",   -1, 0 },
 [IF_DOMSTRINGMAP]   = { "DOMStringMap",   -1, 0 },
 [IF_DOMEXCEPTION]   = { "DOMException",   -1, 0 },
 [IF_DOMIMPLEMENTATION] = { "DOMImplementation", -1, 0 },
};

/* The live prototypes, strong refs, rebuilt per page (js_page.c makes a fresh
 * JSRuntime per document, so nothing here may survive one). */
static JSValue g_iproto[IF__COUNT];
static int g_iface_ready;
static int g_bridge_runs;           /* see section 6 */
static int g_named_count;           /* see section 7 */

/* tag name -> interface index. Open addressing; the keys are the literals in
 * IFACES[].tags, so no allocation and no ownership. */
#define ITAG_SLOTS 256
static struct { const char *name; short len; short iface; } g_itag[ITAG_SLOTS];

static unsigned itag_hash(const char *s, int len)
{
    unsigned h = 5381u;
    for (int i = 0; i < len; i++) h = h * 33u + (unsigned char)s[i];
    return h;
}

static void itag_put(const char *s, int len, int iface)
{
    unsigned i = itag_hash(s, len) & (ITAG_SLOTS - 1);
    for (unsigned k = 0; k < ITAG_SLOTS; k++) {
        unsigned j = (i + k) & (ITAG_SLOTS - 1);
        if (!g_itag[j].name) {
            g_itag[j].name = s; g_itag[j].len = (short)len; g_itag[j].iface = (short)iface;
            return;
        }
    }
}

static int itag_get(const char *s)
{
    if (!s || !*s) return -1;
    int len = (int)strlen(s);
    unsigned i = itag_hash(s, len) & (ITAG_SLOTS - 1);
    for (unsigned k = 0; k < ITAG_SLOTS; k++) {
        unsigned j = (i + k) & (ITAG_SLOTS - 1);
        if (!g_itag[j].name) return -1;
        if (g_itag[j].len == len && !memcmp(g_itag[j].name, s, (size_t)len))
            return g_itag[j].iface;
    }
    return -1;
}

static void itag_build(void)
{
    memset(g_itag, 0, sizeof g_itag);
    for (int i = 0; i < IF__COUNT; i++) {
        const char *t = IFACES[i].tags;
        if (!t) continue;
        while (*t) {
            const char *e = t;
            while (*e && *e != ' ') e++;
            if (e > t) itag_put(t, (int)(e - t), i);
            t = *e ? e + 1 : e;
        }
    }
}

/* ---------------------------------------------------------------------------
 * 2. The prototype a wrapper gets
 * ------------------------------------------------------------------------- */

static JSValueConst iface_proto_for(const struct node *n)
{
#ifdef JSDOM_NO_INTERFACE_HIERARCHY
    (void)n;
    return JS_UNDEFINED;
#else
    if (!g_iface_ready || !n) return JS_UNDEFINED;
    int idx;
    switch (n->type) {
    case N_TEXT:     idx = IF_TEXT; break;
    case N_COMMENT:  idx = IF_COMMENT; break;
    case N_DOCUMENT: idx = IF_DOCUMENT; break;
    case N_DOCTYPE:  idx = IF_DOCUMENTTYPE; break;
    case N_ELEM:
        if (is_fragment(n)) { idx = IF_DOCUMENTFRAGMENT; break; }
        /* The two synthetic kinds, both elements with a name the tokenizer
         * cannot produce. See is_pi / is_docx for why they are elements. */
        if (n->tag && n->tag[0] == '#') {
            if (is_pi(n))   { idx = IF_PROCESSINGINSTRUCTION; break; }
            if (is_docx(n)) { idx = IF_DOCUMENT; break; }
        }
        if (n->ns == NS_SVG) {
            idx = (n->tag && !strcmp(n->tag, "svg")) ? IF_SVGSVGELEMENT : IF_SVGELEMENT;
            break;
        }
        if (n->ns != NS_HTML) { idx = IF_ELEMENT; break; }   /* MathML: no MathMLElement yet */
        idx = itag_get(n->tag);
        /* An element name the HTML spec does not define is an
         * HTMLUnknownElement -- and one it DOES define but gives no dedicated
         * interface is a plain HTMLElement, which is why those names are listed
         * on IF_HTMLELEMENT above rather than left to fall through here. */
        if (idx < 0) idx = IF_HTMLUNKNOWNELEMENT;
        break;
    default:         idx = IF_NODE; break;
    }
    return JS_IsObject(g_iproto[idx]) ? g_iproto[idx] : JS_UNDEFINED;
#endif
}

/* childNodes / children are Arrays (see child_array's comment on why they are
 * snapshots). Giving them NodeList.prototype / HTMLCollection.prototype -- which
 * themselves inherit Array.prototype -- makes `Object.getPrototypeOf(
 * el.childNodes) === NodeList.prototype` true without taking `.map`/`.filter`
 * away from the pages that already use them on these. */
static void iface_tag_list(JSContext *ctx, JSValueConst arr, int elems_only)
{
#ifdef JSDOM_NO_INTERFACE_HIERARCHY
    (void)ctx; (void)arr; (void)elems_only;
#else
    int idx = elems_only ? IF_HTMLCOLLECTION : IF_NODELIST;
    if (g_iface_ready && JS_IsObject(g_iproto[idx]) && JS_IsObject(arr))
        JS_SetPrototype(ctx, arr, g_iproto[idx]);
#endif
}

/* ---------------------------------------------------------------------------
 * 3. Members the hierarchy made worth adding
 *
 * All of these are Node/Element surface that js_dom.c did not have, and three
 * of them (getAttributeNames, attributes, cloneNode) are asked for BY NAME in
 * js_platform.c, which reimplements them by serialising the element to markup
 * and parsing the start tag with a regular expression -- O(subtree) per call --
 * and skips its own version the moment this file provides one.
 * ------------------------------------------------------------------------- */

static JSValue el_get_isConnected(JSContext *ctx, JSValueConst t)
{
    (void)ctx;
    struct node *n = node_of(t);
    if (!n) return JS_FALSE;
    if (!g_root) return JS_FALSE;
    for (const struct node *p = n; p; p = p->parent) if (p == g_root) return JS_TRUE;
    return JS_FALSE;
}

static JSValue el_getRootNode(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct node *n = node_of(t);
    if (!n) return JS_NULL;
    while (n->parent) n = n->parent;
    return wrap(ctx, n);
}

static JSValue el_get_localName(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n || n->type != N_ELEM) return JS_UNDEFINED;
    return JS_NewString(ctx, n->tag);
}
static JSValue el_get_prefix(JSContext *ctx, JSValueConst t)
{ (void)ctx; (void)t; return JS_NULL; }

static JSValue el_get_childElementCount(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    int c = 0;
    if (n) for (struct node *k = n->first_child; k; k = k->next) if (k->type == N_ELEM) c++;
    return JS_NewInt32(ctx, c);
}

/* cloneNode. dom.c has both halves already: dom_clone_element is the shallow
 * copy the tree builder uses, dom_import_node is the deep one innerHTML= uses,
 * and both return a DETACHED node in the same document -- which is exactly what
 * cloneNode owes its caller. */
static JSValue el_cloneNode(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    if (!n) return JS_NULL;
    int deep = argc > 0 && JS_ToBool(ctx, argv[0]);
    struct dom_doc *d = n->doc;
    if (!d) return JS_NULL;
    if (n->type == N_DOCUMENT) return JS_NULL;      /* no document cloning here */
    if (deep) return wrap(ctx, dom_import_node(d, n));
    switch (n->type) {
    case N_TEXT:    return wrap(ctx, dom_create_text(d, n->text ? n->text : "", n->textlen));
    case N_COMMENT: return wrap(ctx, dom_create_comment(d, n->text ? n->text : "", n->textlen));
    case N_ELEM:    return wrap(ctx, dom_clone_element(n));
    default:        return JS_NULL;
    }
}

/* Document position, the real four-way answer rather than a boolean.
 *
 * DOCUMENT_POSITION_ = DISCONNECTED 1, PRECEDING 2, FOLLOWING 4, CONTAINS 8,
 * CONTAINED_BY 16, IMPLEMENTATION_SPECIFIC 32. The containment cases carry the
 * direction bit too, which is the part hand-rolled versions get wrong: a
 * descendant is CONTAINED_BY | FOLLOWING (20), an ancestor is CONTAINS |
 * PRECEDING (10). */
static JSValue el_compareDocumentPosition(JSContext *ctx, JSValueConst t,
                                          int argc, JSValueConst *argv)
{
    struct node *a = node_of(t);
    struct node *b = argc > 0 ? node_of(argv[0]) : 0;
    if (!a || !b) return JS_NewInt32(ctx, 1 | 32);
    if (a == b) return JS_NewInt32(ctx, 0);
    if (is_ancestor(a, b)) return JS_NewInt32(ctx, 16 | 4);
    if (is_ancestor(b, a)) return JS_NewInt32(ctx, 8 | 2);

    struct node *ra = a, *rb = b;
    while (ra->parent) ra = ra->parent;
    while (rb->parent) rb = rb->parent;
    if (ra != rb) return JS_NewInt32(ctx, 1 | 32 | 2);   /* disconnected: order is ours to pick */

    /* Same tree, neither contains the other: a pre-order walk decides. */
    struct node *n = ra;
    while (n) {
        if (n == a) return JS_NewInt32(ctx, 4);          /* a first -> b FOLLOWING a */
        if (n == b) return JS_NewInt32(ctx, 2);
        if (n->first_child) { n = n->first_child; continue; }
        while (n && !n->next) n = n->parent;
        if (n) n = n->next;
    }
    return JS_NewInt32(ctx, 1 | 32);
}

/* ---- outerHTML ----
 *
 * The single largest item in the HTML-parser line's ranked failure list: 232 of
 * its 387 remaining subtests (serializing.html 119, outerHTML.html 111) are
 * this one property. dom_serialize_html(n, 1) has implemented it correctly all
 * along -- innerHTML has been calling the same function with include_self 0 --
 * so the getter is one line and was missing for no reason at all.
 *
 * The setter is the interesting half: it replaces the element ITSELF, so it
 * needs a parent (the spec throws NoModificationAllowedError without one; we
 * ignore, as every other refusal in this file does) and it parses with the
 * PARENT as fragment context, not with the element -- `td.outerHTML = '<td>'`
 * inside a <tr> has to build a cell, and using the <td> as context would put
 * the new one inside it. */
static JSValue el_get_outerHTML(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    if (!n) return JS_UNDEFINED;
    char *s = dom_serialize_html(n, 1);
    JSValue v = JS_NewString(ctx, s ? s : "");
    free(s);
    return v;
}

/* Parse `src` as a fragment in `ctxel`'s context and insert the result into
 * `parent` before `ref`. Shared by outerHTML= and insertAdjacentHTML, which are
 * the same operation with a different insertion point. Returns 1 if it parsed. */
static int insert_markup(JSContext *ctx, struct node *parent, struct node *ref,
                         struct node *ctxel, const char *src)
{
    (void)ctx;
    if (!parent || !ctxel || !src) return 0;
    struct dom_doc *fdoc = 0;
    struct node *frag = html_parse_fragment(&fdoc, src, (int)strlen(src),
                                            ctxel->tag, (int)strlen(ctxel->tag), ctxel->ns);
    if (frag) {
        for (struct node *c = frag->first_child; c; c = c->next) {
            struct node *cp = dom_import_node(parent->doc, c);
            if (cp) dom_insert_before(parent, cp, ref);
        }
        mark_children(parent);
    }
    if (fdoc) dom_free(dom_doc_root(fdoc));
    return frag != 0;
}

static JSValue el_set_outerHTML(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t);
    if (!n || !n->parent) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_UNDEFINED;
    struct node *p = n->parent;
    struct node *cx = (p->type == N_ELEM) ? p : n;    /* context is the PARENT */
    if (insert_markup(ctx, p, n, cx, s)) dom_destroy_subtree(n);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* insertAdjacentHTML(position, text). The four positions are two insertion
 * points in the parent and two in the element, which is why this is here rather
 * than in JS over outerHTML: doing it that way re-serialises the whole subtree
 * to insert one sibling. */
static JSValue el_insertAdjacentHTML(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *pos = JS_ToCString(ctx, argv[0]);
    const char *s = JS_ToCString(ctx, argv[1]);
    if (pos && s) {
        if (ieq(pos, "beforebegin")) {
            if (n->parent && n->parent->type == N_ELEM)
                insert_markup(ctx, n->parent, n, n->parent, s);
        } else if (ieq(pos, "afterend")) {
            if (n->parent && n->parent->type == N_ELEM)
                insert_markup(ctx, n->parent, n->next, n->parent, s);
        } else if (ieq(pos, "afterbegin")) {
            insert_markup(ctx, n, n->first_child, n, s);
        } else if (ieq(pos, "beforeend")) {
            insert_markup(ctx, n, 0, n, s);
        }
    }
    if (pos) JS_FreeCString(ctx, pos);
    if (s) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* getElementsByTagNameNS(ns, localName) -- missing entirely, and the reason
 * `getElementsByTagName` cannot simply be reused is that this one is
 * case-SENSITIVE on the local name and filters on the namespace, which is the
 * only way to find an SVG <clipPath>. `*` matches either half. */
static JSValue el_getElementsByTagNameNS(JSContext *ctx, JSValueConst t,
                                         int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    JSValue a = JS_NewArray(ctx);
    if (!n || argc < 2 || JS_IsException(a)) return a;
    const char *uri = JS_IsNull(argv[0]) ? 0 : JS_ToCString(ctx, argv[0]);
    const char *local = JS_ToCString(ctx, argv[1]);
    if (local) {
        int any_ns = uri && !strcmp(uri, "*");
        int want_ns = !uri ? NS_HTML
                    : !strcmp(uri, NSURI_SVG) ? NS_SVG
                    : !strcmp(uri, NSURI_MATHML) ? NS_MATHML : NS_HTML;
        int any_local = !strcmp(local, "*");
        uint32_t k = 0;
        for (struct node *c = n->first_child; c; ) {
            if (c->type == N_ELEM && !is_fragment(c) &&
                (any_ns || c->ns == want_ns) &&
                (any_local || !strcmp(c->tag, local)))
                JS_DefinePropertyValueUint32(ctx, a, k++, wrap(ctx, c), JS_PROP_C_W_E);
            if (c->first_child) { c = c->first_child; continue; }
            while (c && c != n && !c->next) c = c->parent;
            if (!c || c == n) break;
            c = c->next;
        }
    }
    if (uri) JS_FreeCString(ctx, uri);
    if (local) JS_FreeCString(ctx, local);
    iface_tag_list(ctx, a, 1);
    return a;
}

/* ---- attributes: the enumeration js_dom.c never had ---- */

static JSValue el_getAttributeNames(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct node *n = node_of(t);
    JSValue a = JS_NewArray(ctx);
    if (!n || n->type != N_ELEM || JS_IsException(a)) return a;
    uint32_t k = 0;
    for (int i = 0; i < n->nattr; i++) {
        const char *nm = dom_attr_name_at(n, i);
        if (nm) JS_DefinePropertyValueUint32(ctx, a, k++, JS_NewString(ctx, nm), JS_PROP_C_W_E);
    }
    return a;
}

static JSValue el_hasAttributes(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    struct node *n = node_of(t);
    return JS_NewBool(ctx, n && n->type == N_ELEM && n->nattr > 0);
}

/* An Attr. Not a wrapper over a struct node -- our attributes are not nodes --
 * so it is a plain object over Attr.prototype carrying the values it was built
 * from. That makes it a SNAPSHOT, the same deviation childNodes already
 * documents, and the same reason: a live Attr would need a second handle class
 * keyed on (element, interned name). */
static JSValue attr_object(JSContext *ctx, struct node *owner, const char *name,
                           const char *value)
{
    JSValue o = (g_iface_ready && JS_IsObject(g_iproto[IF_ATTR]))
                ? JS_NewObjectProto(ctx, g_iproto[IF_ATTR]) : JS_NewObject(ctx);
    if (JS_IsException(o)) return o;
    /* DEFINE, never assign. Attr.prototype inherits Node.prototype, whose
     * nodeType / nodeName / nodeValue are ACCESSORS with no setter -- so a
     * plain `o.nodeType = 2` is silently discarded and every Attr reads back
     * with the wrong node type. An own data property shadows the accessor,
     * which is what these values are. */
#define ATTR_DEF(k, v) JS_DefinePropertyValueStr(ctx, o, (k), (v), JS_PROP_C_W_E)
    ATTR_DEF("name", JS_NewString(ctx, name));
    ATTR_DEF("localName", JS_NewString(ctx, name));
    ATTR_DEF("nodeName", JS_NewString(ctx, name));
    ATTR_DEF("value", JS_NewString(ctx, value ? value : ""));
    ATTR_DEF("nodeValue", JS_NewString(ctx, value ? value : ""));
    ATTR_DEF("namespaceURI", JS_NULL);
    ATTR_DEF("prefix", JS_NULL);
    ATTR_DEF("specified", JS_TRUE);
    ATTR_DEF("nodeType", JS_NewInt32(ctx, 2));
    ATTR_DEF("ownerElement", wrap(ctx, owner));
#undef ATTR_DEF
    return o;
}

/* element.attributes -- a NamedNodeMap: indexed AND named, because that is what
 * uses it. jQuery 1.10.2's support probe is `div.attributes[eventName]`, and
 * `.length` + `[i]` is what every attribute-copying loop on the web is written
 * over. */
static JSValue el_get_attributes(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    JSValue m = (g_iface_ready && JS_IsObject(g_iproto[IF_NAMEDNODEMAP]))
                ? JS_NewObjectProto(ctx, g_iproto[IF_NAMEDNODEMAP]) : JS_NewObject(ctx);
    if (JS_IsException(m)) return m;
    int cnt = 0;
    if (n && n->type == N_ELEM) {
        for (int i = 0; i < n->nattr; i++) {
            const char *nm = dom_attr_name_at(n, i);
            const char *vl = dom_attr_value_at(n, i);
            if (!nm) continue;
            JSValue a = attr_object(ctx, n, nm, vl);
            JS_DefinePropertyValueUint32(ctx, m, (uint32_t)cnt, JS_DupValue(ctx, a), JS_PROP_C_W_E);
            JS_DefinePropertyValueStr(ctx, m, nm, a, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
            cnt++;
        }
    }
    JS_DefinePropertyValueStr(ctx, m, "length", JS_NewInt32(ctx, cnt), JS_PROP_CONFIGURABLE);
    return m;
}

static JSValue el_getAttributeNode(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    if (!n || n->type != N_ELEM || argc < 1) return JS_NULL;
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_NULL;
    JSValue r = JS_NULL;
    for (int i = 0; i < n->nattr; i++) {
        const char *an = dom_attr_name_at(n, i);
        if (an && ieq(an, nm)) { r = attr_object(ctx, n, an, dom_attr_value_at(n, i)); break; }
    }
    JS_FreeCString(ctx, nm);
    return r;
}

/* toggleAttribute(name, force). The two-argument form is the one that matters:
 * `el.toggleAttribute('hidden', shouldHide)` is how modern code writes a
 * boolean attribute, and without it a page falls back to nothing at all. */
static JSValue el_toggleAttribute(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    if (!n || n->type != N_ELEM || argc < 1) return JS_FALSE;
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_FALSE;
    int has = dom_attr(n, nm) != 0;
    int want = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToBool(ctx, argv[1]) : !has;
    if (want && !has)      { dom_set_attr(n, nm, ""); mark_self(n, INVAL_STYLE); }
    else if (!want && has) { if (attr_remove(n, nm)) mark_self(n, INVAL_STYLE); }
    JS_FreeCString(ctx, nm);
    return JS_NewBool(ctx, want);
}

/* ---- ChildNode / ParentNode: the variadic insertion helpers ----
 *
 * append/prepend/before/after/replaceWith/replaceChildren take (Node or string)
 * in any mix; a string becomes a text node. Every one of them routes through
 * insert_run(), so the marking, the fragment flattening and the cycle refusal
 * are the ones appendChild already had -- there is one insertion path in this
 * file and these do not add a second. */
static struct node *coerce_node(JSContext *ctx, JSValueConst v)
{
    struct node *n = node_of(v);
    if (n) return n;
    if (!g_root) return 0;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return 0;
    struct node *tn = dom_create_text(g_root->doc, s, (int)strlen(s));
    JS_FreeCString(ctx, s);
    return tn;
}

/* magic: 0 append, 1 prepend, 2 before, 3 after, 4 replaceWith, 5 replaceChildren */
static JSValue el_insert_variadic(JSContext *ctx, JSValueConst t, int argc,
                                  JSValueConst *argv, int magic)
{
    struct node *self = node_of(t);
    if (!self) return JS_UNDEFINED;
    struct node *parent = (magic == 2 || magic == 3 || magic == 4) ? self->parent : self;
    if (!parent) return JS_UNDEFINED;

    struct node *ref = 0;
    if (magic == 1) ref = self->first_child;               /* prepend */
    else if (magic == 2 || magic == 4) ref = self;         /* before / replaceWith */
    else if (magic == 3) ref = self->next;                 /* after */
    else if (magic == 5) { dom_destroy_children(self); ref = 0; }

    for (int i = 0; i < argc; i++) {
        struct node *c = coerce_node(ctx, argv[i]);
        if (!c) continue;
        insert_run(parent, c, ref);
    }
    if (magic == 4) dom_destroy_subtree(self);             /* replaceWith drops the old */
    return JS_UNDEFINED;
}

static JSValue el_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    struct node *n = node_of(t);
    if (!n || !n->parent) return JS_UNDEFINED;
    struct node *p = n->parent;
    dom_destroy_subtree(n);
    mark_children(p);
    return JS_UNDEFINED;
}

/* CharacterData's own edits, over the one in-place rewriter chardata_set uses,
 * so a React text update and a page's appendData cannot disagree about
 * invalidation. */
static JSValue cd_get_length(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    return JS_NewInt32(ctx, (n && (n->type == N_TEXT || n->type == N_COMMENT)) ? n->textlen : 0);
}
static JSValue cd_appendData(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    if (!n || argc < 1 || (n->type != N_TEXT && n->type != N_COMMENT)) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (s) { dom_text_append(n, s, (int)len); mark_chardata(n); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}
static JSValue cd_substringData(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    struct node *n = node_of(t);
    if (!n || argc < 2 || !n->text) return JS_NewString(ctx, "");
    int32_t off = 0, cnt = 0;
    JS_ToInt32(ctx, &off, argv[0]); JS_ToInt32(ctx, &cnt, argv[1]);
    if (off < 0) off = 0;
    if (off > n->textlen) off = n->textlen;
    if (cnt < 0 || off + cnt > n->textlen) cnt = n->textlen - off;
    return JS_NewStringLen(ctx, n->text + off, (size_t)cnt);
}

/* ---------------------------------------------------------------------------
 * 3b. Document members that needed a node kind dom.c does not have
 * ------------------------------------------------------------------------- */

/* A ProcessingInstruction, by the same trick DocumentFragment already uses: an
 * ELEMENT whose tag name the HTML tokenizer can never produce, because '#' is
 * not a name-start character. dom.c has four node types and adding a fifth
 * would mean touching the tokenizer, the tree builder, the serialiser, the CSS
 * engine and layout -- none of which is this file's to change.
 *
 * `target` and `data` are attributes rather than fields for the same reason.
 * Unlike a fragment a PI CAN be inserted into the tree; what makes that safe is
 * that nothing downstream matches on the name -- a selector cannot name it and
 * layout gives an unknown inline element with no children no box. HTML parsers
 * turn a real `<?...?>` into a comment anyway, so nothing that comes off the
 * wire lands here; this exists because scripts and WPT construct them. */
#define PI_TAG "#processing-instruction"
static int is_pi(const struct node *n)
{ return n && n->type == N_ELEM && n->tag && n->tag[0] == '#' && !strcmp(n->tag, PI_TAG); }

static JSValue doc_createProcessingInstruction(JSContext *ctx, JSValueConst t,
                                               int argc, JSValueConst *argv)
{
    (void)t;
    if (!g_root || argc < 2) return JS_NULL;
    const char *tg = JS_ToCString(ctx, argv[0]);
    const char *da = JS_ToCString(ctx, argv[1]);
    struct node *n = dom_create_element(g_root->doc, PI_TAG, (int)strlen(PI_TAG));
    if (n) {
        dom_set_attr_raw(n, "target", 6, tg ? tg : "", tg ? (int)strlen(tg) : 0);
        dom_set_attr_raw(n, "data", 4, da ? da : "", da ? (int)strlen(da) : 0);
    }
    if (tg) JS_FreeCString(ctx, tg);
    if (da) JS_FreeCString(ctx, da);
    return wrap(ctx, n);
}

static JSValue pi_get_target(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    const char *v = is_pi(n) ? dom_attr(n, "target") : 0;
    return JS_NewString(ctx, v ? v : "");
}
static JSValue pi_get_data(JSContext *ctx, JSValueConst t)
{
    struct node *n = node_of(t);
    const char *v = is_pi(n) ? dom_attr(n, "data") : 0;
    return JS_NewString(ctx, v ? v : "");
}
static JSValue pi_set_data(JSContext *ctx, JSValueConst t, JSValueConst v)
{
    struct node *n = node_of(t);
    if (!is_pi(n)) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (s) { dom_set_attr_raw(n, "data", 4, s, (int)strlen(s)); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* ---- document.implementation.createHTMLDocument ----
 *
 * MEASURED, on real sites: jQuery 3.5.1's $.parseHTML calls it unconditionally,
 * so anthropic.com does not load at all and qwen's jQuery dies here too.
 *
 * THE DOCUMENT IT RETURNS LIVES IN THE PAGE'S ARENA, and that is the whole
 * design decision. dom_doc_new() would give a genuinely separate document, and
 * then can_insert() would refuse every node moved out of it -- which is exactly
 * what jQuery does next (parse into the scratch document, then append the
 * result to the real one). A refusal there is worse than the ReferenceError it
 * replaces: jQuery would run to completion and insert nothing. Sharing the
 * arena costs a synthetic node kind and buys the one behaviour the caller
 * exists for.
 *
 * Known cost, since it is not free: the returned tree is never reclaimed until
 * the page's document is. jQuery makes one per call to $.parseHTML. */
#define DOCX_TAG "#document-detached"
static int is_docx(const struct node *n)
{ return n && n->type == N_ELEM && n->tag && n->tag[0] == '#' && !strcmp(n->tag, DOCX_TAG); }

/* Which node a Document member resolves against. The page document's members
 * used to read the global g_root because there was only ever one document
 * object; now there can be several, so `this` decides -- and falls back to
 * g_root for the page's own document object and for anything unrecognised. */
static struct node *doc_base(JSValueConst t)
{
    struct node *n = node_of(t);
    if (n && (n->type == N_DOCUMENT || is_docx(n))) return n;
    return g_root;
}

static JSValue impl_createHTMLDocument(JSContext *ctx, JSValueConst t,
                                       int argc, JSValueConst *argv)
{
    (void)t;
    if (!g_root) return JS_NULL;
    struct dom_doc *d = g_root->doc;
    struct node *doc  = dom_create_element(d, DOCX_TAG, (int)strlen(DOCX_TAG));
    struct node *html = dom_create_element(d, "html", 4);
    struct node *head = dom_create_element(d, "head", 4);
    struct node *body = dom_create_element(d, "body", 4);
    if (!doc || !html || !head || !body) return JS_NULL;
    dom_append_child(doc, html);
    dom_append_child(html, head);
    dom_append_child(html, body);
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        const char *ti = JS_ToCString(ctx, argv[0]);
        struct node *title = dom_create_element(d, "title", 5);
        if (title) {
            dom_append_child(head, title);
            if (ti) {
                struct node *tx = dom_create_text(d, ti, (int)strlen(ti));
                if (tx) dom_append_child(title, tx);
            }
        }
        if (ti) JS_FreeCString(ctx, ti);
    }
    return wrap(ctx, doc);
}

static JSValue impl_createDocumentType(JSContext *ctx, JSValueConst t,
                                       int argc, JSValueConst *argv)
{
    (void)t;
    if (!g_root || argc < 1) return JS_NULL;
    const char *nm = JS_ToCString(ctx, argv[0]);
    const char *pu = argc > 1 ? JS_ToCString(ctx, argv[1]) : 0;
    const char *sy = argc > 2 ? JS_ToCString(ctx, argv[2]) : 0;
    struct node *n = dom_create_doctype(g_root->doc, nm ? nm : "", pu ? pu : "", sy ? sy : "");
    if (nm) JS_FreeCString(ctx, nm);
    if (pu) JS_FreeCString(ctx, pu);
    if (sy) JS_FreeCString(ctx, sy);
    return wrap(ctx, n);
}

static JSValue impl_hasFeature(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)ctx; (void)t; (void)argc; (void)argv; return JS_TRUE; }   /* the spec says: always true */

static const JSCFunctionListEntry impl_funcs[] = {
    JS_CFUNC_DEF("createHTMLDocument", 1, impl_createHTMLDocument),
    JS_CFUNC_DEF("createDocumentType", 3, impl_createDocumentType),
    JS_CFUNC_DEF("hasFeature", 0, impl_hasFeature),
};

static JSValue doc_get_implementation(JSContext *ctx, JSValueConst t)
{
    (void)t;
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) return o;
    JS_SetPropertyFunctionList(ctx, o, impl_funcs, countof(impl_funcs));
    return o;
}

/* The three Document getters, made `this`-aware so a document from
 * createHTMLDocument answers about ITSELF. js_dom.c's originals resolve against
 * the global g_root, which was correct while exactly one document object could
 * exist. These shadow them on Document.prototype. */
static struct node *find_desc(struct node *root, const char *tag)
{
    if (!root) return 0;
    for (struct node *c = root->first_child; c; ) {
        if (c->type == N_ELEM && !strcmp(c->tag, tag)) return c;
        if (c->first_child) { c = c->first_child; continue; }
        while (c && c != root && !c->next) c = c->parent;
        if (!c || c == root) break;
        c = c->next;
    }
    return 0;
}
static JSValue docx_get_body(JSContext *ctx, JSValueConst t)
{
    struct node *b = doc_base(t);
    struct node *r = find_desc(b, "body");
    if (!r) r = find_desc(b, "frameset");
    return wrap(ctx, r);
}
static JSValue docx_get_head(JSContext *ctx, JSValueConst t)
{ return wrap(ctx, find_desc(doc_base(t), "head")); }
static JSValue docx_get_docel(JSContext *ctx, JSValueConst t)
{
    struct node *b = doc_base(t);
    if (!b) return JS_NULL;
    struct node *h = find_desc(b, "html");
    if (!h) for (struct node *c = b->first_child; c && !h; c = c->next)
                if (c->type == N_ELEM) h = c;
    return wrap(ctx, h);
}

/* document.URL / document.documentURI -- the document's ADDRESS.
 *
 * Two names for one value in the DOM Standard, and they were both missing. That
 * is not only a gap in the surface: `form.action` and `input`/`button`
 * .formAction read as the document's URL when the content attribute is absent,
 * so html/dom/reflection-forms.html asserts them against `document.URL` and got
 * `undefined` for its own expectation -- twelve subtests that no amount of
 * correctness in the reflection could pass.
 *
 * The address comes from `location.href`, which js_webapi.c owns, and NOT from
 * baseURI: `<base href>` moves the base for relative URLs and never moves the
 * document's address. A build with no location object answers "about:blank",
 * which is what a document with no address has. */
static JSValue docx_get_url(JSContext *ctx, JSValueConst t)
{
    (void)t;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue loc = JS_GetPropertyStr(ctx, g, "location");
    JSValue href = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "href") : JS_UNDEFINED;
    JS_FreeValue(ctx, loc);
    JS_FreeValue(ctx, g);
    if (JS_IsString(href)) return href;
    JS_FreeValue(ctx, href);
    return JS_NewString(ctx, "about:blank");
}

static const JSCFunctionListEntry document_extra_funcs[] = {
    JS_CGETSET_DEF("URL", docx_get_url, NULL),
    JS_CGETSET_DEF("documentURI", docx_get_url, NULL),
    JS_CGETSET_DEF("implementation", doc_get_implementation, NULL),
    JS_CGETSET_DEF("body", docx_get_body, NULL),
    JS_CGETSET_DEF("head", docx_get_head, NULL),
    JS_CGETSET_DEF("documentElement", docx_get_docel, NULL),
    JS_CFUNC_DEF("createProcessingInstruction", 2, doc_createProcessingInstruction),
    JS_CFUNC_DEF("getElementsByTagNameNS", 2, el_getElementsByTagNameNS),
};

static const JSCFunctionListEntry pi_proto_funcs[] = {
    JS_CGETSET_DEF("target", pi_get_target, NULL),
    JS_CGETSET_DEF("data", pi_get_data, pi_set_data),
};

/* ---------------------------------------------------------------------------
 * 4. The constructors
 * ------------------------------------------------------------------------- */

static JSValue iface_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                          JSValueConst *argv, int magic)
{
    (void)new_target;
    switch (magic) {
    case IF_TEXT:
    case IF_COMMENT: {
        if (!g_root) return JS_ThrowTypeError(ctx, "no document");
        size_t len = 0;
        const char *s = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : 0;
        struct node *n = (magic == IF_TEXT)
            ? dom_create_text(g_root->doc, s ? s : "", (int)(s ? len : 0))
            : dom_create_comment(g_root->doc, s ? s : "", (int)(s ? len : 0));
        if (s) JS_FreeCString(ctx, s);
        return wrap(ctx, n);
    }
    case IF_DOCUMENTFRAGMENT:
        if (!g_root) return JS_ThrowTypeError(ctx, "no document");
        return wrap(ctx, dom_create_element(g_root->doc, FRAG_TAG, (int)strlen(FRAG_TAG)));
    default:
        break;
    }
    /* An HTML*Element constructor is reachable legitimately in exactly one
     * situation: `class X extends HTMLElement { constructor() { super(); } }`
     * during a custom-element upgrade. js_platform.c's installCustomElements
     * parks the element being upgraded in __ceUpgrading and relies on the base
     * constructor returning it -- a base constructor that returns an object
     * makes that object the derived constructor's `this`, which is the one
     * language rule that makes an upgrade possible from JS at all. That handoff
     * used to live in js_platform.c's own HTMLElement; publishing a real one
     * here takes it over, so it has to keep working. */
    if (magic == IF_HTMLELEMENT || IFACES[magic].parent == IF_HTMLELEMENT ||
        magic == IF_HTMLMEDIAELEMENT || IFACES[magic].parent == IF_HTMLMEDIAELEMENT) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue up = JS_GetPropertyStr(ctx, g, "__ceUpgrading");
        if (JS_IsObject(up)) {
            JS_SetPropertyStr(ctx, g, "__ceUpgrading", JS_NULL);
            JS_FreeValue(ctx, g);
            return up;
        }
        JS_FreeValue(ctx, up);
        JS_FreeValue(ctx, g);
    }
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* DOMException: an Error subclass, so `catch (e) { e.stack }` behaves and
 * `instanceof Error` is true. Its `code` is the legacy number the old
 * exceptions carried; the name is the modern identity. */
static const struct { const char *name; int code; } DOMEXC_CODES[] = {
    { "IndexSizeError", 1 }, { "HierarchyRequestError", 3 },
    { "WrongDocumentError", 4 }, { "InvalidCharacterError", 5 },
    { "NoModificationAllowedError", 7 }, { "NotFoundError", 8 },
    { "NotSupportedError", 9 }, { "InUseAttributeError", 10 },
    { "InvalidStateError", 11 }, { "SyntaxError", 12 },
    { "InvalidModificationError", 13 }, { "NamespaceError", 14 },
    { "InvalidAccessError", 15 }, { "TypeMismatchError", 17 },
    { "SecurityError", 18 }, { "NetworkError", 19 }, { "AbortError", 20 },
    { "URLMismatchError", 21 }, { "QuotaExceededError", 22 },
    { "TimeoutError", 23 }, { "InvalidNodeTypeError", 24 },
    { "DataCloneError", 25 },
};

static JSValue domexc_ctor(JSContext *ctx, JSValueConst new_target, int argc,
                           JSValueConst *argv)
{
    (void)new_target;
    JSValue proto = JS_IsObject(g_iproto[IF_DOMEXCEPTION])
                    ? JS_DupValue(ctx, g_iproto[IF_DOMEXCEPTION]) : JS_UNDEFINED;
    JSValue o = JS_IsObject(proto) ? JS_NewObjectProto(ctx, proto) : JS_NewError(ctx);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(o)) return o;
    const char *msg = (argc > 0 && !JS_IsUndefined(argv[0])) ? JS_ToCString(ctx, argv[0]) : 0;
    const char *nm  = (argc > 1 && !JS_IsUndefined(argv[1])) ? JS_ToCString(ctx, argv[1]) : 0;
    JS_DefinePropertyValueStr(ctx, o, "message", JS_NewString(ctx, msg ? msg : ""),
                              JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, o, "name", JS_NewString(ctx, nm ? nm : "Error"),
                              JS_PROP_C_W_E);
    int code = 0;
    for (unsigned i = 0; nm && i < countof(DOMEXC_CODES); i++)
        if (!strcmp(nm, DOMEXC_CODES[i].name)) { code = DOMEXC_CODES[i].code; break; }
    JS_DefinePropertyValueStr(ctx, o, "code", JS_NewInt32(ctx, code), JS_PROP_C_W_E);
    if (msg) JS_FreeCString(ctx, msg);
    if (nm) JS_FreeCString(ctx, nm);
    return o;
}

/* ---------------------------------------------------------------------------
 * 5. Building it
 * ------------------------------------------------------------------------- */

/* Interface objects are writable + configurable + NOT enumerable. All three are
 * observable: WPT's dom/interface-objects.html walks `for (var p in window)`
 * requiring none of them to show up, then requires `delete window.Node` to
 * return true. JS_SetPropertyStr would make them enumerable. */
static void iface_define_global(JSContext *ctx, JSValueConst g, const char *name, JSValue v)
{
    JS_DefinePropertyValueStr(ctx, g, name, v, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

static void iface_set_tostringtag(JSContext *ctx, JSValueConst proto, const char *name)
{
    JSValue gl = JS_GetGlobalObject(ctx);
    JSValue sym = JS_GetPropertyStr(ctx, gl, "Symbol");
    JSValue tst = JS_GetPropertyStr(ctx, sym, "toStringTag");
    if (!JS_IsUndefined(tst) && !JS_IsException(tst)) {
        JSAtom a = JS_ValueToAtom(ctx, tst);
        JS_DefinePropertyValue(ctx, proto, a, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    JS_FreeValue(ctx, tst); JS_FreeValue(ctx, sym); JS_FreeValue(ctx, gl);
}

static const struct { const char *name; int value; } NODE_CONSTANTS[] = {
    { "ELEMENT_NODE", 1 }, { "ATTRIBUTE_NODE", 2 }, { "TEXT_NODE", 3 },
    { "CDATA_SECTION_NODE", 4 }, { "ENTITY_REFERENCE_NODE", 5 },
    { "ENTITY_NODE", 6 }, { "PROCESSING_INSTRUCTION_NODE", 7 },
    { "COMMENT_NODE", 8 }, { "DOCUMENT_NODE", 9 }, { "DOCUMENT_TYPE_NODE", 10 },
    { "DOCUMENT_FRAGMENT_NODE", 11 }, { "NOTATION_NODE", 12 },
    { "DOCUMENT_POSITION_DISCONNECTED", 1 },
    { "DOCUMENT_POSITION_PRECEDING", 2 },
    { "DOCUMENT_POSITION_FOLLOWING", 4 },
    { "DOCUMENT_POSITION_CONTAINS", 8 },
    { "DOCUMENT_POSITION_CONTAINED_BY", 16 },
    { "DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC", 32 },
};

/* The constants sit on BOTH the constructor and the prototype -- `Node.TEXT_NODE`
 * and `node.TEXT_NODE` are both 3, and code in the wild uses each. Two lines,
 * and a common early throw when one is missing. */
static void iface_install_constants(JSContext *ctx, JSValueConst ctor, JSValueConst proto)
{
    for (unsigned i = 0; i < countof(NODE_CONSTANTS); i++) {
        JS_DefinePropertyValueStr(ctx, ctor, NODE_CONSTANTS[i].name,
                                  JS_NewInt32(ctx, NODE_CONSTANTS[i].value), 0);
        JS_DefinePropertyValueStr(ctx, proto, NODE_CONSTANTS[i].name,
                                  JS_NewInt32(ctx, NODE_CONSTANTS[i].value), 0);
    }
}

static const JSCFunctionListEntry node_extra_funcs[] = {
    JS_CGETSET_DEF("isConnected", el_get_isConnected, NULL),
    JS_CFUNC_DEF("getRootNode", 0, el_getRootNode),
    JS_CFUNC_DEF("cloneNode", 1, el_cloneNode),
    JS_CFUNC_DEF("compareDocumentPosition", 1, el_compareDocumentPosition),
};

static const JSCFunctionListEntry element_extra_funcs[] = {
    JS_CGETSET_DEF("outerHTML", el_get_outerHTML, el_set_outerHTML),
    JS_CFUNC_DEF("insertAdjacentHTML", 2, el_insertAdjacentHTML),
    JS_CFUNC_DEF("getElementsByTagNameNS", 2, el_getElementsByTagNameNS),
    JS_CGETSET_DEF("localName", el_get_localName, NULL),
    JS_CGETSET_DEF("prefix", el_get_prefix, NULL),
    JS_CGETSET_DEF("attributes", el_get_attributes, NULL),
    JS_CFUNC_DEF("getAttributeNames", 0, el_getAttributeNames),
    JS_CFUNC_DEF("getAttributeNode", 1, el_getAttributeNode),
    JS_CFUNC_DEF("hasAttributes", 0, el_hasAttributes),
    JS_CFUNC_DEF("toggleAttribute", 2, el_toggleAttribute),
};

static const JSCFunctionListEntry parentnode_extra_funcs[] = {
    JS_CGETSET_DEF("childElementCount", el_get_childElementCount, NULL),
    JS_CFUNC_MAGIC_DEF("append", 1, el_insert_variadic, 0),
    JS_CFUNC_MAGIC_DEF("prepend", 1, el_insert_variadic, 1),
    JS_CFUNC_MAGIC_DEF("replaceChildren", 1, el_insert_variadic, 5),
};

static const JSCFunctionListEntry childnode_funcs[] = {
    JS_CFUNC_MAGIC_DEF("before", 1, el_insert_variadic, 2),
    JS_CFUNC_MAGIC_DEF("after", 1, el_insert_variadic, 3),
    JS_CFUNC_MAGIC_DEF("replaceWith", 1, el_insert_variadic, 4),
    JS_CFUNC_DEF("remove", 0, el_remove),
};

static const JSCFunctionListEntry chardata_extra_funcs[] = {
    JS_CGETSET_DEF("length", cd_get_length, NULL),
    JS_CFUNC_DEF("appendData", 1, cd_appendData),
    JS_CFUNC_DEF("substringData", 2, cd_substringData),
};

/* NodeList / HTMLCollection / NamedNodeMap all answer `item(i)`; giving them one
 * implementation is the whole of their extra surface here. */
static JSValue coll_item(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    uint32_t i = 0;
    if (JS_ToUint32(ctx, &i, argv[0])) return JS_NULL;
    JSValue v = JS_GetPropertyUint32(ctx, t, i);
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return JS_NULL; }
    return v;
}
static JSValue nnm_getNamedItem(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_NULL;
    const char *nm = JS_ToCString(ctx, argv[0]);
    if (!nm) return JS_NULL;
    JSValue v = JS_GetPropertyStr(ctx, t, nm);
    JS_FreeCString(ctx, nm);
    if (JS_IsUndefined(v)) { JS_FreeValue(ctx, v); return JS_NULL; }
    return v;
}
static const JSCFunctionListEntry coll_funcs[] = {
    JS_CFUNC_DEF("item", 1, coll_item),
};
static const JSCFunctionListEntry nnm_funcs[] = {
    JS_CFUNC_DEF("item", 1, coll_item),
    JS_CFUNC_DEF("getNamedItem", 1, nnm_getNamedItem),
};

/* Build the whole hierarchy and publish it. `token_proto_obj` is the live
 * DOMTokenList prototype js_dom_init already made -- classList must keep
 * working, so DOMTokenList is published OVER it rather than beside it. */
static void iface_install(JSContext *ctx, JSValueConst g, JSValueConst token_proto_obj)
{
    for (int i = 0; i < IF__COUNT; i++) g_iproto[i] = JS_UNDEFINED;
    g_bridge_runs = 0;                  /* per page: a fresh JSRuntime each time */
    g_named_count = 0;
    itag_build();

    JSValue arrproto = JS_UNDEFINED;
    { JSValue arr = JS_GetPropertyStr(ctx, g, "Array");
      arrproto = JS_GetPropertyStr(ctx, arr, "prototype");
      JS_FreeValue(ctx, arr); }
    JSValue errproto = JS_UNDEFINED;
    { JSValue err = JS_GetPropertyStr(ctx, g, "Error");
      errproto = JS_GetPropertyStr(ctx, err, "prototype");
      JS_FreeValue(ctx, err); }

    for (int i = 0; i < IF__COUNT; i++) {
        const struct iface_def *d = &IFACES[i];
        JSValue proto;
        if (i == IF_DOMTOKENLIST && JS_IsObject(token_proto_obj)) {
            proto = JS_DupValue(ctx, token_proto_obj);
        } else if (i == IF_NODELIST || i == IF_HTMLCOLLECTION) {
            proto = JS_IsObject(arrproto) ? JS_NewObjectProto(ctx, arrproto) : JS_NewObject(ctx);
            JS_SetPropertyFunctionList(ctx, proto, coll_funcs, countof(coll_funcs));
        } else if (i == IF_NAMEDNODEMAP) {
            proto = JS_NewObject(ctx);
            JS_SetPropertyFunctionList(ctx, proto, nnm_funcs, countof(nnm_funcs));
        } else if (i == IF_DOMEXCEPTION) {
            proto = JS_IsObject(errproto) ? JS_NewObjectProto(ctx, errproto) : JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, proto, "name", JS_NewString(ctx, "Error"));
            JS_SetPropertyStr(ctx, proto, "message", JS_NewString(ctx, ""));
            JS_SetPropertyStr(ctx, proto, "code", JS_NewInt32(ctx, 0));
        } else {
#ifdef JSDOM_NO_INTERFACE_HIERARCHY
            /* THE NEGATIVE CONTROL: the names exist, the chain does not. */
            proto = JS_NewObject(ctx);
#else
            proto = (d->parent >= 0 && JS_IsObject(g_iproto[d->parent]))
                    ? JS_NewObjectProto(ctx, g_iproto[d->parent])
                    : JS_NewObject(ctx);
#endif
        }
        if (JS_IsException(proto)) { g_iproto[i] = JS_UNDEFINED; continue; }
        g_iproto[i] = proto;

        JSValue ctor = (i == IF_DOMEXCEPTION)
            ? JS_NewCFunction2(ctx, (JSCFunction *)domexc_ctor, d->name, 2,
                               JS_CFUNC_constructor, 0)
            : JS_NewCFunction2(ctx, (JSCFunction *)iface_ctor, d->name, 0,
                               JS_CFUNC_constructor_magic, i);
        JS_SetConstructor(ctx, ctor, proto);
        iface_set_tostringtag(ctx, proto, d->name);
        if (i == IF_NODE) iface_install_constants(ctx, ctor, proto);
        if (i == IF_DOMEXCEPTION)
            for (unsigned k = 0; k < countof(DOMEXC_CODES); k++)
                JS_DefinePropertyValueStr(ctx, ctor, DOMEXC_CODES[k].name,
                                          JS_NewInt32(ctx, DOMEXC_CODES[k].code), 0);
        iface_define_global(ctx, g, d->name, ctor);
    }
    JS_FreeValue(ctx, arrproto);
    JS_FreeValue(ctx, errproto);

    /* HTMLDocument is the same interface object as Document (the spec makes it
     * a legacy alias, and pages test for both). */
    if (JS_IsObject(g_iproto[IF_DOCUMENT])) {
        JSValue doc_ctor = JS_GetPropertyStr(ctx, g_iproto[IF_DOCUMENT], "constructor");
        iface_define_global(ctx, g, "HTMLDocument", doc_ctor);
    }

#ifdef JSDOM_NO_INTERFACE_HIERARCHY
    /* Flat: every member on the one class prototype, which is what wrap() hands
     * out when iface_proto_for answers undefined. */
    {
        JSValue ep = JS_GetClassProto(ctx, elem_cid);
        JS_SetPropertyFunctionList(ctx, ep, evtarget_proto_funcs, countof(evtarget_proto_funcs));
        JS_SetPropertyFunctionList(ctx, ep, node_proto_funcs, countof(node_proto_funcs));
        JS_SetPropertyFunctionList(ctx, ep, node_extra_funcs, countof(node_extra_funcs));
        JS_SetPropertyFunctionList(ctx, ep, chardata_proto_funcs, countof(chardata_proto_funcs));
        JS_SetPropertyFunctionList(ctx, ep, chardata_extra_funcs, countof(chardata_extra_funcs));
        JS_SetPropertyFunctionList(ctx, ep, nondoctype_child_funcs, countof(nondoctype_child_funcs));
        JS_SetPropertyFunctionList(ctx, ep, element_proto_funcs, countof(element_proto_funcs));
        JS_SetPropertyFunctionList(ctx, ep, element_extra_funcs, countof(element_extra_funcs));
        JS_SetPropertyFunctionList(ctx, ep, parentnode_funcs, countof(parentnode_funcs));
        JS_SetPropertyFunctionList(ctx, ep, parentnode_extra_funcs, countof(parentnode_extra_funcs));
        JS_SetPropertyFunctionList(ctx, ep, childnode_funcs, countof(childnode_funcs));
        /* pi_proto_funcs is NOT installed here: its `data` would overwrite
         * CharacterData's on the one flat prototype and break every text node.
         * A ProcessingInstruction therefore only works when it has a prototype
         * of its own, which is the point the control is making. */
        install_on_props(ctx, ep, el_on_get, el_on_set);
        JS_FreeValue(ctx, ep);
        g_iface_ready = 0;
        return;
    }
#else
    /* Members, each on the interface that defines it. */
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_EVENTTARGET],
                               evtarget_proto_funcs, countof(evtarget_proto_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_NODE],
                               node_proto_funcs, countof(node_proto_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_NODE],
                               node_extra_funcs, countof(node_extra_funcs));

    JS_SetPropertyFunctionList(ctx, g_iproto[IF_CHARACTERDATA],
                               chardata_proto_funcs, countof(chardata_proto_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_CHARACTERDATA],
                               chardata_extra_funcs, countof(chardata_extra_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_CHARACTERDATA],
                               nondoctype_child_funcs, countof(nondoctype_child_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_CHARACTERDATA],
                               childnode_funcs, countof(childnode_funcs));

    JS_SetPropertyFunctionList(ctx, g_iproto[IF_ELEMENT],
                               element_proto_funcs, countof(element_proto_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_ELEMENT],
                               element_extra_funcs, countof(element_extra_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_ELEMENT],
                               nondoctype_child_funcs, countof(nondoctype_child_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_ELEMENT],
                               childnode_funcs, countof(childnode_funcs));
    /* GlobalEventHandlers belongs to HTMLElement and SVGElement; Element is the
     * cheap superset and costs a <math> element 25 accessors it will not use. */
    install_on_props(ctx, g_iproto[IF_ELEMENT], el_on_get, el_on_set);

    /* ParentNode is Element + Document + DocumentFragment, all three. */
    static const int PARENTS[] = { IF_ELEMENT, IF_DOCUMENT, IF_DOCUMENTFRAGMENT };
    for (unsigned i = 0; i < countof(PARENTS); i++) {
        JS_SetPropertyFunctionList(ctx, g_iproto[PARENTS[i]],
                                   parentnode_funcs, countof(parentnode_funcs));
        JS_SetPropertyFunctionList(ctx, g_iproto[PARENTS[i]],
                                   parentnode_extra_funcs, countof(parentnode_extra_funcs));
    }
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_DOCUMENTTYPE],
                               childnode_funcs, countof(childnode_funcs));

    /* Document's own surface. `document` is an instance of this, so everything
     * that used to be an own property of one object is now inherited by it --
     * which is what makes `document instanceof Document` and
     * `Document.prototype.createElement` both true. */
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_DOCUMENT], doc_funcs, countof(doc_funcs));
    /* AFTER doc_funcs: body/head/documentElement are redefined here in a
     * `this`-aware form, so a document from createHTMLDocument answers about
     * itself instead of about the page. */
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_DOCUMENT],
                               document_extra_funcs, countof(document_extra_funcs));
    JS_SetPropertyFunctionList(ctx, g_iproto[IF_PROCESSINGINSTRUCTION],
                               pi_proto_funcs, countof(pi_proto_funcs));

    /* The class prototype is the fallback wrap() uses if it is ever handed a
     * node kind the table does not cover. Node.prototype is the safe floor. */
    JS_SetClassProto(ctx, elem_cid, JS_DupValue(ctx, g_iproto[IF_NODE]));
    g_iface_ready = 1;
#endif
}

static void iface_seal_free(JSContext *ctx);   /* defined with the bridge */

static void iface_cleanup(JSContext *ctx)
{
    iface_seal_free(ctx);
    for (int i = 0; i < IF__COUNT; i++) {
        JS_FreeValue(ctx, g_iproto[i]);
        g_iproto[i] = JS_UNDEFINED;
    }
    g_iface_ready = 0;
    g_bridge_runs = 0;
    g_named_count = 0;
}

/* ---------------------------------------------------------------------------
 * 6. The bridge -- two repairs to seams with lines that own other files.
 *
 * (A) THE ELEMENT PROTOTYPE PROBE. js_select.c, js_platform.c, js_media.c and
 * js_events.c all reach the shared element prototype as
 * `Object.getPrototypeOf(document.createElement('div'))`, which this change
 * turns from "the prototype every element shares" into HTMLDivElement.prototype.
 * querySelector, matches, closest, getRootNode, dataset, the reflected .src /
 * .href and the patched addEventListener would land on <div> and on nothing
 * else -- a silent, total regression on every other tag. So, once, after all of
 * them have run, move anything on HTMLDivElement.prototype that this file did
 * not put there down onto **Element.prototype**.
 *
 * Element.prototype and not HTMLElement.prototype, deliberately: before this
 * change those installs reached SVG elements and text nodes too, because there
 * was one prototype for everything, and Element.prototype is the tightest
 * object that keeps every element -- HTML and SVG -- seeing them.
 *
 * The set of things this file DOES put on HTMLDivElement.prototype is exactly
 * {constructor, Symbol.toStringTag} -- that interface has no members of its own
 * -- so the rule needs no bookkeeping and no list to keep in sync. Descriptors
 * are moved as descriptors, because half of what arrives (dataset, src, href)
 * is a getter pair and a plain copy would evaluate it once against the
 * throwaway probe element and freeze that answer for every element on the page.
 *
 * (B) THE EventTarget CONSTRUCTOR. The event line's js_events.c publishes its
 * own `EventTarget` (a constructible one, backed by a WeakMap store for objects
 * that are not in the document) with `G.EventTarget = ET`, unconditionally and
 * after js_dom_init. That is its call to make -- it owns the BEHAVIOUR of the
 * three methods -- but it leaves `node instanceof EventTarget` false, because
 * Node.prototype still inherits the object the replaced constructor pointed at.
 * So: if the global constructor is no longer the one at the root of this chain,
 * copy the three NATIVE methods down onto Node.prototype (the DOM must keep the
 * C dispatcher; the standalone store knows nothing about propagation) and
 * reparent Node.prototype onto whatever EventTarget.prototype now is. Both
 * lines then get what they own, and the chain answers.
 * ------------------------------------------------------------------------- */

/* WHEN IT RUNS, which cost a wrong measurement to learn. The first version hung
 * this off js_dom_run_jobs(), on the reasoning that js_page.c drains the job
 * queue after every script evaluation, so the bridge would land between
 * js_page_open and anything a page could observe. It does -- for js_page_eval.
 * The WPT runner calls JS_Eval DIRECTLY (it has to: it runs each <script> as its
 * own evaluation the way a browser does), so js_dom_run_jobs was never reached
 * and the bridge never fired. Every non-<div> element lost querySelector,
 * matches and closest, which is 500-odd subtests reported as "not a function".
 *
 * So it does not depend on an embedder calling anything. It runs from wrap(),
 * for the first BRIDGE_WRAPS wrappers of a page. Every installer probes with
 * `document.createElement('div')` -- that is a wrap -- and js_page_open makes
 * fewer than a dozen wrappers in total, so the window covers the whole install
 * sequence with two orders of magnitude to spare, and closes long before it
 * could cost anything. It is C rather than a JS_Eval for the same reason: at 128
 * runs a compile per run would be real, and at 128 runs of this it is not
 * measurable. */
#define BRIDGE_WRAPS 128

/* WHAT THIS FILE PUT ON HTMLDivElement.prototype, recorded rather than listed.
 *
 * The bridge below moves everything on that prototype that "this file did not
 * put there" down onto Element.prototype, and the rule used to need no
 * bookkeeping because the answer was exactly {constructor, Symbol.toStringTag}.
 * IDL attribute reflection ends that: <div> reflects `align`, so the DOM line's
 * own accessor now sits there too, and moving it would give `align` to every
 * element in the document -- a reflected attribute leaking off the one
 * interface that declares it.
 *
 * So the set is SNAPSHOT instead of enumerated: iface_seal_div() runs at the end
 * of js_dom_init, before any other installer can touch the object, and whatever
 * is on it at that instant is ours by construction. That is self-maintaining --
 * it stays correct as this file grows members -- and it needs no cross-file
 * registry with js_reflect.c. */
#define DIV_SEAL_MAX 64
static JSAtom g_div_seal[DIV_SEAL_MAX];
static int g_div_seal_n;

static void iface_seal_div(JSContext *ctx)
{
    g_div_seal_n = 0;
    if (!g_iface_ready || !JS_IsObject(g_iproto[IF_HTMLDIV])) return;
    JSPropertyEnum *tab = 0;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, g_iproto[IF_HTMLDIV],
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return;
    for (uint32_t i = 0; i < len; i++) {
        if (g_div_seal_n < DIV_SEAL_MAX)
            g_div_seal[g_div_seal_n++] = JS_DupAtom(ctx, tab[i].atom);
        JS_FreeAtom(ctx, tab[i].atom);
    }
    js_free(ctx, tab);
}

static void iface_seal_free(JSContext *ctx)
{
    for (int i = 0; i < g_div_seal_n; i++) JS_FreeAtom(ctx, g_div_seal[i]);
    g_div_seal_n = 0;
}

static int div_is_ours(JSAtom k)
{
    for (int i = 0; i < g_div_seal_n; i++) if (g_div_seal[i] == k) return 1;
    return 0;
}

/* Move every own property of `from` that `to` does not already have, then
 * delete it from `from`. Descriptors are moved AS descriptors: half of what
 * arrives is an accessor pair and copying the VALUE would evaluate the getter
 * once against the installer's throwaway probe element and freeze that answer
 * for every element on the page. Returns the number moved. */
static int move_own_props(JSContext *ctx, JSValueConst from, JSValueConst to)
{
    JSPropertyEnum *tab = 0;
    uint32_t len = 0, i;
    int moved = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, from,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return 0;
    JSAtom a_ctor = JS_NewAtom(ctx, "constructor");
    for (i = 0; i < len; i++) {
        JSAtom k = tab[i].atom;
        if (k == a_ctor || div_is_ours(k)) continue;
        JSPropertyDescriptor d;
        int has = JS_GetOwnProperty(ctx, &d, from, k);
        if (has <= 0) continue;
        JSPropertyDescriptor dt;
        int hast = JS_GetOwnProperty(ctx, &dt, to, k);
        if (hast > 0) {
            JS_FreeValue(ctx, dt.value); JS_FreeValue(ctx, dt.getter); JS_FreeValue(ctx, dt.setter);
        } else {
            JS_DefineProperty(ctx, to, k, d.value, d.getter, d.setter,
                              d.flags | JS_PROP_HAS_CONFIGURABLE | JS_PROP_HAS_ENUMERABLE |
                              (JS_IsUndefined(d.getter) && JS_IsUndefined(d.setter)
                               ? (JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE)
                               : (JS_PROP_HAS_GET | JS_PROP_HAS_SET)));
        }
        JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
        JS_DeleteProperty(ctx, from, k, 0);
        moved++;
    }
    JS_FreeAtom(ctx, a_ctor);
    for (i = 0; i < len; i++) JS_FreeAtom(ctx, tab[i].atom);
    js_free(ctx, tab);
    return moved;
}

static void iface_bridge(JSContext *ctx)
{
#ifndef JSDOM_NO_INTERFACE_HIERARCHY
    if (!g_iface_ready || g_bridge_runs >= BRIDGE_WRAPS) return;
    g_bridge_runs++;
    if (!JS_IsObject(g_iproto[IF_HTMLDIV]) || !JS_IsObject(g_iproto[IF_ELEMENT])) return;

    /* (A) */
    move_own_props(ctx, g_iproto[IF_HTMLDIV], g_iproto[IF_ELEMENT]);

    /* (B) the EventTarget constructor, if the event line replaced it. */
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue et = JS_GetPropertyStr(ctx, g, "EventTarget");
    JSValue etp = JS_IsObject(et) ? JS_GetPropertyStr(ctx, et, "prototype") : JS_UNDEFINED;
    if (JS_IsObject(etp) && JS_IsObject(g_iproto[IF_NODE])) {
        JSValue cur = JS_GetPrototype(ctx, g_iproto[IF_NODE]);
        if (JS_VALUE_GET_PTR(cur) != JS_VALUE_GET_PTR(etp)) {
            static const char *THREE[] = { "addEventListener", "removeEventListener",
                                           "dispatchEvent" };
            for (unsigned j = 0; j < countof(THREE); j++) {
                JSAtom k = JS_NewAtom(ctx, THREE[j]);
                JSPropertyDescriptor d, dn;
                if (JS_GetOwnProperty(ctx, &dn, g_iproto[IF_NODE], k) > 0) {
                    JS_FreeValue(ctx, dn.value); JS_FreeValue(ctx, dn.getter);
                    JS_FreeValue(ctx, dn.setter);
                } else if (JS_IsObject(cur) && JS_GetOwnProperty(ctx, &d, cur, k) > 0) {
                    JS_DefineProperty(ctx, g_iproto[IF_NODE], k, d.value, d.getter, d.setter,
                                      d.flags | JS_PROP_HAS_VALUE | JS_PROP_HAS_WRITABLE |
                                      JS_PROP_HAS_CONFIGURABLE);
                    JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter);
                    JS_FreeValue(ctx, d.setter);
                }
                JS_FreeAtom(ctx, k);
            }
            JS_SetPrototype(ctx, g_iproto[IF_NODE], etp);
        }
        JS_FreeValue(ctx, cur);
    }
    JS_FreeValue(ctx, etp); JS_FreeValue(ctx, et); JS_FreeValue(ctx, g);
#else
    (void)ctx;
#endif
}

/* ---------------------------------------------------------------------------
 * 7. Named access on the window object
 *
 * `<span id="target">` makes `target` a global. It is one mechanism and it is
 * the largest single item the corpus ranks: ~480 files across dom/ and css/ die
 * BEFORE their first assertion on a bare `ReferenceError: 'target' is not
 * defined` (or 'host', 'scroller', 'container', 'anchored', 'main', ...), and a
 * file that dies that early hides every subtest inside it. It reads as forty
 * different missing globals in a report and it is one bug.
 *
 * THE PROPERTY IS AN ACCESSOR, NOT A SNAPSHOT. The name is defined once, and
 * every read resolves through the document's live id index -- so a page that
 * moves or replaces the element gets the current one, and an element that has
 * left the document reads as undefined because dom_get_element_by_id only
 * returns nodes connected to the root. A snapshot of the values taken at parse
 * end passes the easy tests and fails exactly the ones that are about liveness.
 *
 * TWO SHADOWING RULES, both required and both easy to get wrong:
 *   - a real global wins. Defining is skipped when the global already has an
 *     own property of that name, so a page's own `var target = ...` (which is a
 *     global binding created before its markup can matter) is never clobbered.
 *   - an assignment replaces the name permanently. That is what the SETTER is
 *     for: it does not write through to the element, it redefines the property
 *     as a plain writable value and the accessor is gone for good.
 *
 * KNOWN DEVIATION: after the element leaves the document the name still EXISTS
 * (reading it gives undefined) where the spec says it should be gone, so
 * `'target' in window` stays true. Removing it would mean tracking every
 * detach; the observable difference is a `typeof` that already answers
 * "undefined" either way.
 *
 * The set of names is maintained where the DOM changes: at init over the parsed
 * document, on an id/name attribute write, and on every insertion (insert_run
 * walks what arrived). Only elements CONNECTED to the document contribute. */
#define NAMED_MAX 8192          /* a bound on how many names one page can claim */

static int named_is_name_tag(const struct node *n)
{
    /* The elements whose `name` attribute is exposed. The spec's list, which is
     * shorter than "everything with a name": a <input name=x> must NOT become a
     * global, or every form on the web would carpet the window object. */
    switch (n->tag_id) {
    case TAG_EMBED: case TAG_FORM: case TAG_IFRAME: case TAG_IMG:
    case TAG_OBJECT: return 1;
    default: return 0;
    }
}

static JSValue named_get(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                         int magic, JSValue *data)
{
    (void)t; (void)argc; (void)argv; (void)magic;
    if (!g_root) return JS_UNDEFINED;
    const char *nm = JS_ToCString(ctx, data[0]);
    if (!nm) return JS_UNDEFINED;
    struct node *n = dom_get_element_by_id(g_root->doc, nm);
    if (!n) {
        /* the `name` half: no index for it, so a walk -- and only when the id
         * index missed, which is the overwhelmingly common case answered. */
        for (struct node *c = g_root->first_child; c; ) {
            if (c->type == N_ELEM && named_is_name_tag(c)) {
                const char *v = dom_attr(c, "name");
                if (v && !strcmp(v, nm)) { n = c; break; }
            }
            if (c->first_child) { c = c->first_child; continue; }
            while (c && c != g_root && !c->next) c = c->parent;
            if (!c || c == g_root) break;
            c = c->next;
        }
    }
    JS_FreeCString(ctx, nm);
    return n ? wrap(ctx, n) : JS_UNDEFINED;
}

/* The assignment rule: `target = 1` must make `target` 1, for good. */
static JSValue named_set(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv,
                         int magic, JSValue *data)
{
    (void)t; (void)magic;
    const char *nm = JS_ToCString(ctx, data[0]);
    if (!nm) return JS_UNDEFINED;
    JSValue g = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, g, nm,
                              argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED,
                              JS_PROP_C_W_E);
    JS_FreeValue(ctx, g);
    JS_FreeCString(ctx, nm);
    return JS_UNDEFINED;
}

static void named_define(JSContext *ctx, JSValueConst g, const char *nm)
{
    if (!nm || !*nm || g_named_count >= NAMED_MAX) return;
    JSAtom a = JS_NewAtom(ctx, nm);
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, g, a);
    if (has > 0) {                       /* a real global wins, always */
        JS_FreeValue(ctx, d.value); JS_FreeValue(ctx, d.getter); JS_FreeValue(ctx, d.setter);
        JS_FreeAtom(ctx, a);
        return;
    }
    JSValue nv = JS_NewString(ctx, nm);
    JSValue get = JS_NewCFunctionData(ctx, named_get, 0, 0, 1, &nv);
    JSValue set = JS_NewCFunctionData(ctx, named_set, 1, 0, 1, &nv);
    JS_FreeValue(ctx, nv);
    JS_DefinePropertyGetSet(ctx, g, a, get, set, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
    g_named_count++;
}

/* Claim every name in `root`'s subtree. Called with the document at init and
 * with whatever just arrived on every insertion. */
static void named_scan(JSContext *ctx, struct node *root)
{
#ifndef JSDOM_NO_INTERFACE_HIERARCHY
    if (!ctx || !root || g_named_count >= NAMED_MAX) return;
    JSValue g = JS_GetGlobalObject(ctx);
    struct node *c = root;
    while (c) {
        if (c->type == N_ELEM) {
            const char *id = dom_attr(c, "id");
            if (id && *id) named_define(ctx, g, id);
            if (named_is_name_tag(c)) {
                const char *nm = dom_attr(c, "name");
                if (nm && *nm) named_define(ctx, g, nm);
            }
        }
        if (c->first_child) { c = c->first_child; continue; }
        while (c && c != root && !c->next) c = c->parent;
        if (!c || c == root) break;
        c = c->next;
    }
    JS_FreeValue(ctx, g);
#else
    (void)ctx; (void)root;
#endif
}

/* One attribute write. Cheaper than a subtree scan and it is the hot path:
 * `el.id = 'x'` and `setAttribute('id', ...)` both land here. */
static void named_note_attr(JSContext *ctx, struct node *n, const char *attr)
{
#ifndef JSDOM_NO_INTERFACE_HIERARCHY
    if (!ctx || !n || !attr || n->type != N_ELEM) return;
    if (!connected(n)) return;
    int isid = ieq(attr, "id"), isnm = ieq(attr, "name");
    if (!isid && !isnm) return;
    if (isnm && !named_is_name_tag(n)) return;
    const char *v = dom_attr(n, attr);
    if (!v || !*v) return;
    JSValue g = JS_GetGlobalObject(ctx);
    named_define(ctx, g, v);
    JS_FreeValue(ctx, g);
#else
    (void)ctx; (void)n; (void)attr;
#endif
}
