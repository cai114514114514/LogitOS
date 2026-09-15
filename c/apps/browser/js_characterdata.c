/* CharacterData's mutating methods that were still absent after
 * js_dom_iface.inc's appendData/substringData landed: insertData, deleteData,
 * replaceData, and Text.splitText.
 *
 * WHY THIS IS ITS OWN FILE AND NOT AN ADDITION TO js_dom_iface.inc. That file
 * is owned by another line and is being extended in parallel (it already
 * carries cd_appendData/cd_substringData -- see its own comment at
 * "CharacterData's own edits"). This does not touch it: every method here is
 * built entirely on `.data`, the accessor js_dom.c's el_get_nodeValue /
 * el_set_nodeValue already publish, plus insertBefore/parentNode/nextSibling/
 * createTextNode -- the same calls a page makes. That has the same consequence
 * js_tokenlist.c's header states for classList: whatever invalidation and
 * observation el_set_nodeValue's chardata_set does for a plain `node.data = x`
 * assignment, insertData/deleteData/replaceData/splitText get for free and
 * cannot disagree with, because there is no second writer. In particular: if
 * MutationObserver's characterData branch lands in chardata_set later, these
 * four methods start emitting correct records with no change here.
 *
 * WHY JS OFFSETS AND NOT BYTE OFFSETS. cd_appendData/cd_substringData in
 * js_dom_iface.inc index `n->text`/`n->textlen` directly, which are UTF-8
 * BYTES -- correct for ASCII, wrong for a surrogate pair or any multi-byte
 * code point, where the DOM's `unsigned long offset` is defined in UTF-16
 * CODE UNITS (DOM sec 4.9). This file never touches n->text: every method
 * reads `this.data` into a JS string first and slices THAT, so offset and
 * count are code units by construction, matching the spec, without having to
 * agree with the sibling methods' byte counting -- they only have to agree on
 * the string that comes back out of `.data`, and they do because both paths
 * end at the same getter.
 *
 * WHAT IS DELIBERATELY OUT OF SCOPE.
 *   - Range boundary adjustment (DOM sec 4.9 steps 8-11 of replace data, and
 *     the equivalent in split text). This engine's Range is a snapshot, not a
 *     live object with boundary points that mutations must fix up -- see its
 *     own header for the six methods it already lists as absent -- so there is
 *     nothing live to adjust. Implementing the adjustment against a Range that
 *     is not live would be motion with no effect, which is worse than silence
 *     because it reads as done.
 *   - ProcessingInstruction. js_dom_iface.inc chains ProcessingInstruction
 *     under CharacterData in the interface hierarchy, but its data lives in an
 *     attribute on a synthetic element (see doc_createProcessingInstruction /
 *     pi_get_data), not in n->text/n->textlen -- `.data` on a PI does not even
 *     go through el_get_nodeValue. cd_appendData and cd_substringData already
 *     exclude it (both check `n->type != N_TEXT && n->type != N_COMMENT`), and
 *     this file keeps the same boundary rather than being the one method
 *     family that treats PI differently from its four siblings.
 */
#include "quickjs.h"
#include "js_characterdata.h"
#include "dom.h"
#include "js_dom.h"
#include <string.h>

int printf(const char *, ...);

/* Correction (2026-09-09), preserving the old rationale above: a whole
 * `.data = prefix + replacement + suffix` write cannot tell live Range where
 * the splice occurred. Mutations now use the native UTF-16 operation. The
 * platform observer consumes DOM_MUT_TEXT, including native keyboard edits,
 * rather than relying on the old setter wrapper. */
