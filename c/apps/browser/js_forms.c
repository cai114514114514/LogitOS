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
 *   delete this whole mechanism. That was the original ASK; js_dom_node_from
 *   now exists. The select hooks use it for wrapper arguments so a switcher
 *   configured before insertion is not lost to the document-only key walk.
 *   Other hooks retain the old integer protocol here.
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
#include "css.h"
#include "../../../include/weaksym.h"
/* Focus-only host embeddings can omit the style engine. Shipping builds use
 * the same synchronous style door as computed-style reads. */
extern void css_ensure_styled(struct node *) LOGIT_WEAK;
LOGIT_WEAK_STUB(css_ensure_styled);
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
#ifndef FORM_SELECTION_RAW_BYTES
    if (magic == 2) return JS_NewInt32(ctx, fc_selection_direction(n));
    int len = 0;
    const char *value = fc_value(n, &len);
    int offset = magic ? b : a;
    if (offset > len) offset = len;
    return JS_NewInt32(ctx, (int)dom_utf16_length(value, (unsigned)offset));
#else
    if (magic == 2) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, magic ? b : a);
#endif
}

/* JS text controls count UTF-16 units; native editing and painting count UTF-8
 * bytes. Guest evidence: a 600-unit CJK value reported End=1800 and Left=1797.
 * Count through complete scalars, including two units for astral characters.
 * The byte editor cannot represent the middle of a surrogate pair: clamp that
 * endpoint to the scalar's leading boundary, deliberately preserving text
 * instead of creating an invalid UTF-8 caret. Exact half-surrogate selection
 * remains absent until the native position representation can express it. */
static int selection_byte_offset(const char *value, int len, uint32_t units)
{
    int at = 0;
    uint32_t used = 0;
    while (at < len) {
        int next = at + 1;
        while (next < len && ((unsigned char)value[next] & 0xc0) == 0x80) next++;
        unsigned span = dom_utf16_length(value + at, (unsigned)(next - at));
        if (span > units - used) break;
        used += span; at = next;
    }
    return at;
}

static JSValue jf_setsel(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 3) return JS_UNDEFINED;
    struct node *n = node_of(arg_id(ctx, argv[0]));
#ifdef FORM_SELECTION_RAW_BYTES
    int32_t a = 0, b = 0;
    JS_ToInt32(ctx, &a, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &b, argv[2]);
    else b = a;
    if (n) fc_set_selection(n, a, b);
#else
    uint32_t a = 0, b = 0;
    int32_t direction = 0;
    if (JS_ToUint32(ctx, &a, argv[1]) || JS_ToUint32(ctx, &b, argv[2])) return JS_EXCEPTION;
    if (argc > 3 && JS_ToInt32(ctx, &direction, argv[3])) return JS_EXCEPTION;
    if (n) {
        int len = 0;
        const char *value = fc_value(n, &len);
        int start = selection_byte_offset(value, len, a);
        int end = selection_byte_offset(value, len, b);
        if (start > end) start = end;
        fc_set_selection_directed(n, start, end, direction);
        /* A second programmatic selection need not stamp an attribute or
         * change value. Without its own paint invalidation the native caret
         * moves in memory while the previous caret remains on screen. */
        js_dom_control_changed(n);
    }
#endif
    return JS_UNDEFINED;
}

static JSValue jf_selectall(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc;
    struct node *n = node_of(arg_id(ctx, argv[0]));
    if (n) fc_edit_select_all(n);
    return JS_UNDEFINED;
}

static struct node *selection_node(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1) return 0;
    return JS_IsObject(argv[0]) ? js_dom_node_from(argv[0])
                               : node_of(arg_id(ctx, argv[0]));
}

static JSValue jf_selidx(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; return JS_NewInt32(ctx, fc_selected_index(selection_node(ctx, argc, argv))); }

static JSValue jf_setselidx(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 2) return JS_UNDEFINED;
    struct node *n = selection_node(ctx, argc, argv);
    int32_t i = -1;
    JS_ToInt32(ctx, &i, argv[1]);
    if (n) {
        int old = fc_selected_index(n);
        fc_set_selected_index(n, i);
        if (old != fc_selected_index(n)) js_dom_control_changed(n);
    }
    return JS_UNDEFINED;
}

static JSValue jf_optsel(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; return JS_NewBool(ctx, fc_option_selected(selection_node(ctx, argc, argv))); }

static JSValue jf_setoptsel(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc >= 2) {
        struct node *n = selection_node(ctx, argc, argv);
        fc_set_option_selected(n, JS_ToBool(ctx, argv[1]));
        for (struct node *p = n ? n->parent : 0; p; p = p->parent)
            if (fc_kind(p) == FC_SELECT) { js_dom_control_changed(p); break; }
    }
    return JS_UNDEFINED;
}

static JSValue jf_resetsel(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    struct node *n = selection_node(ctx, argc, argv);
    if (n) { fc_reset_control(n); js_dom_control_changed(n); }
    return JS_UNDEFINED;
}

/* magic: 0 focus, 1 blur */
static JSValue jf_focus(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv, int magic)
{
    (void)t; (void)argc;
    struct node *n = argc > 0 && JS_IsObject(argv[0]) ? js_dom_node_from(argv[0])
                                                      : argc > 0 ? node_of(arg_id(ctx, argv[0])) : 0;
    if (magic) { if (focus_current() == n) focus_set(0); }
    else if (n) {
        /* Detached focus must be a no-op, and must not make the style cache
         * adopt a detached subtree as its author-sheet document. */
        struct node *root = n;
        while (root->parent) root = root->parent;
        if (root != js_dom_root()) return JS_UNDEFINED;
#ifndef FOCUS_NO_STYLE_FLUSH
        /* A script may insert a display:none ancestor and focus its child in
         * the same call stack, before browser.c gets its next settle turn.
         * Null/stale cstyle is not evidence of visibility. The old JS fallback
         * inspected inline styles; routing focus to its native owner exposed
         * this missing native flush even with both recent CSS optimizations
         * disabled (semantics: 89 checks, the same one failure).
         * css_ensure_styled updates style only and DOES NOT clear js_dom_dirty,
         * so the embedder still rebuilds/paints its display list afterwards. */
        if (LOGIT_HAVE(css_ensure_styled)) css_ensure_styled(n);
#endif
        /* A refused focus request preserves the prior active element. Passing
         * an unrendered target to focus_set turns it into an explicit blur. */
        if (!focus_is_focusable(n)) return JS_UNDEFINED;
        struct node *old = focus_current();
        uint32_t old_serial = old ? old->serial : 0, serial = n->serial;
        focus_set(n);
        if (focus_current() == n && n->serial == serial) {
            /* Focus handlers can destroy/recycle either node synchronously.
             * Commit only the old identity, never a new element in its slot. */
            if (old && old != n && old->serial == old_serial && fc_kind(old) != FC_NONE) fc_commit(old);
            if (fc_kind(n) != FC_NONE) fc_mark_focus(n);
        }
    }
    return JS_UNDEFINED;
}

