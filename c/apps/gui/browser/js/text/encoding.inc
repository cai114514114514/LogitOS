/* The Encoding Standard's two lookups, in C, for js_webapi.c's PRELUDE.
 *
 * Included by js_webapi.c rather than compiled on its own, and the reason is
 * mechanical rather than aesthetic: tests/wpt.mk names the translation units
 * the WPT runner links, that file belongs to another line, and an API nobody's
 * harness compiles is an API nobody measures. An .inc costs one #include and
 * is measured by every harness that already builds js_webapi.c. Promote it to
 * a real TU whenever the build lists are next touched -- nothing here depends
 * on being included rather than compiled.
 *
 * ONLY THE TABLE LOOKUPS ARE HERE. The decoders themselves are JavaScript, in
 * js_encoding_prelude.inc, for the reason the top of js_webapi.c gives about
 * Headers and URL: they are byte-shuffling over strings and typed arrays, and
 * a JS engine is what that is for. What C is holding is the 228-label table
 * and 27 index tables, which are 546 lines of generated data that would cost
 * a hundred kilobytes of source to express as a JS literal in the prelude.
 *
 * The tables come from the WHATWG data files -- see tools/gen_encoding_tables.py,
 * which also writes down, at length, exactly how much of the encoding/ WPT
 * result this makes tautological. Read that before quoting a labels number.
 */

#include "encoding_tables.inc"

/* Get an encoding, per https://encoding.spec.whatwg.org/#concept-encoding-get.
 *
 * The algorithm, NOT the table, is what the corpus breaks implementations on:
 *   - strip leading AND trailing ASCII whitespace, which the Encoding Standard
 *     defines as exactly TAB LF FF CR SPACE. Not \v. `isspace()` includes \v
 *     and would accept "\vutf-8" as a label, which is a subtest.
 *   - ASCII-lowercase the remainder -- ASCII only, so a label is never
 *     case-folded through a locale.
 *   - an unmatched label is a FAILURE, and the caller turns that into a
 *     RangeError. Quietly falling back to UTF-8 is what this file replaces.
 */
static int enc_ws(int c)
{ return c == 0x09 || c == 0x0A || c == 0x0C || c == 0x0D || c == 0x20; }

/* ---- THE NEGATIVE CONTROL -------------------------------------------------
 *
 * -DENC_NEGCTL_IGNORE_LABEL replaces the lookup with "every label is UTF-8".
 *
 * This is not an invented straw man. It is, exactly, the implementation that
 * shipped in js_webapi.c until this file replaced it, and it is the shape a
 * reasonable person writes first: the web is UTF-8, a decoder that decodes
 * UTF-8 works on every page anyone tests it against, and the label argument
 * looks like something you can safely take on trust. It renders every real
 * site correctly. It also makes `new TextDecoder('shift_jis').encoding`
 * answer 'utf-8', makes an invalid label succeed instead of throwing, and
 * turns twenty-seven legacy encodings into mojibake -- none of which any test
 * written from a page load would notice.
 *
 * So `make test-encoding-negctl` builds THIS and requires the encoding suite
 * to FAIL. If it passes, the suite is measuring the corpus's presence and not
 * the decoder, and the 94.1% means nothing.
 *
 * Note what is NOT stubbed: the decoder machinery, the driver, the BOM sniff,
 * encodeInto. Removing the table alone is the sharpest possible cut -- one
 * lookup, everything else identical -- which is what makes a failure
 * attributable. */
static const char *enc_lookup_label(const char *label)
{
    if (!label) return 0;
    const char *s = label;
    const char *e = label + strlen(label);
    while (s < e && enc_ws((unsigned char)*s)) s++;
    while (e > s && enc_ws((unsigned char)e[-1])) e--;
    int n = (int)(e - s);
    if (n <= 0 || n > 64) return 0;

    char buf[65];
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    buf[n] = 0;

    /* ENC_LABELS is sorted by the generator, which asserts uniqueness. */
    int lo = 0, hi = ENC_LABEL_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(buf, ENC_LABELS[mid].label);
        if (c == 0) return ENC_LABELS[mid].name;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}

/* __encLabel(label) -> the canonical lowercased encoding name, or null.
 *
 * THE LENGTH IS NOT OPTIONAL. A JS string may contain U+0000, and
 * `new TextDecoder("utf-8\0")` must be a RangeError -- NUL is not one of the
 * five characters the Encoding Standard strips, so the label is simply not in
 * the table. Read back through plain JS_ToCString it arrives as a
 * NUL-terminated "utf-8", matches, and the decoder is built. That is 228
 * subtests of encoding/api-invalid-label.any.js on its own, and it is the
 * whole of the difference between this and the obvious version.
 *
 * No table label contains a NUL, so a label whose C length disagrees with its
 * JS length cannot match anything and is rejected without a lookup. */
static JSValue js_enc_label(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
#ifdef ENC_NEGCTL_IGNORE_LABEL
    /* See the block comment above enc_lookup_label. One cut: the table is
     * still compiled, the decoders are still here, the driver is unchanged --
     * the label simply stops being consulted, which is what shipped. */
    (void)argc; (void)argv;
    return JS_NewString(ctx, "utf-8");
#else
    if (argc < 1) return JS_NULL;
    size_t len = 0;
    const char *label = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!label) return JS_NULL;
    const char *name = (strlen(label) == len) ? enc_lookup_label(label) : 0;
    JS_FreeCString(ctx, label);
    return name ? JS_NewString(ctx, name) : JS_NULL;
#endif
}

/* __encIndex(name) -> a 128-element Array mapping byte 0x80+i to a code point,
 * 0 meaning "no mapping"; or null when `name` is not a single-byte encoding.
 *
 * A plain Array and not a Uint16Array on purpose: the decoder needs to tell a
 * hole from a real code point on every byte, and `0` in a numeric array is one
 * comparison either way -- while building a typed array here would mean the
 * prelude had to care whether ArrayBuffer construction from C succeeded. Built
 * on demand, once per decoder construction, so the 27 tables cost nothing
 * until a page asks for one.
 */
static JSValue js_enc_index(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;

    const unsigned short *idx = 0;
    for (int i = 0; i < ENC_SINGLE_BYTE_COUNT; i++)
        if (strcmp(name, ENC_SINGLE_BYTE[i].name) == 0) { idx = ENC_SINGLE_BYTE[i].idx; break; }
    JS_FreeCString(ctx, name);
    if (!idx) return JS_NULL;

    JSValue a = JS_NewArray(ctx);
    if (JS_IsException(a)) return JS_NULL;
    for (int i = 0; i < 128; i++)
        JS_SetPropertyUint32(ctx, a, (uint32_t)i, JS_NewInt32(ctx, idx[i]));
    return a;
}
