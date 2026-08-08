/*
 * canon.h -- LogitOS addition to LibCSS: the SPECIFIED-value parser and its
 * canonical serialization.
 *
 * WHY THIS EXISTS, AND WHY IT IS NOT THE CASCADE.
 *
 * LibCSS's model is text -> bytecode -> computed style. There is no third
 * form, and in particular there is no way to ask it "what did the author
 * write, spelled canonically?". The CSSOM needs exactly that: `el.style.foo`
 * is defined as a parse followed by a *specified*-value serialization, and it
 * is the only CSS surface a script can read back byte for byte.
 *
 * Until this file, `el.style` in this browser was a VERBATIM TEXT STORE --
 * js_dom.c spliced the author's bytes into the style attribute and handed the
 * same bytes back. That is wrong in two directions at once:
 *
 *   - it accepts garbage. `el.style.width = 'banana'` sticks, and reads back
 *     as 'banana'. The CSSOM says an unparseable declaration is DISCARDED.
 *   - it never canonicalises. `anchor-size(width --foo)` must read back as
 *     `anchor-size(--foo width)`; `color(srgb 10% 10% 10%)` as
 *     `color(srgb 0.1 0.1 0.1)`. Whoever compares strings -- and the whole
 *     "-parse-valid" corpus under css/ does -- sees a failure.
 *
 * So this is a THIRD entry point into the same grammar, deliberately separate
 * from the bytecode path, and the separation is the design rather than a
 * shortcut. Teaching LibCSS's bytecode about anchor() would mean new opcodes,
 * new computed-style fields and a new select dispatch for a value this engine
 * cannot lay out anyway; teaching it to *parse and spell* anchor() is a closed
 * problem with an exact answer that a test can check. The cascade is
 * unchanged by everything in here.
 *
 * THE THREE-WAY ANSWER is the load-bearing part of the contract. A caller
 * needs to distinguish "I know this and here is its canonical spelling" from
 * "this is nobody's" -- but ALSO from "LibCSS itself takes this and I have no
 * specified serializer for it". That third case is most of CSS (`width: 10px`)
 * and it must keep working exactly as it does today, or wiring this in would
 * regress every declaration the browser already honours. Hence CSS_CANON_PASS:
 * the caller keeps the author's bytes, which is what it does now.
 *
 * The safety property that makes CSS_CANON_INVALID usable: it is returned only
 * when NEITHER this parser NOR LibCSS's own can take the declaration. Such a
 * declaration is already dropped by the cascade and renders nothing, so
 * refusing to store it in the CSSOM costs no rendering behaviour -- it only
 * stops `el.style` from remembering something no engine will ever honour.
 */
#ifndef libcss_canon_h_
#define libcss_canon_h_

#ifdef __cplusplus
extern "C" {
#endif

enum css_canon_status {
	/** Neither this parser nor LibCSS can take it: the CSSOM stores
	 *  nothing and getPropertyValue() must answer "". */
	CSS_CANON_INVALID = -1,
	/** Parsed here; `out` holds the canonical specified serialization. */
	CSS_CANON_OK      =  0,
	/** Not ours. LibCSS may or may not take it; either way this file has
	 *  no specified serializer, so the caller keeps the author's bytes.
	 *  Custom properties (--x) always land here: their value is by
	 *  definition whatever token stream the author wrote. */
	CSS_CANON_PASS    =  1
};

/**
 * Parse one declaration as a SPECIFIED value and spell it canonically.
 *
 * \param prop    property name; `plen` < 0 means NUL-terminated
 * \param value   the value text as written, without the trailing `!important`
 * \param out     receives the canonical serialization, NUL-terminated
 * \param outcap  bytes available in `out` (including the NUL)
 * \param outlen  if non-NULL, receives the length written on CSS_CANON_OK
 * \return one of enum css_canon_status
 *
 * `out` is written only on CSS_CANON_OK. A value whose canonical form does not
 * fit `outcap` returns CSS_CANON_PASS rather than truncating: a truncated
 * declaration is worse than an uncanonical one, because it is silently a
 * DIFFERENT value.
 */
int css_canon_decl(const char *prop, int plen,
		const char *value, int vlen,
		char *out, int outcap, int *outlen);

/**
 * Whether this file claims the property at all, ignoring the value.
 *
 * The declaration hook in parse/language.c uses this to stay off properties
 * LibCSS owns: a `width` that LibCSS's own handler refused must still be
 * offered here (it may be `anchor-size(...)`), but there is no reason to run
 * this parser over a property it has never heard of.
 */
int css_canon_knows_property(const char *prop, int plen);

#ifdef __cplusplus
}
#endif

#endif