static JSValue cd_native_replace(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (argc < 4) return JS_ThrowTypeError(ctx, "replaceData requires node, offset, count and string");
    struct node *n = js_dom_node_from(argv[0]);
    uint32_t offset, count;
    if (!n || (n->type != N_TEXT && n->type != N_COMMENT)) return JS_ThrowTypeError(ctx, "Not CharacterData");
    if (JS_ToUint32(ctx, &offset, argv[1]) || JS_ToUint32(ctx, &count, argv[2])) return JS_EXCEPTION;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[3]);
    if (!s) return JS_EXCEPTION;
    if (offset > dom_text_length(n)) {
        JS_FreeCString(ctx, s);
        return js_dom_throw_dom(ctx, "IndexSizeError", "CharacterData offset exceeds length");
    }
    int ok = len <= 0x7fffffff && dom_text_replace(n, offset, count, s, (int)len);
    JS_FreeCString(ctx, s);
    if (!ok) return JS_ThrowOutOfMemory(ctx);
    js_dom_text_changed(n);
    return JS_UNDEFINED;
}
static JSValue cd_native_split(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv)
{
    (void)self;
    if (argc < 2) return JS_ThrowTypeError(ctx, "splitText requires node and offset");
    struct node *n = js_dom_node_from(argv[0]);
    uint32_t offset;
    if (!n || n->type != N_TEXT) return JS_ThrowTypeError(ctx, "Not a Text node");
    if (JS_ToUint32(ctx, &offset, argv[1])) return JS_EXCEPTION;
    if (offset > dom_text_length(n)) return js_dom_throw_dom(ctx, "IndexSizeError", "Text offset exceeds length");
    struct node *tail = dom_text_split(n, offset);
    if (!tail) return JS_ThrowOutOfMemory(ctx);
    js_dom_text_changed(n);
    return js_dom_node_value(ctx, tail);
}

static const char *CHARACTERDATA_PRELUDE =
"(function () {\n"
"'use strict';\n"
"var G = globalThis;\n"
"if (G.__logit_characterdata) return;\n"
"var doc = G.document;\n"
"if (!doc || typeof doc.createElement !== 'function') return;\n"
/* Both must already be real, chained prototypes -- js_dom_iface.inc installs
 * them from js_dom_init(), which runs before this file is ever reached (see
 * js_characterdata.h). If either is missing this is a build where the DOM
 * interface hierarchy did not link, and there is nowhere sane to hang these:
 * bailing out leaves the methods genuinely absent rather than guessing at a
 * flat fallback prototype, which is how classList's fallback earned its own
 * fifty-line comment in js_tokenlist.c. */
"if (typeof G.CharacterData !== 'function' || !G.CharacterData.prototype) return;\n"
"if (typeof G.Text !== 'function' || !G.Text.prototype) return;\n"
"var CDP = G.CharacterData.prototype;\n"
"var TP = G.Text.prototype;\n"

/* Same idiom as js_tokenlist.c and js_select.c: js_platform.c publishes
 * DOMException after this file installs but long before any of these
 * functions is CALLED, so the lookup is deliberately late. */
"function domThrow(name, msg) {\n"
"  var e = null, DE = G.DOMException;\n"
"  if (typeof DE === 'function') { try { e = new DE(msg, name); } catch (q) { e = null; } }\n"
"  if (!e) { e = new Error(msg); e.name = name; }\n"
"  throw e;\n"
"}\n"
"function def(o, k, v) {\n"
"  try { Object.defineProperty(o, k,\n"
"    { value: v, writable: true, configurable: true, enumerable: false }); } catch (e) {}\n"
"}\n"

/* The receiver check. `data` is null on anything that is not a Text or
 * Comment (el_get_nodeValue's own rule), so a non-CharacterData receiver reads
 * back `null` here and is refused before .slice ever runs on it -- the
 * TypeError names the actual problem instead of "null has no method slice". */
"function cdOf(t) {\n"
"  if (t == null || typeof t.data !== 'string')\n"
"    throw new TypeError('not a CharacterData node');\n"
"  return t;\n"
"}\n"

/* unsigned long coercion, DOM's plain (non-[EnforceRange]) IDL type: ToUint32,
 * which is exactly what >>> 0 does -- NaN/Infinity/negative all fold to a
 * defined value rather than throwing, matching item()'s coercion in
 * js_tokenlist.c. */
"function ulong(v) { return v >>> 0; }\n"

/* insertData(offset, data) -- DOM sec 4.9. Only the length check can fail;
 * there is no upper bound on data's own length, an insert past the end is the
 * one thing that must throw and it is checked against `.data`'s CURRENT
 * length, read once so the check and the write agree even if `data` somehow
 * re-entered .data's getter (it does not, but there is no reason to depend on
 * that). */
