/* js_forms.c -- the JavaScript surface of the form controls and the focus
 * model. `input.value`, `checkbox.checked`, `el.focus()`,
 * `document.activeElement`, `form.submit()`, `selectionStart`.
 *
 * WHY IT IS ITS OWN FILE. js_dom.c owns the Element wrapper and belongs to
 * another line. Everything below is therefore installed from OUTSIDE it, onto
 * the prototype it publishes -- the same seam js_media.c uses for
 * HTMLMediaElement, and for the same reason. Nothing here edits js_dom.c and
 * nothing here needs to.
 *
 * HOW A JSValue BECOMES A `struct node *`. It cannot, directly: js_dom.c
 * exports no accessor and js_media.c already ran into exactly this. The
 * mechanism is the one that file established -- the element carries an integer
 * key in a `data-logit-fcid` attribute, the JS shim stamps it, and every native
 * call passes the integer. The native side resolves the integer by walking the
 * document, with a small {id -> node, serial} cache in front so a page that
 * reads `input.value` in a loop does not walk the DOM per read.
 *
 *   `struct node *js_dom_node_of(JSValueConst)` exported from js_dom.c would
 *   delete this whole mechanism. It is an ASK for that line, not an edit.
 *
 * WHAT IS IN C AND WHAT IS IN THE SHIM. The C is only what has to reach forms.c
 * -- twelve entry points, all taking the integer key. Everything that is
 * plumbing (the property descriptors, the tag guards, activeElement, `form`,
 * `elements`) is written in JavaScript at the bottom, the way js_webapi.c and
 * js_platform.c do it. `document.activeElement` in particular is NOT a native
 * call: it is a focusin/focusout listener on the document, which works because
 * focus.c already dispatches both through the same bubbling walk a page's own
 * listener uses -- so if activeElement is right, the events are right.
 */

#include "quickjs.h"
#include "dom.h"
#include "js_dom.h"
#include "forms.h"
#include "focus.h"
#include <string.h>

/* ================================================= id -> node resolution == */

static int g_next_id = 1;

#define FC_CACHE 64
static struct { int id; struct node *n; uint32_t serial; } g_cache[FC_CACHE];

static struct node *walk_for_id(struct node *n, const char *want)
{
    for (struct node *c = n->first_child; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        const char *v = dom_attr(c, "data-logit-fcid");
        if (v) {
            int i = 0;
            while (v[i] && want[i] && v[i] == want[i]) i++;
            if (!v[i] && !want[i]) return c;
        }
        struct node *d = walk_for_id(c, want);
        if (d) return d;
    }
    return 0;
}

static struct node *node_of(int id)
{
    if (id <= 0) return 0;
    int slot = id % FC_CACHE;
    if (g_cache[slot].id == id && g_cache[slot].n &&
        g_cache[slot].n->serial == g_cache[slot].serial)
        return g_cache[slot].n;
    char want[16];
    int p = 0, v = id;
    char t[12]; int i = 0;
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) want[p++] = t[--i];
    want[p] = 0;
    struct node *root = js_dom_root();
    if (!root) return 0;
    struct node *n = walk_for_id(root, want);
    if (n) { g_cache[slot].id = id; g_cache[slot].n = n; g_cache[slot].serial = n->serial; }
    return n;
}

static int arg_id(JSContext *ctx, JSValueConst v)
{
    int32_t id = 0;
    JS_ToInt32(ctx, &id, v);
    return (int)id;
}

/* ==================================================== the native surface == */

static JSValue jf_newid(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; (void)argc; (void)argv; return JS_NewInt32(ctx, g_next_id++); }

static JSValue jf_kind(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; (void)argc; return JS_NewInt32(ctx, fc_kind(node_of(arg_id(ctx, argv[0])))); }

static JSValue jf_value(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    if (!n) return JS_NewString(ctx, "");
    int len = 0;
    const char *v = fc_value(n, &len);
    return JS_NewStringLen(ctx, v, (size_t)len);
}

static JSValue jf_default_value(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    if (!n) return JS_NewString(ctx, "");
    int len = 0;
    const char *v = fc_default_value(n, &len);
    return JS_NewStringLen(ctx, v, (size_t)len);
}