static JSValue jf_active(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    /* Correction: the old stamped-id fallback and event-maintained JS cache
     * drifted when inert/detach cleared native focus without a focus event. */
    return js_dom_node_value(ctx, focus_current());
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

/* ============================== the editing events, with an inputType ===== *
 *
 * focus.h's second dispatch seam lands here. js_dom.c raises every event it
 * knows about out of a fixed `struct js_event_init`, which has no inputType and
 * no data, and that file is another line's -- so an `input` event raised
 * through it is an input event a React composer cannot read.
 *
 * js_events.c already defines a real `InputEvent` with both fields (it is in
 * its makeSub table). So the event is CONSTRUCTED IN THE PAGE'S OWN RUNTIME and
 * dispatched with the ordinary dispatchEvent -- the same object a page would
 * build itself, carrying the right inputType, going through the same
 * capture/target/bubble walk as everything else.
 *
 * WHAT THAT COSTS, said plainly: the event's isTrusted is false, because it did
 * not come from js_dom_dispatch's trusted path. No framework's input handling
 * reads isTrusted (it is a security signal for click and key, not for input),
 * but it is a real difference and the honest fix is one field on
 * `struct js_event_init` -- an ASK for the js_dom line, three lines long, at
 * which point this whole indirection deletes itself. */
static JSContext *g_ctx;

/* Give a node an integer key, reading back the one it already has. The C
 * counterpart of the shim's key(): the same attribute, the same numbering. */
static int stamp_id(struct node *n)
{
    if (!n || n->type != N_ELEM) return 0;
    const char *v = dom_attr(n, "data-logit-fcid");
    if (v && v[0]) {
        int id = 0;
        for (const char *p = v; *p >= '0' && *p <= '9'; p++) id = id * 10 + (*p - '0');
        if (id > 0) return id;
    }
    int id = g_next_id++;
    char buf[16];
    int p = 0, x = id;
    char t[12]; int i = 0;
    while (x) { t[i++] = (char)('0' + x % 10); x /= 10; }
    while (i) buf[p++] = t[--i];
    buf[p] = 0;
    if (!dom_set_attr(n, "data-logit-fcid", buf)) return 0;
    return id;
}

static int fire_input(struct node *target, const char *type, const char *itype,
                      const char *data, int bubbles, int cancelable)
{
    if (!g_ctx || !target) return 1;
    int id = stamp_id(target);
    if (id <= 0) return 1;
    JSContext *ctx = g_ctx;
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, g, "__fcFireInput");
    int ok = 1;
    if (JS_IsFunction(ctx, fn)) {
        JSValue a[6];
        a[0] = JS_NewInt32(ctx, id);
        a[1] = JS_NewString(ctx, type);
        a[2] = JS_NewString(ctx, itype ? itype : "");
        a[3] = data ? JS_NewString(ctx, data) : JS_NULL;
        a[4] = JS_NewBool(ctx, bubbles);
        a[5] = JS_NewBool(ctx, cancelable);
        JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 6, a);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        } else {
            /* The shim returns dispatchEvent's answer: false means a listener
             * called preventDefault, which for beforeinput means the edit must
             * not happen. */
            ok = JS_ToBool(ctx, r) != 0;
        }
        JS_FreeValue(ctx, r);
        for (int i = 0; i < 6; i++) JS_FreeValue(ctx, a[i]);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, g);
    return ok;
}

/* ================================ Selection and Range ==================== *
 *
 * A contenteditable composer is nearly always read back through
 * document.getSelection(), so the editing model is only half a feature without
 * it. What crosses the boundary is a PATH of child indices from the document
 * element, for the reason forms.h gives: js_dom.c exports no way to turn a
 * `struct node *` into a JS wrapper, and unlike an element a TEXT node cannot
 * even carry the `data-logit-fcid` attribute the workaround above uses.
 *
 * OFFSETS ARE CONVERTED, not passed through. The DOM counts a Range offset in
 * UTF-16 code units; forms.c holds UTF-8 bytes. They agree for ASCII and
 * disagree for every accented or CJK character, and a selection API that is
 * silently wrong on non-ASCII text is worse than one that is absent. The
 * conversion is in the shim, where the string is. */
static int read_path(JSContext *ctx, JSValueConst v, int *out, int max)
{
    if (!JS_IsArray(ctx, v)) return -1;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    if ((int)len > max) return -1;
    for (uint32_t i = 0; i < len; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        int32_t x = 0;
        JS_ToInt32(ctx, &x, e);
        JS_FreeValue(ctx, e);
        out[i] = (int)x;
    }
    return (int)len;
}

static JSValue jf_selget(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    int ap[64], fp[64], ao = 0, fo = 0;
    int ad = fc_ce_path(0, ap, 64, &ao);
    int fd = fc_ce_path(1, fp, 64, &fo);
    if (ad < 0 || fd < 0) return JS_NULL;
    JSValue a = JS_NewArray(ctx);
    uint32_t k = 0;
    JS_DefinePropertyValueUint32(ctx, a, k++, JS_NewInt32(ctx, ad), JS_PROP_C_W_E);
    for (int i = 0; i < ad; i++)
        JS_DefinePropertyValueUint32(ctx, a, k++, JS_NewInt32(ctx, ap[i]), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, a, k++, JS_NewInt32(ctx, ao), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, a, k++, JS_NewInt32(ctx, fd), JS_PROP_C_W_E);
    for (int i = 0; i < fd; i++)
        JS_DefinePropertyValueUint32(ctx, a, k++, JS_NewInt32(ctx, fp[i]), JS_PROP_C_W_E);
    JS_DefinePropertyValueUint32(ctx, a, k++, JS_NewInt32(ctx, fo), JS_PROP_C_W_E);
    return a;
}

static JSValue jf_selset(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t;
    if (argc < 4) return JS_FALSE;
    int ap[64], fp[64];
    int ad = read_path(ctx, argv[0], ap, 64);
    int fd = read_path(ctx, argv[2], fp, 64);
    if (ad < 0 || fd < 0) return JS_FALSE;
    int32_t ao = 0, fo = 0;
    JS_ToInt32(ctx, &ao, argv[1]);
    JS_ToInt32(ctx, &fo, argv[3]);
    /* forms.c's ce_root() -- the function fc_ce_set_paths resolves both paths
     * against -- has exactly three sources: a live g_ce_f, a live g_ce_a, or
     * the root hint fc_ce_set_root() stamps down when a control gets REAL
     * focus. A page that builds a Range and calls Selection.addRange() on a
     * contenteditable it never .focus()'d (which is legal -- selecting text
     * does not require focus() first, and this is exactly what
     * editing/include/tests.js does for every one of its generated cases) has
     * none of the three: ce_root() returns 0, fc_ce_set_paths refuses, and
     * every query of the selection this call was supposed to establish reads
     * back empty forever. `__fc_selSet` is the one native that always carries
     * a real node to seed the hint from (the path itself resolves against
     * js_dom_root() one line below), so it is refreshed here on every call --
     * cheap (a parent-chain walk to the document element, already paid for
     * inside fc_ce_set_paths itself) and it costs nothing on the path where a
     * hint already exists (fc_ce_set_root only overwrites it). */
    fc_ce_set_root(js_dom_root());
    return JS_NewBool(ctx, fc_ce_set_paths(ap, ad, ao, fp, fd, fo));
}