"def(CDP, 'insertData', function (offset, data) {\n"
"  var self = cdOf(this);\n"
"  var s = self.data;\n"
"  offset = ulong(offset);\n"
"  if (offset > s.length) domThrow('IndexSizeError',\n"
"    \"insertData: offset (\" + offset + \") is greater than the node's length (\" + s.length + \").\");\n"
"  G.__cdReplace(self, offset, 0, String(data));\n"
"});\n"

/* deleteData(offset, count) -- count is clamped to what remains, not an error:
 * `deleteData(0, 1e9)` on a 3-character node deletes exactly those 3, which is
 * "If offset+count > length, set count to length-offset" verbatim. Only offset
 * itself can be out of range. */
"def(CDP, 'deleteData', function (offset, count) {\n"
"  var self = cdOf(this);\n"
"  var s = self.data;\n"
"  offset = ulong(offset); count = ulong(count);\n"
"  if (offset > s.length) domThrow('IndexSizeError',\n"
"    \"deleteData: offset (\" + offset + \") is greater than the node's length (\" + s.length + \").\");\n"
"  var end = offset + count;\n"
"  if (end > s.length) end = s.length;\n"
"  G.__cdReplace(self, offset, end - offset, '');\n"
"});\n"

/* replaceData(offset, count, data) -- the same clamp on count, and the
 * replacement text can be a different length from what it replaces (that is
 * the entire point of the method), so there is no relation to check between
 * data.length and count. */
"def(CDP, 'replaceData', function (offset, count, data) {\n"
"  var self = cdOf(this);\n"
"  var s = self.data;\n"
"  offset = ulong(offset); count = ulong(count);\n"
"  if (offset > s.length) domThrow('IndexSizeError',\n"
"    \"replaceData: offset (\" + offset + \") is greater than the node's length (\" + s.length + \").\");\n"
"  var end = offset + count;\n"
"  if (end > s.length) end = s.length;\n"
"  G.__cdReplace(self, offset, end - offset, String(data));\n"
"});\n"

/* Text.splitText(offset) -- DOM sec 4.10. Read the tail BEFORE truncating the
 * original: `self.data = ...` below is the one write, and it must happen
 * after newData/newNode are computed or the tail it copies would already be
 * gone. Insertion happens with the ORIGINAL data still on self, matching the
 * spec's order (insert the new node, THEN replace data on the old one) --
 * observably different only to something watching between the two calls to
 * `.data =`, which on a single-threaded engine with no reentry here is nobody.
 *
 * `self.nextSibling` is read once, before the insertBefore call that is about
 * to change what "next sibling" means -- inserting newNode as self's OWN next
 * sibling would otherwise become the reference on the very insert that places
 * it. */
"def(TP, 'splitText', function (offset) {\n"
"  var self = cdOf(this);\n"
"  if (self.nodeType !== 3)\n"
"    throw new TypeError('splitText is only defined on Text nodes');\n"
"  var s = self.data;\n"
"  offset = ulong(offset);\n"
"  if (offset > s.length) domThrow('IndexSizeError',\n"
"    \"splitText: offset (\" + offset + \") is greater than the node's length (\" + s.length + \").\");\n"
"  return G.__cdSplit(self, offset);\n"
"});\n"

/* The sibling methods used byte offsets. Keep the full family on the same
 * UTF-16 contract; no new setter call means no duplicate observer record. */
"def(CDP, 'appendData', function(data){var self=cdOf(this);G.__cdReplace(self,self.data.length,0,String(data));});\n"
"def(CDP, 'substringData', function(offset,count){var s=cdOf(this).data;offset=ulong(offset);count=ulong(count);if(offset>s.length)domThrow('IndexSizeError','Offset exceeds length');return s.slice(offset,offset+count);});\n"
"G.__logit_characterdata = 1;\n"
"})\n";

void js_characterdata_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__cdReplace", JS_NewCFunction(ctx, cd_native_replace, "__cdReplace", 4));
    JS_SetPropertyStr(ctx, g, "__cdSplit", JS_NewCFunction(ctx, cd_native_split, "__cdSplit", 2));
    JS_FreeValue(ctx, g);
    JSValue fn = JS_Eval(ctx, CHARACTERDATA_PRELUDE, strlen(CHARACTERDATA_PRELUDE),
                         "<characterdata>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[characterdata] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        return;
    }
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, 0);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[characterdata] install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
}