static JSValue jf_setvalue(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (n && s) fc_set_value(n, s, (int)len);
    if (s) JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue jf_checked(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; (void)argc; return JS_NewBool(ctx, fc_checked(node_of(arg_id(ctx, argv[0])))); }

static JSValue jf_setchecked(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    if (n) fc_set_checked(n, JS_ToBool(ctx, argv[1]) > 0);
    return JS_UNDEFINED;
}

/* magic 0 = start, 1 = end */
static JSValue jf_selpos(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    if (!n || !FC_IS_TEXTUAL(fc_kind(n))) return JS_NULL;
    int a = 0, b = 0;
    fc_selection(n, &a, &b);
    return JS_NewInt32(ctx, magic ? b : a);
}

static JSValue jf_setsel(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    int32_t a = 0, b = 0;
    JS_ToInt32(ctx, &a, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &b, argv[2]);
    else b = a;
    if (n) fc_set_selection(n, a, b);
    return JS_UNDEFINED;
}

static JSValue jf_selectall(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    if (n) fc_edit_select_all(n);
    return JS_UNDEFINED;
}

static JSValue jf_selidx(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; (void)argc; return JS_NewInt32(ctx, fc_selected_index(node_of(arg_id(ctx, argv[0])))); }

static JSValue jf_setselidx(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    int32_t i = -1;
    JS_ToInt32(ctx, &i, argv[1]);
    if (n) fc_set_selected_index(n, i);
    return JS_UNDEFINED;
}

/* magic: 0 focus, 1 blur */
static JSValue jf_focus(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    if (magic) { if (focus_current() == n) focus_set(0); }
    else if (n) {
        struct node *old = focus_current();
        if (old && fc_kind(old) != FC_NONE) fc_commit(old);
        focus_set(n);
        if (fc_kind(n) != FC_NONE) fc_mark_focus(n);
    }
    return JS_UNDEFINED;
}

static JSValue jf_active(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    /* The id of the focused element, or 0. Only useful when that element has
     * already been stamped -- the shim's focusin listener is the primary path
     * and this is the fallback for focus moved from C (a click, or Tab). */
    struct node *n = focus_current();
    if (!n) return JS_NewInt32(ctx, 0);
    const char *v = dom_attr(n, "data-logit-fcid");
    if (!v) return JS_NewInt32(ctx, -1);        /* focused, but never stamped */
    int id = 0;
    for (const char *p = v; *p >= '0' && *p <= '9'; p++) id = id * 10 + (*p - '0');
    return JS_NewInt32(ctx, id);
}

/* magic: 0 = submit (no event), 1 = requestSubmit (fires it), 2 = reset */
static JSValue jf_formact(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic)
{
    (void)t;
    struct node *f = node_of(arg_id(ctx, argv[0]));
    if (!f) return JS_UNDEFINED;
    if (magic == 2) { fc_reset_form(f); return JS_UNDEFINED; }
    struct node *sub = (argc > 1) ? node_of(arg_id(ctx, argv[1])) : 0;
    fc_submit(f, sub, magic == 1);
    return JS_UNDEFINED;
}

static JSValue jf_encode(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *f = node_of(arg_id(ctx, argv[0]));
    static char buf[8192];
    int n = f ? fc_encode(f, 0, buf, (int)sizeof buf) : -1;
    if (n < 0) return JS_NewString(ctx, "");
    return JS_NewStringLen(ctx, buf, (size_t)n);
}

static const JSCFunctionListEntry g_natives[] = {
    JS_CFUNC_DEF("__fc_newid", 0, jf_newid),
    JS_CFUNC_DEF("__fc_kind", 1, jf_kind),
    JS_CFUNC_DEF("__fc_value", 1, jf_value),
    JS_CFUNC_DEF("__fc_defvalue", 1, jf_default_value),
    JS_CFUNC_DEF("__fc_setvalue", 2, jf_setvalue),
    JS_CFUNC_DEF("__fc_checked", 1, jf_checked),
    JS_CFUNC_DEF("__fc_setchecked", 2, jf_setchecked),
    JS_CFUNC_MAGIC_DEF("__fc_selstart", 1, jf_selpos, 0),
    JS_CFUNC_MAGIC_DEF("__fc_selend", 1, jf_selpos, 1),
    JS_CFUNC_DEF("__fc_setsel", 3, jf_setsel),
    JS_CFUNC_DEF("__fc_selectall", 1, jf_selectall),
    JS_CFUNC_DEF("__fc_selidx", 1, jf_selidx),
    JS_CFUNC_DEF("__fc_setselidx", 2, jf_setselidx),
    JS_CFUNC_MAGIC_DEF("__fc_focus", 1, jf_focus, 0),
    JS_CFUNC_MAGIC_DEF("__fc_blur", 1, jf_focus, 1),
    JS_CFUNC_DEF("__fc_active", 0, jf_active),
    JS_CFUNC_MAGIC_DEF("__fc_submit", 2, jf_formact, 0),
    JS_CFUNC_MAGIC_DEF("__fc_reqsubmit", 2, jf_formact, 1),
    JS_CFUNC_MAGIC_DEF("__fc_formreset", 1, jf_formact, 2),
    JS_CFUNC_DEF("__fc_encode", 1, jf_encode),
};

/* ============================================================== the shim == */

static const char SHIM[] =
"(function(G){\n"
"var doc = G.document; if (!doc || typeof doc.createElement !== 'function') return;\n"
"var proto = Object.getPrototypeOf(doc.createElement('input'));\n"
"if (!proto || proto.__fcInstalled) return;\n"
"proto.__fcInstalled = true;\n"
/* The integer key. Stamped lazily: an element nobody scripts costs nothing. */
"function key(el){\n"
"  var k = el.getAttribute && el.getAttribute('data-logit-fcid');\n"
"  if (!k) { k = String(G.__fc_newid()); el.setAttribute('data-logit-fcid', k); }\n"
"  return +k;\n"
"}\n"
"function tag(el){ return (el.tagName||'').toLowerCase(); }\n"
/* DETACHED ELEMENTS, and this is the one real limitation of the integer-key
 * mechanism. The native side resolves a key by walking the DOCUMENT, so an
 * element made by document.createElement() and never inserted is invisible to
 * it -- and that is not a corner case, it is how most of the WPT forms corpus
 * is written (`var i = document.createElement("input"); i.value = "x"`).
 *
 * So: __fc_kind() answering 0 for something we already know is a control means
 * "the native side cannot see this one", and the value, checkedness and
 * selection are kept on the wrapper itself until it is inserted. That is a
 * genuine second implementation of the state, which is a cost worth naming --
 * it exists because `js_dom_node_of(JSValueConst)` does not, and it disappears
 * the day that lands. Known gap: a value set while detached does not carry over
 * when the element is inserted. */
"function detached(el){ return G.__fc_kind(key(el)) === 0; }\n"
"function dget(el, slot, dflt){ return (slot in el) ? el[slot] : dflt; }\n"
"function isCtl(el){ var t = tag(el); return t==='input'||t==='select'||t==='textarea'||t==='button'; }\n"
"function isField(el){ var t = tag(el); return t==='input'||t==='textarea'; }\n"
/* NEVER CLOBBER. js_dom.c publishes the Element wrapper and js_platform.c fills
 * gaps behind it, and both are other lines' files; an accessor defined here
 * that shadows one of theirs turns a working property into `undefined` for
 * EVERY element, not just controls. The first cut of this file defined `type`
 * and `name` unconditionally and the WPT forms subset went DOWN, because
 * <script>.type and <link>.type stopped answering. So: if the property already
 * resolves on the prototype chain, its owner keeps it. */
"function def(o, name, get, set){\n"
"  if (name in o) return;\n"
"  var d = { configurable: true, enumerable: false };\n"
"  if (get) d.get = get; if (set) d.set = set;\n"
"  try { Object.defineProperty(o, name, d); } catch (e) {}\n"
"}\n"
"function force(o, name, get, set){\n"
"  var d = { configurable: true, enumerable: false };\n"
"  if (get) d.get = get; if (set) d.set = set;\n"
"  try { Object.defineProperty(o, name, d); } catch (e) {}\n"
"}\n"
/* ---- value / defaultValue ------------------------------------------------ */
"function hasValue(el){ var t = tag(el);\n"
"  return t==='input'||t==='select'||t==='textarea'||t==='button'||\n"
"         t==='option'||t==='li'||t==='data'||t==='param'||t==='progress'||t==='meter'; }\n"
"def(proto, 'value',\n"
"  function(){ if (!hasValue(this)) return undefined;\n"
"    if (isCtl(this)) {\n"
"      if (!detached(this)) return G.__fc_value(key(this));\n"
"      if ('__fcv' in this) return this.__fcv;\n"
"      if (tag(this)==='textarea') return this.textContent || '';\n"
"      var a = this.getAttribute('value');\n"
"      return a === null ? '' : a;\n"
"    }\n"
"    var v = this.getAttribute('value');\n"
"    return v === null ? (tag(this)==='option' ? this.textContent : '') : v; },\n"
"  function(v){ var s = (v === null || v === undefined) ? '' : String(v);\n"
"    if (isCtl(this)) { if (detached(this)) this.__fcv = s;\n"
"                       else G.__fc_setvalue(key(this), s); return; }\n"
"    if (hasValue(this)) this.setAttribute('value', s); });\n"
"def(proto, 'defaultValue',\n"
"  function(){ return isCtl(this) ? G.__fc_defvalue(key(this)) : undefined; },\n"
"  function(v){ if (tag(this)==='textarea') this.textContent = String(v);\n"
"               else this.setAttribute('value', String(v)); });\n"
/* ---- checked / defaultChecked -------------------------------------------- */
"def(proto, 'checked',\n"
"  function(){ if (tag(this)!=='input') return undefined;\n"
"    if (!detached(this)) return G.__fc_checked(key(this));\n"
"    return dget(this, '__fcc', this.hasAttribute('checked')); },\n"
"  function(v){ if (tag(this)!=='input') return;\n"
"    if (detached(this)) this.__fcc = !!v; else G.__fc_setchecked(key(this), !!v); });\n"
"def(proto, 'defaultChecked',\n"
"  function(){ return this.hasAttribute('checked'); },\n"
"  function(v){ if (v) this.setAttribute('checked',''); else this.removeAttribute('checked'); });\n"
/* ---- the plain reflected attributes -------------------------------------- */
"function reflect(name, attr, dflt){\n"
"  def(proto, name,\n"
"    function(){ var v = this.getAttribute(attr); return v === null ? (dflt||'') : v; },\n"
"    function(v){ this.setAttribute(attr, String(v)); });\n"
"}\n"
"reflect('placeholder','placeholder');\n"
"reflect('name','name');\n"
"reflect('accept','accept');\n"
"reflect('autocomplete','autocomplete');\n"
"def(proto, 'type',\n"
"  function(){ var t = tag(this);\n"
"    if (t==='input')  { return (this.getAttribute('type')||'text').toLowerCase(); }\n"
"    if (t==='button') { return (this.getAttribute('type')||'submit').toLowerCase(); }\n"
"    if (t==='select') { return this.hasAttribute('multiple') ? 'select-multiple' : 'select-one'; }\n"
"    if (t==='textarea') return 'textarea';\n"
"    return this.getAttribute('type') || ''; },\n"
"  function(v){ this.setAttribute('type', String(v)); });\n"
"function boolattr(name, attr){\n"
"  def(proto, name,\n"
"    function(){ return this.hasAttribute(attr); },\n"
"    function(v){ if (v) this.setAttribute(attr,''); else this.removeAttribute(attr); });\n"
"}\n"
"boolattr('disabled','disabled');\n"
"boolattr('readOnly','readonly');\n"
"boolattr('required','required');\n"
"boolattr('multiple','multiple');\n"
"boolattr('autofocus','autofocus');\n"
"def(proto, 'maxLength',\n"
"  function(){ var v = this.getAttribute('maxlength'); return v === null ? -1 : (parseInt(v,10)|0); },\n"
"  function(v){ this.setAttribute('maxlength', String(v|0)); });\n"
/* ---- selection ----------------------------------------------------------- */
"function selGet(el, which){\n"
"  if (!isField(el)) return null;\n"
"  if (!detached(el)) return which ? G.__fc_selend(key(el)) : G.__fc_selstart(key(el));\n"
"  return dget(el, which ? '__fcs1' : '__fcs0', (el.value || '').length);\n"
"}\n"
"function selSet(el, a, b){\n"
"  if (!isField(el)) return;\n"
"  var n = (el.value || '').length;\n"
"  a = a|0; b = b|0;\n"
"  if (a < 0) a = 0; if (a > n) a = n;\n"
"  if (b < 0) b = 0; if (b > n) b = n;\n"
"  if (b < a) b = a;\n"
"  if (detached(el)) { el.__fcs0 = a; el.__fcs1 = b; }\n"
"  else G.__fc_setsel(key(el), a, b);\n"
"}\n"
"def(proto, 'selectionStart',\n"
"  function(){ return selGet(this, 0); },\n"
"  function(v){ selSet(this, v, selGet(this, 1)); });\n"
"def(proto, 'selectionEnd',\n"
"  function(){ return selGet(this, 1); },\n"
"  function(v){ selSet(this, selGet(this, 0), v); });\n"
/* selectionDirection is stored and reported but does nothing: the caret has no
 * direction in this engine, and answering \"none\" for a field that was
 * selected backwards would be a wrong answer rather than a missing one. */
"def(proto, 'selectionDirection',\n"
"  function(){ return isField(this) ? dget(this, '__fcsd', 'none') : null; },\n"
"  function(v){ if (isField(this)) this.__fcsd = String(v); });\n"
"proto.setSelectionRange = function(a, b, d){\n"
"  if (!isField(this)) return;\n"
"  selSet(this, a, b);\n"
"  this.__fcsd = d ? String(d) : 'none';\n"
"  var ev = null;\n"
"  try { ev = new Event('select', { bubbles: true }); } catch (e) {}\n"
"  if (ev && this.dispatchEvent) this.dispatchEvent(ev);\n"
"};\n"
"proto.select = function(){\n"
"  if (!isField(this)) return;\n"
"  selSet(this, 0, (this.value || '').length);\n"
"  var ev = null;\n"
"  try { ev = new Event('select', { bubbles: true }); } catch (e) {}\n"
"  if (ev && this.dispatchEvent) this.dispatchEvent(ev);\n"
"};\n"
/* ---- focus --------------------------------------------------------------- */
"proto.focus = function(){ G.__fc_focus(key(this)); };\n"
"proto.blur  = function(){ G.__fc_blur(key(this)); };\n"
/* ---- <select> ------------------------------------------------------------ */
"def(proto, 'selectedIndex',\n"
"  function(){ return tag(this)==='select' ? G.__fc_selidx(key(this)) : undefined; },\n"
"  function(v){ if (tag(this)==='select') G.__fc_setselidx(key(this), v|0); });\n"
"def(proto, 'options', function(){\n"
"  if (tag(this)!=='select') return undefined;\n"
"  var out = [], st = [this];\n"
"  while (st.length) { var n = st.shift(), c = n.children||[];\n"
"    for (var i=0;i<c.length;i++){ var t=(c[i].tagName||'').toLowerCase();\n"
"      if (t==='option') out.push(c[i]); else if (t==='optgroup') st.push(c[i]); } }\n"
"  return out; });\n"
"def(proto, 'selected',\n"
"  function(){ if (tag(this)!=='option') return undefined;\n"
"    var s = this.parentNode; while (s && (s.tagName||'').toLowerCase()!=='select') s = s.parentNode;\n"
"    if (!s) return this.hasAttribute('selected');\n"
"    var o = s.options; for (var i=0;i<o.length;i++) if (o[i]===this) return i === s.selectedIndex;\n"
"    return false; },\n"
"  function(v){ if (tag(this)!=='option') return;\n"
"    var s = this.parentNode; while (s && (s.tagName||'').toLowerCase()!=='select') s = s.parentNode;\n"
"    if (!s) return;\n"
"    var o = s.options; for (var i=0;i<o.length;i++) if (o[i]===this) { s.selectedIndex = v ? i : -1; return; } });\n"
/* ---- <label> ------------------------------------------------------------- */
"def(proto, 'htmlFor',\n"
"  function(){ return this.getAttribute('for') || ''; },\n"
"  function(v){ this.setAttribute('for', String(v)); });\n"
/* ---- form / elements ----------------------------------------------------- */
"function ownerForm(el){\n"
"  var fid = el.getAttribute && el.getAttribute('form');\n"
"  if (fid) { var f = doc.getElementById(fid);\n"
"             return (f && (f.tagName||'').toLowerCase()==='form') ? f : null; }\n"
"  var p = el.parentNode;\n"
"  while (p && p.tagName) { if ((p.tagName||'').toLowerCase()==='form') return p; p = p.parentNode; }\n"
"  return null;\n"
"}\n"
"def(proto, 'form', function(){ return isCtl(this)||tag(this)==='fieldset' ? ownerForm(this) : undefined; });\n"
"def(proto, 'elements', function(){\n"
"  if (tag(this)!=='form') return undefined;\n"
"  var out = [], self = this;\n"
"  (function walk(n){ var c = n.children||[];\n"
"     for (var i=0;i<c.length;i++){ if (isCtl(c[i]) && ownerForm(c[i])===self) out.push(c[i]); walk(c[i]); } })(doc.documentElement||this);\n"
"  return out; });\n"

"proto.submit        = function(){ if (tag(this)==='form') G.__fc_submit(key(this), 0); };\n"
"proto.requestSubmit = function(sub){ if (tag(this)==='form')\n"
"                                       G.__fc_reqsubmit(key(this), sub ? key(sub) : 0); };\n"
"proto.reset         = function(){ if (tag(this)==='form') G.__fc_formreset(key(this)); };\n"
/* ---- document.activeElement ---------------------------------------------
 * Tracked from the events focus.c already dispatches rather than asked for
 * natively -- so if activeElement is right, focusin/focusout are right, and one
 * cannot silently drift from the other. */
/* Both pairs, and `focus`/`blur` in the CAPTURE phase specifically. They do not
 * bubble, but capture runs from the document down to the target whatever the
 * event's bubbles flag says -- so this listener runs BEFORE the element's own
 * focus handler, which is what makes `document.activeElement` already correct
 * inside it. Listening only on focusin (which fires after focus) left every
 * page that reads activeElement from a focus handler seeing the body. */
"var active = null;\n"
"doc.addEventListener('focus',    function(e){ active = e.target; }, true);\n"
"doc.addEventListener('focusin',  function(e){ active = e.target; }, true);\n"
"doc.addEventListener('blur',     function(e){ if (active === e.target) active = null; }, true);\n"
"doc.addEventListener('focusout', function(e){ if (active === e.target) active = null; }, true);\n"
"try { Object.defineProperty(doc, 'activeElement', { configurable: true,\n"
"  get: function(){ return active || doc.body || doc.documentElement; } }); } catch (e) {}\n"
"try { Object.defineProperty(doc, 'forms', { configurable: true, get: function(){\n"
"  var out = [];\n"
"  (function walk(n){ var c = n.children||[];\n"
"     for (var i=0;i<c.length;i++){ if ((c[i].tagName||'').toLowerCase()==='form') out.push(c[i]); walk(c[i]); } })(doc.documentElement||doc);\n"
"  return out; } }); } catch (e) {}\n"
"})(globalThis);\n";

void js_forms_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyFunctionList(ctx, g, g_natives,
                               (int)(sizeof g_natives / sizeof g_natives[0]));
    JS_FreeValue(ctx, g);
    JSValue r = JS_Eval(ctx, SHIM, sizeof SHIM - 1, "<forms>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        if (m) { int printf(const char *, ...); printf("[forms] shim failed: %s\n", m);
                 JS_FreeCString(ctx, m); }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    for (int i = 0; i < FC_CACHE; i++) { g_cache[i].id = 0; g_cache[i].n = 0; }
}