static JSValue jf_selclear(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{ (void)t; (void)argc; (void)argv; fc_ce_clear(); return JS_UNDEFINED; }

static JSValue jf_seltext(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv)
{
    (void)t; (void)argc; (void)argv;
    static char buf[8192];
    int n = fc_ce_selection_text(buf, (int)sizeof buf);
    return JS_NewStringLen(ctx, buf, (size_t)(n < 0 ? 0 : n));
}

#include "js_live_range_native.inc"

static const JSCFunctionListEntry g_natives[] = {
    JS_CFUNC_DEF("__fc_rangeNew", 0, jf_range_new),
    JS_CFUNC_MAGIC_DEF("__fc_rangeSC", 1, jf_range_get, 0),
    JS_CFUNC_MAGIC_DEF("__fc_rangeSO", 1, jf_range_get, 1),
    JS_CFUNC_MAGIC_DEF("__fc_rangeEC", 1, jf_range_get, 2),
    JS_CFUNC_MAGIC_DEF("__fc_rangeEO", 1, jf_range_get, 3),
    JS_CFUNC_MAGIC_DEF("__fc_rangeStart", 3, jf_range_set, 0),
    JS_CFUNC_MAGIC_DEF("__fc_rangeEnd", 3, jf_range_set, 1),
    JS_CFUNC_DEF("__fc_rangeCompare", 4, jf_range_compare),
    JS_CFUNC_DEF("__fc_selGet", 0, jf_selget),
    JS_CFUNC_DEF("__fc_selSet", 4, jf_selset),
    JS_CFUNC_DEF("__fc_selClear", 0, jf_selclear),
    JS_CFUNC_DEF("__fc_selText", 0, jf_seltext),
    JS_CFUNC_DEF("__fc_newid", 0, jf_newid),
    JS_CFUNC_DEF("__fc_kind", 1, jf_kind),
    JS_CFUNC_DEF("__fc_value", 1, jf_value),
    JS_CFUNC_DEF("__fc_defvalue", 1, jf_default_value),
    JS_CFUNC_DEF("__fc_setvalue", 2, jf_setvalue),
    JS_CFUNC_DEF("__fc_checked", 1, jf_checked),
    JS_CFUNC_DEF("__fc_setchecked", 2, jf_setchecked),
    JS_CFUNC_MAGIC_DEF("__fc_selstart", 1, jf_selpos, 0),
    JS_CFUNC_MAGIC_DEF("__fc_selend", 1, jf_selpos, 1),
    JS_CFUNC_MAGIC_DEF("__fc_seldirection", 1, jf_selpos, 2),
    JS_CFUNC_DEF("__fc_setsel", 4, jf_setsel),
    JS_CFUNC_DEF("__fc_selectall", 1, jf_selectall),
    JS_CFUNC_DEF("__fc_selidx", 1, jf_selidx),
    JS_CFUNC_DEF("__fc_setselidx", 2, jf_setselidx),
    JS_CFUNC_DEF("__fc_optsel", 1, jf_optsel),
    JS_CFUNC_DEF("__fc_setoptsel", 2, jf_setoptsel),
    JS_CFUNC_DEF("__fc_resetsel", 1, jf_resetsel),
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
/* ONE PROTOTYPE PER TAG, and this file used to install onto exactly one of
 * them. It took `Object.getPrototypeOf(doc.createElement('input'))` and put
 * every member below on it, which was right while js_dom.c had ONE shared
 * element prototype -- and every accessor here is written for that world: they
 * are tag-guarded, `selectedIndex` answers undefined unless tag(this) is
 * 'select', `htmlFor` is a <label>'s, `submit` is a <form>'s.
 *
 * 7fc2bec gave each tag its own prototype chained under HTMLElement. From that
 * commit the whole file landed on HTMLInputElement.prototype and NOWHERE ELSE:
 * select.value, textarea.value, textarea.selectionStart, button.value,
 * select.selectedIndex, select.options, option.selected, label.htmlFor,
 * el.form, form.elements, form.submit() and form.reset() all disappeared. A
 * silent, total regression on every control that is not an <input>.
 *
 * js_dom_iface.inc names the seam the other outside-installers use for this
 * (js_select.c, js_platform.c, js_media.c): install on the <div> prototype and
 * iface_bridge() relocates it onto Element.prototype. That is right for a
 * member EVERY element has and wrong for these -- putting `value`, `type` and
 * `disabled` on Element.prototype would give them to <div> and <span>, and
 * js_reflect.c's per-interface reflections exist precisely so that does not
 * happen. So the members below go on each SERVED tag's own prototype, and only
 * focus()/blur() -- which really are HTMLElement's, and which every element
 * therefore needs -- take the div/bridge seam.
 *
 * A prototype is skipped when it IS HTMLElement.prototype: that is what a tag
 * with no dedicated interface answers, and installing there is the clobber the
 * paragraph above is avoiding. Every tag in TAGS has one today (the interface
 * table in js_dom_iface.inc lists all twelve); the guard is for the day one is
 * removed, because the failure without it is invisible. */
"function protoOf(t){ try { var e = doc.createElement(t);\n"
"    return e ? Object.getPrototypeOf(e) : null; } catch (x) { return null; } }\n"
"var HTMLEP = (G.HTMLElement && G.HTMLElement.prototype) || null;\n"
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
/* ---- FileList, and why <input type=file>.files is always EMPTY -----------
 *
 * MEASURED: jsfb_matrix's work order names one implementation stopped cold by
 * `ReferenceError: FileList is not defined` -- a bare reference to the global
 * constructor, the same shape NodeList/HTMLCollection needed (js_select.c
 * already carries that fix for those two; this file is FileList's, because
 * `.files` is a control's property, the same family as `.value`/`.checked`
 * two sections up, not a query-collection).
 *
 * The honest answer for .files on this machine is an always-EMPTY, but REAL,
 * FileList -- not absent, not a plain array pretending to be one. There is no
 * native file-picker anywhere in this tree (grep -rn 'file.*picker\\|choose.*file'
 * across c/kernel/gui and c/apps/gui finds nothing), so a user can never
 * actually select a file through an <input type=file>; "zero files chosen" is
 * not a limitation being papered over, it is the SAME state a real browser
 * reports before its picker has ever been used. Answering it with a genuine
 * FileList (constructor identity, .length, .item(), numeric indices,
 * iterable) rather than leaving `.files` undefined means `input.files
 * instanceof FileList`, `for (var f of input.files)` and `input.files.length
 * === 0` all get the SAME true answer a real browser gives an untouched
 * input, instead of a page's own `.files &&` guard taking the wrong branch.
 * What is NOT here: DataTransfer.files (drag-and-drop has no source on this
 * machine either) and any path that could ever populate this list with a
 * real File -- both absent by name rather than guessed at, per rule 1. */
"if (!('FileList' in G)) {\n"
"  var FileList = function FileList(){ throw new TypeError('Illegal constructor'); };\n"
"  var FLP = { length: 0 };\n"
"  FLP.item = function(i){ i = i|0; return (i >= 0 && i < this.length && this[i] !== undefined) ? this[i] : null; };\n"
"  try { FLP[Symbol.iterator] = Array.prototype[Symbol.iterator]; } catch (e) {}\n"
"  FileList.prototype = FLP;\n"
"  try { Object.defineProperty(FileList, 'name', { value: 'FileList', configurable: true }); } catch (e) {}\n"
"  G.FileList = FileList;\n"
"}\n"
/* Correction (2026-09-10): empty does not mean readonly. A real page reached
 * its 10,589,862-byte module, then stopped rendering at a strict assignment
 * with "no setter for property 'files'". HTML's nullable FileList setter must
 * accept null/undefined (no change) and another genuine FileList (same object).
 * https://html.spec.whatwg.org/multipage/input.html#dom-input-files
 *
 * Keep both identity and the brand private: instanceof accepts forged
 * Object.create(FileList.prototype), and the former __fcFiles expando let page
 * code supply arbitrary arrays. This realm's only producer remains the empty
 * list below. A future picker/DataTransfer/cross-realm bridge must register its
 * real lists here AND connect value/reset/submission to native selected files;
 * do not accept an array and invent selected file bytes. */
"var fileLists = new WeakMap(), fileListBrand = new WeakSet();\n"
"var fileListProto = G.FileList.prototype;\n"
"function isFile(el){ return tag(el)==='input' && (el.getAttribute('type')||'text').toLowerCase()==='file'; }\n"
"function emptyFileList(){\n"
"  var fl = Object.create(fileListProto);\n"
"  try { Object.defineProperty(fl, 'length', { value: 0 }); } catch (e) {}\n"
"  fileListBrand.add(fl);\n"
"  return fl;\n"
"}\n"
/* Everything from here to the end of installOn() is per-served-tag. `proto` is
 * the parameter, not a closure over one prototype, which is the whole change:
 * `def`'s "if it already resolves, its owner keeps it" rule is now evaluated
 * against the RIGHT chain -- <input> keeps js_reflect.c's reflected `type` and
 * `defaultValue`, <select> keeps its own `name` and `multiple`, and each still
 * gains the members reflection cannot produce. */
"function installOn(proto){\n"
/* ---- value / defaultValue ------------------------------------------------ */
"function hasValue(el){ var t = tag(el);\n"
"  return t==='input'||t==='select'||t==='textarea'||t==='button'||\n"
"         t==='option'||t==='li'||t==='data'||t==='param'||t==='progress'||t==='meter'; }\n"
"def(proto, 'value',\n"
"  function(){ if (!hasValue(this)) return undefined;\n"
/* No picker can select a file yet. Do not let a markup value or a text editor
 * expando masquerade as a selected path. Clearing an already-empty list keeps
 * its identity; no input/change events accompany script assignment. */
"    if (isFile(this)) return '';\n"
"    if (isCtl(this)) {\n"
"      if (!detached(this)) return G.__fc_value(key(this));\n"
"      if ('__fcv' in this) return this.__fcv;\n"
"      if (tag(this)==='textarea') return this.textContent || '';\n"
"      var a = this.getAttribute('value');\n"
"      return a === null ? '' : a;\n"
"    }\n"
"    var v = this.getAttribute('value');\n"
"    return v === null ? (tag(this)==='option' ? this.textContent : '') : v; },\n"
"  function(v){\n"
"    if (isFile(this)) {\n"
"      if (v !== null && String(v) !== '') throw new DOMException('File input value can only be cleared', 'InvalidStateError');\n"
"      return;\n"
"    }\n"
"    var s = (v === null || v === undefined) ? '' : String(v);\n"
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
/* .files -- see the FileList comment above installOn for why this is always
 * a real, empty FileList rather than absent or a plain array. Guarded on
 * type=file the same way `checked` is guarded on tag==='input': every other
 * <input> type answers undefined, matching the spec (files is declared only
 * on the file state's IDL attributes).
 * Correction: the old "undefined, matching the spec" sentence is wrong.
 * The property belongs to HTMLInputElement for EVERY type; non-file inputs
 * return null. Web IDL validates a non-null assignment before HTML's no-op
 * for other types, so even a text input must reject an array. */
"if (proto === protoOf('input')) def(proto, 'files',\n"
"  function(){\n"
"    if (tag(this)!=='input') throw new TypeError('Illegal invocation');\n"
"    if (!isFile(this)) return null;\n"
"    if (!fileLists.has(this)) fileLists.set(this, emptyFileList());\n"
"    return fileLists.get(this); }\n"
#ifndef FORM_FILES_GETTER_ONLY
", function(v){\n"
"    if (tag(this)!=='input') throw new TypeError('Illegal invocation');\n"
"    if (v === null || v === undefined) return;\n"
"    if (!fileListBrand.has(v)) throw new TypeError('files requires a FileList');\n"
"    if (isFile(this)) fileLists.set(this, v);\n"
"  }\n"
#endif
");\n"
"def(proto, 'maxLength',\n"
"  function(){ var v = this.getAttribute('maxlength'); return v === null ? -1 : (parseInt(v,10)|0); },\n"
"  function(v){ this.setAttribute('maxlength', String(v|0)); });\n"
/* ---- selection ----------------------------------------------------------- */
"function selGet(el, which){\n"
"  if (!isField(el)) return null;\n"
"  if (!detached(el)) return which ? G.__fc_selend(key(el)) : G.__fc_selstart(key(el));\n"
"  return dget(el, which ? '__fcs1' : '__fcs0', (el.value || '').length);\n"
"}\n"
"function selDir(el){\n"
"  if (!isField(el)) return null;\n"
"  if (detached(el)) return dget(el, '__fcsd', 'none');\n"
"  var d = G.__fc_seldirection(key(el)); return d < 0 ? 'backward' : d > 0 ? 'forward' : 'none';\n"
"}\n"
"function selSet(el, a, b, d){\n"
"  if (!isField(el)) return;\n"
"  var n = (el.value || '').length;\n"
/* Web IDL unsigned-long conversion comes before value-length clamping: -1
 * wraps and clamps to End; it is not a signed request for the start. */
"  a = a>>>0; b = b>>>0;\n"
"  if (a > n) a = n; if (b > n) b = n;\n"
"  if (b < a) a = b;\n"
"  d = String(d); if (d !== 'forward' && d !== 'backward') d = 'none';\n"
"  if (detached(el)) { el.__fcs0 = a; el.__fcs1 = b; el.__fcsd = d; }\n"
"  else G.__fc_setsel(key(el), a, b, d === 'backward' ? -1 : d === 'forward' ? 1 : 0);\n"
"}\n"
"def(proto, 'selectionStart',\n"
"  function(){ return selGet(this, 0); },\n"
"  function(v){ var a=v>>>0; selSet(this, a, Math.max(a, selGet(this, 1)), selDir(this)); });\n"
"def(proto, 'selectionEnd',\n"
"  function(){ return selGet(this, 1); },\n"
"  function(v){ selSet(this, selGet(this, 0), v, selDir(this)); });\n"
/* selectionDirection is stored and reported but does nothing: the caret has no
 * direction in this engine, and answering \"none\" for a field that was
 * selected backwards would be a wrong answer rather than a missing one. */
/* Correction: native selection now keeps this direction and its anchor, so
 * changing direction changes subsequent Shift+Arrow and the painted caret. */
"def(proto, 'selectionDirection',\n"
"  function(){ return selDir(this); },\n"
"  function(v){ selSet(this, selGet(this, 0), selGet(this, 1), v); });\n"
"proto.setSelectionRange = function(a, b, d){\n"
"  if (!isField(this)) return;\n"
"  selSet(this, a, b, d);\n"
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
/* focus()/blur() are NOT here: they are HTMLElement's, every element needs
 * them (an <a>, anything with tabindex), and they go on the div/bridge seam
 * below so they reach Element.prototype. */
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
"}\n"
/* The served tags. Not "every tag": each member above is guarded on tag(this),
 * so a prototype that no guard can ever match would only carry dead accessors
 * -- and, on a shared prototype, would shadow the reflected ones. */
"var TAGS = ['input','select','textarea','button','option','optgroup',\n"
"            'form','label','fieldset','output','datalist','legend'];\n"
"for (var ti = 0; ti < TAGS.length; ti++) {\n"
"  var p = protoOf(TAGS[ti]);\n"
"  if (!p || p === HTMLEP || p.__fcInstalled) continue;\n"
"  p.__fcInstalled = true;\n"
"  installOn(p);\n"
"}\n"
/* focus()/blur() for EVERY element, through the seam js_dom_iface.inc
 * documents: what lands on the <div> prototype is moved down to
 * Element.prototype by iface_bridge() on the next wrapper the page makes.
 * `def`, not assignment -- js_platform.c is another line's file and may have
 * published a focus() of its own, and shadowing a working one is the failure
 * the NEVER CLOBBER note above describes. */
"var DP = protoOf('div');\n"
"if (DP && DP !== HTMLEP && !DP.__fcFocus) {\n"
"  DP.__fcFocus = true;\n"
"  if (!('focus' in DP)) DP.focus = function(){ G.__fc_focus(key(this)); };\n"
"  if (!('blur'  in DP)) DP.blur  = function(){ G.__fc_blur(key(this)); };\n"
"}\n"
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
/* Correction: events describe transitions; they are not the owner of focus.
 * The native holder also validates serials and dynamic inert ancestors. */
"try { Object.defineProperty(doc, 'activeElement', { configurable: true,\n"
"  get: function(){ return G.__fc_active() || doc.body || doc.documentElement; } }); } catch (e) {}\n"
"try { Object.defineProperty(doc, 'forms', { configurable: true, get: function(){\n"
"  var out = [];\n"
"  (function walk(n){ var c = n.children||[];\n"
"     for (var i=0;i<c.length;i++){ if ((c[i].tagName||'').toLowerCase()==='form') out.push(c[i]); walk(c[i]); } })(doc.documentElement||doc);\n"
"  return out; } }); } catch (e) {}\n"
/* document.defaultView USED to be defined here as `Object.defineProperty(doc,
 * 'defaultView', { get: function(){ return G; } })` -- an own property on the
 * live `document` instance that unconditionally answered the global object.
 * ONE JAR, TWO DOORS: js_dom_iface.inc's document_extra_funcs[] now installs
 * the real accessor on Document.prototype, and an own property on `doc` would
 * SHADOW it there -- silently, since both compute the same answer for the
 * live document and only diverge for a document this file never sees
 * (createHTMLDocument's detached tree, where the correct answer is NULL, not
 * G). Two implementations of one property that happen to agree today is
 * exactly the shape rule 3 in CLAUDE.md warns about, so this door is closed;
 * see js_dom_iface.inc's docx_get_defaultView for the one that stays open. */
"})(globalThis);\n";


/* ================================ the Selection / Range shim ============== *
 *
 * Its own string, evaluated after the one above, so a syntax error in either
 * cannot take the other down -- the forms bindings shipped first and must not
 * become dependent on this landing cleanly.
 *
 * WHAT IS HERE AND WHAT IS NOT, stated so the gaps are not discovered by a
 * page. Implemented: document.getSelection / window.getSelection, the Selection
 * members a composer reads (anchorNode/anchorOffset/focusNode/focusOffset,
 * isCollapsed, rangeCount, type, toString, getRangeAt, collapse,
 * collapseToStart/End, extend, setBaseAndExtent, removeAllRanges/empty,
 * addRange, selectAllChildren, containsNode), document.createRange, and a Range
 * with the container/offset pairs, collapsed, commonAncestorContainer, setStart
 * / setEnd (+Before/After), collapse, selectNode, selectNodeContents,
 * cloneRange and toString.
 *
 * NOT implemented, deliberately: Range.insertNode / deleteContents /
 * extractContents / cloneContents / surroundContents (a page that edits through
 * a Range instead of through the keyboard is doing something no composer does),
 * Range.compareBoundaryPoints, Selection.modify, and
 * Range.getBoundingClientRect / getClientRects -- a rect this cannot compute
 * correctly would be worse than an absent method, because a popup positioner
 * would believe it. Nothing here is a silent stub: what is missing is missing.
 *
 * document.execCommand and document.designMode are NOT here either, and that is
 * a measurement rather than an omission -- both grep to nothing across the
 * corpus this browser is aimed at. Building them speculatively would be two
 * more surfaces to keep true. */
/* Correction (2026-09-09): the preceding absence claim is retained as history.
 * Range now owns native live boundaries and exposes compareBoundaryPoints,
 * clone/delete/extractContents and insertNode. js_live_range_shim.inc consumes
 * the C mutation stream; forms.c keyboard edits and CharacterData methods use
 * the same UTF-16 replacement primitive. Range geometry, surroundContents and
 * Selection.modify remain absent. Selection still maps to the native caret;
 * addRange/getRangeAt do not yet preserve the identity of an associated Range.
 * Cross-document boundaries and Document-level insertNode explicitly refuse;
 * moving one endpoint across detached/shadow roots collapses into that root. */
static const char SEL_SHIM[] =
"(function(G){\n"
"var doc = G.document; if (!doc) return;\n"
"function root(){ return doc.documentElement; }\n"
/* previousSibling in this engine is the previous ELEMENT sibling (js_dom.c),
 * which would skip the text nodes a caret lives in and produce a path that
 * addresses the wrong child. So the index is taken from childNodes, which is
 * every node in order, and identity works because js_dom.c caches one wrapper
 * per node. */
"function idx(p, n){ var cs = p.childNodes || []; \n"
"  for (var i = 0; i < cs.length; i++) if (cs[i] === n) return i;\n"
"  return -1; }\n"
"function pathOf(n){\n"
"  var r = root(), p = [];\n"
"  while (n && n !== r) { var q = n.parentNode; if (!q) return null;\n"
"    var i = idx(q, n); if (i < 0) return null; p.unshift(i); n = q; }\n"
"  return n === r ? p : null; }\n"
"function nodeAt(p){ var n = root();\n"
"  for (var i = 0; i < p.length && n; i++) n = (n.childNodes || [])[p[i]];\n"
"  return n || null; }\n"
/* UTF-16 code units (what the DOM counts) <-> UTF-8 bytes (what forms.c
 * holds). Identical for ASCII, different for every accented or CJK character,
 * and a selection API that is silently wrong on non-ASCII is worse than one
 * that is missing. */
"function b2u(s, b){ var by = 0, u = 0;\n"
"  for (u = 0; u < s.length && by < b; u++) { var c = s.charCodeAt(u);\n"
"    if (c < 0x80) by += 1; else if (c < 0x800) by += 2;\n"
"    else if (c >= 0xD800 && c < 0xDC00) { by += 4; u++; } else by += 3; }\n"
"  return u; }\n"
"function u2b(s, u){ var by = 0;\n"
"  for (var i = 0; i < s.length && i < u; i++) { var c = s.charCodeAt(i);\n"
"    if (c < 0x80) by += 1; else if (c < 0x800) by += 2;\n"
"    else if (c >= 0xD800 && c < 0xDC00) { by += 4; i++; } else by += 3; }\n"
"  return by; }\n"
"function offOut(n, bytes){ return (n && n.nodeType === 3) ? b2u(n.data || '', bytes) : bytes; }\n"
"function offIn(n, units){ return (n && n.nodeType === 3) ? u2b(n.data || '', units) : units; }\n"
/* Document order from two paths: lexicographic on the child indices, with the
 * shorter path (an ancestor) first. No tree walk needed and no second copy of
 * the comparison in C. */
"function cmpPath(a, b, ao, bo){\n"
"  var n = Math.min(a.length, b.length);\n"
"  for (var i = 0; i < n; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;\n"
"  if (a.length !== b.length) {\n"
"    var shortIsA = a.length < b.length;\n"
"    var deep = shortIsA ? b : a, off = shortIsA ? ao : bo;\n"
"    var r = off <= deep[n] ? -1 : 1;\n"
"    return shortIsA ? r : -r; }\n"
"  return ao < bo ? -1 : ao > bo ? 1 : 0; }\n"
/* The live caret, decoded. NULL when there is none -- which is the honest
 * answer for a page with nothing focused, and what rangeCount 0 means. */
"function cur(){\n"
"  var v = G.__fc_selGet(); if (!v) return null;\n"
"  var i = 0, ad = v[i++], ap = [], k;\n"
"  for (k = 0; k < ad; k++) ap.push(v[i++]);\n"
"  var ao = v[i++], fd = v[i++], fp = [];\n"
"  for (k = 0; k < fd; k++) fp.push(v[i++]);\n"
"  var fo = v[i++];\n"
"  var an = nodeAt(ap), fn = nodeAt(fp);\n"
"  if (!an || !fn) return null;\n"
"  return { an: an, ao: offOut(an, ao), fn: fn, fo: offOut(fn, fo), ap: ap, fp: fp };\n"
"}\n"
"function put(an, ao, fn, fo){\n"
"  var a = pathOf(an), f = pathOf(fn);\n"
"  if (!a || !f) return false;\n"
"  return !!G.__fc_selSet(a, offIn(an, ao | 0), f, offIn(fn, fo | 0)); }\n"

#include "js_live_range_shim.inc"

/* ---- Selection ---------------------------------------------------------- */
"function Selection(){}\n"
"var SP = Selection.prototype;\n"
"defp(SP, 'anchorNode',   function(){ var c = cur(); return c ? c.an : null; });\n"
"defp(SP, 'anchorOffset', function(){ var c = cur(); return c ? c.ao : 0; });\n"
"defp(SP, 'focusNode',    function(){ var c = cur(); return c ? c.fn : null; });\n"
"defp(SP, 'focusOffset',  function(){ var c = cur(); return c ? c.fo : 0; });\n"
"defp(SP, 'isCollapsed',  function(){ var c = cur();\n"
"  return c ? (c.an === c.fn && c.ao === c.fo) : true; });\n"
"defp(SP, 'rangeCount',   function(){ return cur() ? 1 : 0; });\n"
"defp(SP, 'type', function(){ var c = cur();\n"
"  if (!c) return 'None';\n"
"  return (c.an === c.fn && c.ao === c.fo) ? 'Caret' : 'Range'; });\n"
"SP.toString = function(){ return G.__fc_selText(); };\n"
"SP.getRangeAt = function(i){\n"
"  var c = cur();\n"
"  if (!c || i !== 0) throw new RangeError('getRangeAt: no range at ' + i);\n"
"  var r = new Range();\n"
/* The Range's start is the EARLIER end in document order, whichever way the
 * selection was made -- that is the whole difference between a Range and the
 * anchor/focus pair, and a backwards drag is how a user makes one. */
"  if (cmpPath(c.ap, c.fp, c.ao, c.fo) <= 0) { r.setStart(c.an, c.ao); r.setEnd(c.fn, c.fo); }\n"
"  else { r.setStart(c.fn, c.fo); r.setEnd(c.an, c.ao); }\n"
"  return r; };\n"
"SP.collapse = function(n, o){ if (!n) { G.__fc_selClear(); return; } put(n, o | 0, n, o | 0); };\n"
"SP.collapseToStart = function(){ var r = this.rangeCount ? this.getRangeAt(0) : null;\n"
"  if (r) this.collapse(r.startContainer, r.startOffset); };\n"
"SP.collapseToEnd = function(){ var r = this.rangeCount ? this.getRangeAt(0) : null;\n"
"  if (r) this.collapse(r.endContainer, r.endOffset); };\n"
"SP.extend = function(n, o){ var c = cur(); if (!c) { this.collapse(n, o); return; }\n"
"  put(c.an, c.ao, n, o | 0); };\n"
"SP.setBaseAndExtent = function(an, ao, fn, fo){ put(an, ao, fn, fo); };\n"
"SP.setPosition = function(n, o){ this.collapse(n, o); };\n"
"SP.removeAllRanges = function(){ G.__fc_selClear(); };\n"
"SP.empty = function(){ G.__fc_selClear(); };\n"
"SP.addRange = function(r){ if (r && r.startContainer)\n"
"    put(r.startContainer, r.startOffset, r.endContainer, r.endOffset); };\n"
"SP.selectAllChildren = function(n){\n"
"  if (!n) return;\n"
"  var last = (n.childNodes || []).length;\n"
"  put(n, 0, n, last); };\n"
"SP.containsNode = function(n){\n"
"  var c = cur(); if (!c || !n) return false;\n"
"  var p = pathOf(n); if (!p) return false;\n"
"  var lo = cmpPath(c.ap, c.fp, c.ao, c.fo) <= 0 ? [c.ap, c.ao] : [c.fp, c.fo];\n"
"  var hi = lo[0] === c.ap ? [c.fp, c.fo] : [c.ap, c.ao];\n"
"  return cmpPath(lo[0], p, lo[1], 0) <= 0 && cmpPath(p, hi[0], 0, hi[1]) <= 0; };\n"
"G.Selection = Selection;\n"
"var theSel = new Selection();\n"
"doc.getSelection = function(){ return theSel; };\n"
"G.getSelection = function(){ return theSel; };\n"

/* ---- the InputEvent the C side raises ----------------------------------- *
 * Constructed here rather than in js_dom.c because that file's event struct
 * has no inputType. Falls back to a plain Event with the fields stamped on it
 * when InputEvent is somehow absent -- a page reading e.inputType then still
 * gets the right answer, which is the field that matters. */
"G.__fcFireInput = function(id, type, itype, data, bubbles, cancelable){\n"
"  var el = null;\n"
"  try { el = doc.querySelector('[data-logit-fcid=\"' + id + '\"]'); } catch (e) {}\n"
"  if (!el || !el.dispatchEvent) return true;\n"
"  var init = { bubbles: !!bubbles, cancelable: !!cancelable,\n"
"               inputType: itype || '', data: (data === null ? null : String(data)) };\n"
"  var ev = null;\n"
"  try { ev = new G.InputEvent(type, init); } catch (e) { ev = null; }\n"
"  if (!ev) { try { ev = new Event(type, init); ev.inputType = init.inputType;\n"
"                   ev.data = init.data; } catch (e2) { return true; } }\n"
"  try { return el.dispatchEvent(ev) !== false; } catch (e3) { return true; }\n"
"};\n"

/* ---- isContentEditable, which a composer reads to decide it is one ------ */
"function protoOf2(t){ try { var e = doc.createElement(t);\n"
"  return e ? Object.getPrototypeOf(e) : null; } catch (x) { return null; } }\n"
"var DP2 = protoOf2('div');\n"
"var HEP = (G.HTMLElement && G.HTMLElement.prototype) || null;\n"
"if (DP2 && DP2 !== HEP && !DP2.__fcCE) {\n"
"  DP2.__fcCE = true;\n"
"  if (!('contentEditable' in DP2))\n"
"    defp(DP2, 'contentEditable',\n"
"      function(){ var v = this.getAttribute('contenteditable');\n"
"        return v === null ? 'inherit' : (v === '' ? 'true' : String(v)); },\n"
"      function(v){ this.setAttribute('contenteditable', String(v)); });\n"
"  if (!('isContentEditable' in DP2))\n"
"    defp(DP2, 'isContentEditable', function(){\n"
"      for (var n = this; n && n.getAttribute; n = n.parentNode) {\n"
"        var v = n.getAttribute('contenteditable');\n"
"        if (v === null) continue;\n"
"        if (v === 'false') return false;\n"
"        return true; }\n"
"      return false; });\n"
"}\n"
"})(globalThis);\n";


/* ================================ the execCommand shim ===================== *
 *
 * Its own string, evaluated after SEL_SHIM, following the same rule: a fault
 * in here cannot take the forms or selection bindings down, because those
 * shipped first.
 *
 * WHAT IS HERE. document.execCommand / queryCommandIndeterm / queryCommandState
 * / queryCommandValue / queryCommandSupported / queryCommandEnabled, and a
 * command table (`EDIT_CMDS`) with two groups:
 *
 *   1. The three flag commands (stylewithcss, usecss, defaultparagraphseparator)
 *      -- two module-level variables, no DOM work at all.
 *   2. Fourteen commands whose queried answer is the spec's CONSTANT
 *      false/false/"" (no indeterm slot, no state slot, no value slot) because
 *      none of them is implemented as a DOM mutation yet -- that absence is
 *      what queryCommandIndeterm/State/Value read as "not on the list", which
 *      IS the specified answer for an unsupported query, not a stand-in for one.
 *
 * bold/italic real-state reading (a third group) was BUILT and REVERTED --
 * see the long comment lower in this file, right before "the entry points",
 * for why: it is demonstrably wrong on the commonest shape in the corpus for
 * a reason that belongs to css_engine.c, not to this file. formatblock /
 * justify* / fontname / fontsize / forecolor / backcolor / hilitecolor /
 * insert{un,}orderedlist / underline / strikethrough / subscript / superscript
 * are the same shape (real value computation, no mutation) and were never
 * attempted -- left for the next pass, not stubbed.
 *
 * NONE of the fourteen group-2 commands gets a real `action`: execCommand
 * keeps returning false for all of them, because none of them mutates the
 * DOM (that is the XL cluster, gated on this one landing first). Returning
 * true without mutating is the exact lie a caller believing "I performed it"
 * would be told -- see c/apps/libc's flock rule.
 *
 * document.execCommand and document.designMode were said, above in SEL_SHIM's
 * own comment, to "grep to nothing across the corpus this browser is aimed
 * at" and not worth building speculatively. That was true of that corpus and
 * is false of WPT's editing/run suite, which is 96,800 of the corpus's
 * subtests behind exactly this one absence -- so the correction belongs next
 * to the claim it corrects, not in place of it. designMode remains absent:
 * nothing in editing/ evidence calls for it. */
static const char EDIT_SHIM[] =
"(function(G){\n"
"var doc = G.document; if (!doc) return;\n"
"function lc(v){ return String(v).toLowerCase(); }\n"

/* ---- cluster 1: the flag commands. Two module-level variables, no DOM. --- */
"var g_cssStyling = false;\n"   /* false on page load, per spec */
"var g_paraSep = 'div';\n"      /* the default single-line container name */

"var EDIT_CMDS = Object.create(null);\n"

/* stylewithcss HAS a state (the flag itself) and NO indeterm slot -- do not
 * add one. usecss and defaultparagraphseparator have NEITHER a state NOR an
 * indeterm slot. That asymmetry is what the corpus's expected arrays encode
 * ([false,false,'',...] vs [false,<flag>,'',...]); collapsing it costs 2,427
 * (case,command) pairs. */
"EDIT_CMDS['stylewithcss'] = {\n"
"  action: function(v){ g_cssStyling = (lc(v) !== 'false'); return true; },\n"
"  state:  function(){ return g_cssStyling; },\n"
"  value:  function(){ return ''; }\n"
"};\n"
/* usecss is the INVERSE of stylewithcss and has no state of its own. */
"EDIT_CMDS['usecss'] = {\n"
"  action: function(v){ g_cssStyling = (lc(v) === 'false'); return true; },\n"
"  value:  function(){ return ''; }\n"
"};\n"
"EDIT_CMDS['defaultparagraphseparator'] = {\n"
"  action: function(v){ var x = lc(v);\n"
"    if (x === 'p' || x === 'div') g_paraSep = x;\n"   /* else: unchanged, still true */
"    return true; },\n"
"  value:  function(){ return g_paraSep; }\n"
"};\n"

/* ---- cluster 3: commands whose queried answer is the spec's constant ---- *
 * false/false/"". Registered (so queryCommandSupported is true and no
 * exception is thrown) with NO indeterm/state/value slot -- the absence IS
 * the answer, not a stand-in for one. `action` returns false: none of these
 * mutates the DOM yet. */
"var NOOP_CMDS = ['createlink', 'delete', 'forwarddelete', 'indent',\n"
"  'inserthorizontalrule', 'inserthtml', 'insertimage', 'insertlinebreak',\n"
"  'insertparagraph', 'inserttext', 'outdent', 'removeformat', 'selectall',\n"
"  'unlink'];\n"
"NOOP_CMDS.forEach(function(name){\n"
"  EDIT_CMDS[name] = { action: function(){ return false; } };\n"
"});\n"

/* ---- cluster 4 (bold/italic real state, read-only) was ATTEMPTED and
 * REVERTED, and the reason is worth keeping here rather than in a commit
 * message nobody reads before touching this file again.
 *
 * A real getAllEffectivelyContainedNodes / getEffectiveCommandValue reader
 * (mirroring editing/include/implementation.js) was built and wired to
 * bold/italic, on the theory that `getComputedStyle(el).fontWeight` /
 * `.fontStyle` already carry the UA default (`b,strong{font-weight:bold}`,
 * `i,em{font-style:italic}`, css_engine.c's UA_CSS) plus inheritance, so no
 * tag-name special-casing would be needed. Two real bugs were found on the
 * way, one fixed and one not:
 *
 *   FIXED (kept, see jf_selset above): document.getSelection().rangeCount
 *   was 0 for EVERY editing/run test case, because forms.c's ce_root() --
 *   what fc_ce_set_paths resolves an incoming Range against -- had exactly
 *   three sources (a live g_ce_f, a live g_ce_a, or the root hint
 *   fc_ce_set_root() stamps down on real focus()), and
 *   editing/include/tests.js builds a Range and calls
 *   `getSelection().addRange(range)` on a div it never focus()es -- which is
 *   legal; selecting text does not require focus() first. `jf_selset` now
 *   reseeds the hint from js_dom_root() on every call, and
 *   doc.getSelection().rangeCount is 1 for the whole corpus as a result.
 *
 *   FOUND, NOT FIXED, OUT OF SCOPE: with the selection fix in, bold's state()
 *   reader answered WRONG more often than the naive "always false" it
 *   replaced (measured: 1232/1494 pass before the selection fix -- via
 *   accident, since state() always took the `!r -> false` exit -- fell to
 *   418/834 after it, once the reader actually ran). The cause is
 *   `getComputedStyle(el).fontWeight` answering the EMPTY STRING for a `<b>`
 *   element `setupDiv()` had just parsed into the test div moments earlier
 *   in the SAME tick -- verified directly: a probe on `<b>foo[]bar</b>` with
 *   a collapsed caret logged `scParent=B ev=` (empty), while the identical
 *   probe on the DIV itself logged `ev=400`. This is the class of bug
 *   css_engine.c's own comments name ("getComputedStyle() answered \"\" for
 *   10,196 css/ subtests... the flush CSSOM requires") -- a computed-style
 *   read racing a cascade that has not run for a just-inserted node -- and
 *   the fix belongs in css_engine.c's css_ensure_styled()/cssd_prop_get(),
 *   which this change does not own.
 *
 * Per "implement a correct SUBSET... leave the rest genuinely absent rather
 * than present and lying": a reader that is demonstrably wrong on the
 * commonest shape in the corpus (a freshly-parsed `<b>` around a collapsed
 * caret) is worse than no reader, so bold/italic are NOT registered.
 * queryCommandIndeterm/State/Value('bold') therefore answer the honest
 * "not on my list" false/false/"" via the same path as every other
 * unimplemented command, until the css_engine.c bug above is fixed by
 * whoever owns that file. */

/* ---- the entry points ---------------------------------------------------- */
"function getActiveRangeOf(){\n"
"  var s = doc.getSelection ? doc.getSelection() : null;\n"
"  return (s && s.rangeCount) ? s.getRangeAt(0) : null; }\n"
"function isEditableNode(n){ var e = (n && n.nodeType === 3) ? n.parentNode : n;\n"
"  return !!(e && e.isContentEditable); }\n"
"var ALWAYS_ENABLED = { copy: 1, defaultparagraphseparator: 1, selectall: 1,\n"
"  stylewithcss: 1, usecss: 1 };\n"
"function isEditableOrHost(n){ return isEditableNode(n); }\n"
"doc.execCommand = function(command, showUi, value){\n"
"  var c = lc(command); var cmd = EDIT_CMDS[c];\n"
"  if (!cmd) return false;\n"
"  if (!doc.queryCommandEnabled(c)) return false;\n"
"  if (typeof cmd.action !== 'function') return false;\n"
"  try { return !!cmd.action(value); } catch (e) { return false; } };\n"
"doc.queryCommandIndeterm = function(command){\n"
"  var cmd = EDIT_CMDS[lc(command)];\n"
"  return (cmd && cmd.indeterm) ? !!cmd.indeterm() : false; };\n"
"doc.queryCommandState = function(command){\n"
"  var cmd = EDIT_CMDS[lc(command)];\n"
"  return (cmd && cmd.state) ? !!cmd.state() : false; };\n"
"doc.queryCommandValue = function(command){\n"
"  var cmd = EDIT_CMDS[lc(command)];\n"
"  return (cmd && cmd.value) ? String(cmd.value()) : ''; };\n"
"doc.queryCommandSupported = function(command){ return lc(command) in EDIT_CMDS; };\n"
"doc.queryCommandEnabled = function(command){\n"
"  var c = lc(command);\n"
"  if (!(c in EDIT_CMDS)) return false;\n"
"  if (ALWAYS_ENABLED[c]) return true;\n"
"  var r = getActiveRangeOf(); if (!r) return false;\n"
"  return isEditableOrHost(r.startContainer) && isEditableOrHost(r.endContainer); };\n"
"})(globalThis);\n";

void js_forms_install(JSContext *ctx)
{
    if (!ctx) return;
    range_native_install(ctx);
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

    /* The Selection/Range surface, in its own eval so a fault in it cannot take
     * the forms bindings -- which shipped first -- down with it. */
    JSValue r2 = JS_Eval(ctx, SEL_SHIM, sizeof SEL_SHIM - 1, "<selection>",
                         JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r2)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        if (m) { int printf(const char *, ...); printf("[forms] selection shim failed: %s\n", m);
                 JS_FreeCString(ctx, m); }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r2);

    /* execCommand and friends, in its own eval for the same reason as above --
     * and AFTER SEL_SHIM, since it calls doc.getSelection(). `g_cssStyling` and
     * `g_paraSep` live inside this eval's closure, so a fresh eval per page
     * (js_page.c makes a new JSContext per navigation) is what resets them --
     * there is no separate reset call to forget. */
    JSValue r3 = JS_Eval(ctx, EDIT_SHIM, sizeof EDIT_SHIM - 1, "<editcmd>",
                         JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r3)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        if (m) { int printf(const char *, ...); printf("[forms] edit shim failed: %s\n", m);
                 JS_FreeCString(ctx, m); }
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r3);

    /* Only NOW is the rich dispatcher installed: it calls __fcFireInput, which
     * the shim above just defined. Installing it earlier would mean an edit
     * during page setup fired nothing, silently. */
    g_ctx = ctx;
    fc_set_dispatch_input(fire_input);

    for (int i = 0; i < FC_CACHE; i++) { g_cache[i].id = 0; g_cache[i].n = 0; }
}

/* The context dies with the page (js_page.c owns that), and a dispatcher
 * pointing at a freed context would fire on the next keystroke into whatever
 * the allocator handed the memory to. browser.c calls this from the same
 * teardown that drops focus and the control state. */
void js_forms_cleanup(void);
void js_forms_cleanup(void)
{
    g_ctx = 0;
    fc_set_dispatch_input(0);
}
