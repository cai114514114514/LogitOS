/*
 * cssparse_test.c -- the specified-value parser and its canonical
 * serialization (third_party/css/libcss/src/parse/canon.c).
 *
 * WHY A SEPARATE SUITE WHEN test-wpt ALREADY MEASURES THIS.
 *
 * It does not, yet, and that is the point. The corpus reaches this grammar
 * only through `el.style.foo = x`, which in this browser is still a verbatim
 * text store in c/apps/browser/js_dom.c -- so every number test-wpt prints
 * about css/*-parse-valid.html today is a number about a store, not about a
 * parser. This suite asks the parser directly. It is therefore the thing that
 * can go red on a parser bug on the day the bug lands, rather than on the day
 * the CSSOM is wired to it.
 *
 * EVERY EXPECTATION HERE IS TRANSCRIBED FROM WPT, not invented. The comment on
 * each block names the file it came from, so a disagreement can be checked
 * against upstream rather than argued about. Where WPT permits a set of
 * serializations (assert_in_array), only the canonical one is required here --
 * a stricter test than the corpus runs, deliberately: "one of these is
 * acceptable" is the right rule for an engine being measured and the wrong one
 * for an engine being written.
 *
 * THE THREE OUTCOMES ARE ALL TESTED. A parser that only ever says yes is not
 * a parser; test_invalid_value is half of the corpus and the reason
 * CSS_CANON_INVALID exists. And CSS_CANON_PASS is tested because it is the
 * safety property the whole wiring rests on: if `width: 10px` ever stops
 * answering PASS, wiring this into the CSSOM would silently reroute most of
 * CSS through a parser that was never meant to own it.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <libcss/canon.h>

static int g_pass, g_fail;
static const char *g_group = "";

static void group(const char *g) { g_group = g; }

/* expect == NULL  -> CSS_CANON_INVALID
 * expect == PASS_ -> CSS_CANON_PASS
 * otherwise       -> CSS_CANON_OK with exactly these bytes */
static const char PASS_[] = "\x01(pass)";

static void chk(const char *prop, const char *value, const char *expect)
{
	char out[1024];
	int len = -1;
	int rc;

	memset(out, 0, sizeof out);
	rc = css_canon_decl(prop, -1, value, -1, out, (int)sizeof out, &len);

	if (expect == NULL) {
		if (rc == CSS_CANON_INVALID) { g_pass++; return; }
		g_fail++;
		printf("  FAIL [%s] %s: %s\n    expected INVALID, got %s%s%s\n",
			g_group, prop, value,
			rc == CSS_CANON_OK ? "OK \"" : "PASS",
			rc == CSS_CANON_OK ? out : "",
			rc == CSS_CANON_OK ? "\"" : "");
		return;
	}
	if (expect == PASS_) {
		if (rc == CSS_CANON_PASS) { g_pass++; return; }
		g_fail++;
		printf("  FAIL [%s] %s: %s\n    expected PASS, got %s%s%s\n",
			g_group, prop, value,
			rc == CSS_CANON_OK ? "OK \"" : "INVALID",
			rc == CSS_CANON_OK ? out : "",
			rc == CSS_CANON_OK ? "\"" : "");
		return;
	}
	if (rc != CSS_CANON_OK) {
		g_fail++;
		printf("  FAIL [%s] %s: %s\n    expected \"%s\", got %s\n",
			g_group, prop, value, expect,
			rc == CSS_CANON_INVALID ? "INVALID" : "PASS");
		return;
	}
	if (strcmp(out, expect) != 0) {
		g_fail++;
		printf("  FAIL [%s] %s: %s\n    expected \"%s\"\n         got \"%s\"\n",
			g_group, prop, value, expect, out);
		return;
	}
	if (len != (int)strlen(expect)) {
		g_fail++;
		printf("  FAIL [%s] %s: %s\n    outlen %d != %d\n",
			g_group, prop, value, len, (int)strlen(expect));
		return;
	}
	g_pass++;
}

/* Round-trip: the corpus asserts that feeding a serialization back in yields
 * itself. A serializer that is merely self-consistent passes that; a
 * serializer that is canonical passes it AND matches `expect`. Both are
 * checked, because the round-trip is the assertion that catches an emitter
 * whose output its own parser cannot read -- the failure mode a table of
 * hand-written expectations never finds. */
static void chk_rt(const char *prop, const char *value, const char *expect)
{
	char out[1024];
	int rc;

	chk(prop, value, expect);
	if (expect == NULL || expect == PASS_) return;

	memset(out, 0, sizeof out);
	rc = css_canon_decl(prop, -1, expect, -1, out, (int)sizeof out, NULL);
	if (rc != CSS_CANON_OK || strcmp(out, expect) != 0) {
		g_fail++;
		printf("  FAIL [%s] %s: round-trip of \"%s\"\n    got %s\"%s\"\n",
			g_group, prop, expect,
			rc == CSS_CANON_OK ? "" :
			rc == CSS_CANON_INVALID ? "INVALID " : "PASS ", out);
	} else {
		g_pass++;
	}
}

/* ------------------------------------------------------------------ */

/* css/css-anchor-position/anchor-size-parse-valid.html
 *
 * The reordering is the whole test: <anchor-element> may be written on either
 * side of <anchor-size>, and only name-first is the serialization. */
static void t_anchor_size(void)
{
	static const char *const sizes[] = {
		"width", "height", "block", "inline",
		"self-block", "self-inline", NULL
	};
	static const char *const props[] = {
		"width", "min-width", "max-width",
		"height", "min-height", "max-height",
		"block-size", "min-block-size", "max-block-size",
		"inline-size", "min-inline-size", "max-inline-size",
		"left", "right", "top", "bottom", "inset",
		"inset-block", "inset-block-start", "inset-block-end",
		"inset-inline", "inset-inline-start", "inset-inline-end",
		"margin", "margin-left", "margin-right",
		"margin-top", "margin-bottom",
		"margin-block", "margin-block-start", "margin-block-end",
		"margin-inline", "margin-inline-start", "margin-inline-end",
		NULL
	};
	static const char *const fallbacks[] = {
		NULL, "1px", "50%", "calc(50% + 1px)",
		"anchor-size(block)", "anchor-size(--bar block)",
		"anchor-size(--bar block, anchor-size(--baz inline))"
	};
	int pi, si, fi;

	group("anchor-size");

	/* The full cross product the corpus generates, both orders. */
	for (pi = 0; props[pi] != NULL; pi++) {
		for (si = 0; sizes[si] != NULL; si++) {
			for (fi = 0; fi < 7; fi++) {
				char in[256], flip[256], exp[256];
				const char *fb = fallbacks[fi];

				/* name-first: input is already canonical */
				snprintf(exp, sizeof exp, "anchor-size(--foo %s%s%s)",
					sizes[si], fb ? ", " : "", fb ? fb : "");
				snprintf(in, sizeof in, "%s", exp);
				chk_rt(props[pi], in, exp);

				/* size-first: must be reordered on the way out */
				snprintf(flip, sizeof flip, "anchor-size(%s --foo%s%s)",
					sizes[si], fb ? ", " : "", fb ? fb : "");
				chk(props[pi], flip, exp);

				/* no name at all */
				snprintf(exp, sizeof exp, "anchor-size(%s%s%s)",
					sizes[si], fb ? ", " : "", fb ? fb : "");
				chk_rt(props[pi], exp, exp);
			}
		}
	}

	/* Implicit <anchor-size>. */
	chk_rt("width", "anchor-size()", "anchor-size()");
	chk_rt("width", "anchor-size(--foo)", "anchor-size(--foo)");
	chk_rt("width", "anchor-size(--foo, 10px)", "anchor-size(--foo, 10px)");
	chk_rt("width", "anchor-size(10px)", "anchor-size(10px)");

	/* Unitless zero in the typed fallback slot takes the type. */
	chk("width", "anchor-size(--foo width, 0)", "anchor-size(--foo width, 0px)");
	chk("width", "calc(anchor-size(--foo width, 0))",
	    "anchor-size(--foo width, 0px)");

	/* Case folding: keywords and units are ASCII case-insensitive; the
	 * dashed-ident is NOT. */
	chk("width", "ANCHOR-SIZE(WIDTH --Foo)", "anchor-size(--Foo width)");
	chk("width", "anchor-size(--foo width, 1PX)", "anchor-size(--foo width, 1px)");

	/* Whitespace normalization. */
	chk("width", "anchor-size(  --foo   width ,  1px  )",
	    "anchor-size(--foo width, 1px)");

	/* Inside a math function, still reordered. */
	chk_rt("width", "min(100px, 10%, anchor-size(--foo width), anchor-size(--bar height))",
	    "min(100px, 10%, anchor-size(--foo width), anchor-size(--bar height))");
	chk("width", "min(100px,anchor-size(width --foo))",
	    "min(100px, anchor-size(--foo width))");
}

/* css/css-anchor-position/anchor-size-parse-invalid.html and the grammar's
 * own edges. A parser that says yes to these is the bug this file's
 * CSS_CANON_INVALID exists to prevent. */
static void t_anchor_size_invalid(void)
{
	group("anchor-size/invalid");

	chk("width", "anchor-size(--foo bogus)", NULL);
	chk("width", "anchor-size(--foo --bar)", NULL);
	chk("width", "anchor-size(width height)", NULL);
	chk("width", "anchor-size(--foo width extra)", NULL);
	chk("width", "anchor-size(--foo width", NULL);	/* unclosed */
	chk("width", "anchor-size(--foo width))", NULL);
	chk("width", "anchor-size(foo width)", NULL);	/* not dashed */
	chk("width", "anchor-size(--foo width) junk", NULL);

	/* anchor-size() is not allowed in a property that is not a size,
	 * inset or margin. `color` is not one of ours at all, so it PASSes;
	 * the check that matters is a property we DO claim. */
	chk("width", "anchor(--foo left)", NULL);   /* anchor() needs an inset */
	chk("margin-top", "anchor(--foo left)", NULL);
}

/* css/css-anchor-position/anchor-parse-valid.html */
static void t_anchor(void)
{
	static const char *const sides[] = {
		"inside", "outside", "left", "right", "top", "bottom",
		"start", "end", "self-start", "self-end", "center", NULL
	};
	static const char *const props[] = {
		"left", "right", "top", "bottom",
		"inset-block-start", "inset-block-end",
		"inset-inline-start", "inset-inline-end", NULL
	};
	static const char *const fallbacks[] = {
		NULL, "1px", "50%", "calc(50% + 1px)",
		"anchor(left)", "anchor(--bar left)",
		"anchor(--bar left, anchor(--baz right))"
	};
	int pi, si, fi;

	group("anchor");

	for (pi = 0; props[pi] != NULL; pi++) {
		for (si = 0; sides[si] != NULL; si++) {
			for (fi = 0; fi < 7; fi++) {
				char in[256], exp[256];
				const char *fb = fallbacks[fi];

				snprintf(exp, sizeof exp, "anchor(--foo %s%s%s)",
					sides[si], fb ? ", " : "", fb ? fb : "");
				chk_rt(props[pi], exp, exp);

				snprintf(in, sizeof in, "anchor(%s --foo%s%s)",
					sides[si], fb ? ", " : "", fb ? fb : "");
				chk(props[pi], in, exp);

				snprintf(exp, sizeof exp, "anchor(%s%s%s)",
					sides[si], fb ? ", " : "", fb ? fb : "");
				chk_rt(props[pi], exp, exp);
			}
		}
	}

	/* <percentage> side, and a math function resolving to one. */
	chk_rt("top", "anchor(--foo 50%)", "anchor(--foo 50%)");
	chk_rt("top", "anchor(50%)", "anchor(50%)");
	chk_rt("top", "anchor(--foo calc(50%))", "anchor(--foo calc(50%))");
	chk("top", "anchor(50% --foo)", "anchor(--foo 50%)");

	chk("top", "anchor(--foo left, 0)", "anchor(--foo left, 0px)");
	chk("top", "calc(anchor(--foo left, 0))", "anchor(--foo left, 0px)");
	chk_rt("top", "min(100px, 10%, anchor(--foo top), anchor(--bar bottom))",
	    "min(100px, 10%, anchor(--foo top), anchor(--bar bottom))");
}

static void t_anchor_invalid(void)
{
	group("anchor/invalid");

	chk("top", "anchor()", NULL);		/* a side is required */
	chk("top", "anchor(--foo)", NULL);
	chk("top", "anchor(--foo bogus)", NULL);
	chk("top", "anchor(left right)", NULL);
	chk("top", "anchor(--foo --bar left)", NULL);
	chk("top", "anchor(--foo left extra)", NULL);
	chk("top", "anchor(--foo left", NULL);
	chk("width", "anchor(--foo left)", NULL);  /* not an inset property */
}

/* anchor-name / position-anchor */
static void t_anchor_name(void)
{
	group("anchor-name");

	chk_rt("anchor-name", "none", "none");
	chk_rt("anchor-name", "--foo", "--foo");
	chk_rt("anchor-name", "--foo, --bar", "--foo, --bar");
	chk("anchor-name", "--foo,--bar", "--foo, --bar");
	chk("anchor-name", "  --foo ,  --bar  ", "--foo, --bar");
	chk("anchor-name", "NONE", "none");
	chk("anchor-name", "--Foo", "--Foo");	/* case preserved */

	chk("anchor-name", "foo", NULL);
	chk("anchor-name", "auto", NULL);
	chk("anchor-name", "none, --foo", NULL);
	chk("anchor-name", "--foo,", NULL);
	chk("anchor-name", ",--foo", NULL);
	chk("anchor-name", "--foo --bar", NULL);
	chk("anchor-name", "", PASS_);		/* empty is a removal */

	group("position-anchor");
	chk_rt("position-anchor", "auto", "auto");
	chk_rt("position-anchor", "--foo", "--foo");
	chk("position-anchor", "AUTO", "auto");
	chk("position-anchor", "foo", NULL);
	chk("position-anchor", "--foo --bar", NULL);
	chk("position-anchor", "--foo, --bar", NULL);
	chk("position-anchor", "none", NULL);
}

/* css/css-anchor-position/position-area-parsing.html
 *
 * The same cross products the corpus generates, from the same keyword lists.
 * Three rules interact here and each one is a separate way to be wrong:
 * slot order, `span-all` being the droppable default, and the axis-ambiguous
 * groups doing NEITHER of those. */
static const char *const pa_horizontal[] = {
	"left", "right", "span-left", "span-right", "x-start", "x-end",
	"span-x-start", "span-x-end", "self-x-start", "self-x-end",
	"span-self-x-start", "span-self-x-end", NULL
};
static const char *const pa_vertical[] = {
	"top", "bottom", "span-top", "span-bottom", "y-start", "y-end",
	"span-y-start", "span-y-end", "self-y-start", "self-y-end",
	"span-self-y-start", "span-self-y-end", NULL
};
static const char *const pa_inline[] = {
	"inline-start", "inline-end", "span-inline-start",
	"span-inline-end", NULL
};
static const char *const pa_block[] = {
	"block-start", "block-end", "span-block-start",
	"span-block-end", NULL
};
static const char *const pa_self_inline[] = {
	"self-inline-start", "self-inline-end", "span-self-inline-start",
	"span-self-inline-end", NULL
};
static const char *const pa_self_block[] = {
	"self-block-start", "self-block-end", "span-self-block-start",
	"span-self-block-end", NULL
};
static const char *const pa_start_end[] = {
	"start", "end", "span-start", "span-end", NULL
};
static const char *const pa_self_start_end[] = {
	"self-start", "self-end", "span-self-start", "span-self-end", NULL
};

/* Mirrors test_valid_position_area_value_pairs(): `flip` means the pair
 * serializes in the opposite order from the way it is written. */
static void pa_pairs(const char *const *k1, const char *const *k2, int flip)
{
	int i, j;
	for (i = 0; k1[i] != NULL; i++) {
		for (j = 0; k2[j] != NULL; j++) {
			char in[128], exp[128];
			snprintf(in, sizeof in, "%s %s", k1[i], k2[j]);
			if (strcmp(k1[i], k2[j]) == 0)
				snprintf(exp, sizeof exp, "%s", k1[i]);
			else if (flip)
				snprintf(exp, sizeof exp, "%s %s", k2[j], k1[i]);
			else
				snprintf(exp, sizeof exp, "%s %s", k1[i], k2[j]);
			chk_rt("position-area", in, exp);
		}
	}
}

/* Mirrors test_valid_position_area_value_pairs_with_span_all_center(). */
static void pa_universal(const char *const *kws, int flip)
{
	int i;
	for (i = 0; kws[i] != NULL; i++) {
		char in[128], exp[128];

		snprintf(exp, sizeof exp, flip ? "center %s" : "%s center",
			kws[i]);
		snprintf(in, sizeof in, "%s center", kws[i]);
		chk("position-area", in, exp);
		snprintf(in, sizeof in, "center %s", kws[i]);
		chk("position-area", in, exp);

		/* span-all is the default and disappears. */
		snprintf(in, sizeof in, "%s span-all", kws[i]);
		chk("position-area", in, kws[i]);
		snprintf(in, sizeof in, "span-all %s", kws[i]);
		chk("position-area", in, kws[i]);
	}
}

/* Mirrors the START/END variant: axis-ambiguous keywords can neither be
 * reordered nor have span-all dropped, so everything stays as written. */
static void pa_universal_ambiguous(const char *const *kws)
{
	int i;
	for (i = 0; kws[i] != NULL; i++) {
		char in[128];
		snprintf(in, sizeof in, "%s center", kws[i]);
		chk_rt("position-area", in, in);
		snprintf(in, sizeof in, "center %s", kws[i]);
		chk_rt("position-area", in, in);
		snprintf(in, sizeof in, "%s span-all", kws[i]);
		chk_rt("position-area", in, in);
		snprintf(in, sizeof in, "span-all %s", kws[i]);
		chk_rt("position-area", in, in);
	}
}

static void pa_bad_pairs(const char *const *k1, const char *const *k2)
{
	int i, j;
	for (i = 0; k1[i] != NULL; i++) {
		for (j = 0; k2[j] != NULL; j++) {
			char in[128];
			snprintf(in, sizeof in, "%s %s", k1[i], k2[j]);
			chk("position-area", in, NULL);
			snprintf(in, sizeof in, "%s %s", k2[j], k1[i]);
			chk("position-area", in, NULL);
		}
	}
}

static void t_position_area(void)
{
	static const char *const *const singles[] = {
		pa_horizontal, pa_vertical, pa_inline, pa_block,
		pa_self_inline, pa_self_block, pa_start_end,
		pa_self_start_end, NULL
	};
	int g, i;

	group("position-area");

	chk_rt("position-area", "none", "none");
	chk("position-area", "none none", NULL);
	chk("position-area", "start none", NULL);
	chk("position-area", "none start", NULL);
	chk("position-area", "top left top", NULL);

	chk_rt("position-area", "center", "center");
	chk("position-area", "center center", "center");
	chk_rt("position-area", "span-all", "span-all");
	chk("position-area", "span-all span-all", "span-all");
	chk_rt("position-area", "center span-all", "center span-all");
	chk_rt("position-area", "span-all center", "span-all center");

	for (g = 0; singles[g] != NULL; g++)
		for (i = 0; singles[g][i] != NULL; i++)
			chk_rt("position-area", singles[g][i], singles[g][i]);

	/* Valid combinations, both orders. Horizontal, block and self-block
	 * take the first slot. */
	pa_pairs(pa_horizontal, pa_vertical, 0);
	pa_pairs(pa_vertical, pa_horizontal, 1);
	pa_pairs(pa_block, pa_inline, 0);
	pa_pairs(pa_inline, pa_block, 1);
	pa_pairs(pa_self_block, pa_self_inline, 0);
	pa_pairs(pa_self_inline, pa_self_block, 1);
	pa_pairs(pa_start_end, pa_start_end, 0);
	pa_pairs(pa_self_start_end, pa_self_start_end, 0);

	pa_universal(pa_horizontal, 0);
	pa_universal(pa_vertical, 1);
	pa_universal(pa_block, 0);
	pa_universal(pa_inline, 1);
	pa_universal(pa_self_block, 0);
	pa_universal(pa_self_inline, 1);
	pa_universal_ambiguous(pa_start_end);
	pa_universal_ambiguous(pa_self_start_end);

	group("position-area/invalid");

	/* Incompatible axes, both orders. */
	pa_bad_pairs(pa_horizontal, pa_inline);
	pa_bad_pairs(pa_horizontal, pa_block);
	pa_bad_pairs(pa_horizontal, pa_self_inline);
	pa_bad_pairs(pa_horizontal, pa_self_block);
	pa_bad_pairs(pa_horizontal, pa_start_end);
	pa_bad_pairs(pa_horizontal, pa_self_start_end);
	pa_bad_pairs(pa_vertical, pa_inline);
	pa_bad_pairs(pa_vertical, pa_block);
	pa_bad_pairs(pa_vertical, pa_self_inline);
	pa_bad_pairs(pa_vertical, pa_self_block);
	pa_bad_pairs(pa_vertical, pa_start_end);
	pa_bad_pairs(pa_vertical, pa_self_start_end);
	pa_bad_pairs(pa_inline, pa_self_inline);
	pa_bad_pairs(pa_inline, pa_self_block);
	pa_bad_pairs(pa_inline, pa_start_end);
	pa_bad_pairs(pa_inline, pa_self_start_end);
	pa_bad_pairs(pa_block, pa_self_inline);
	pa_bad_pairs(pa_block, pa_self_block);
	pa_bad_pairs(pa_block, pa_start_end);
	pa_bad_pairs(pa_block, pa_self_start_end);
	pa_bad_pairs(pa_start_end, pa_self_start_end);

	/* Same axis twice. */
	for (g = 0; g < 6; g++)
		for (i = 0; singles[g][i] != NULL; i++) {
			char in[128];
			snprintf(in, sizeof in, "%s %s",
				singles[g][i], singles[g][i]);
			chk("position-area", in, NULL);
		}

	chk("position-area", "foobar", NULL);
	chk("position-area", "visible", NULL);
	chk("position-area", "hidden", NULL);
	chk("position-area", "start foobar", NULL);
	chk("position-area", "end visible", NULL);
	chk("position-area", "block-start hidden", NULL);
	chk("position-area", "hidden inline-end", NULL);
	chk("position-area", "foo bar", NULL);
	chk("position-area", "visible hidden", NULL);
	chk("position-area", "hidden visible", NULL);

	/* Case folding, and the value is keywords only. */
	chk("position-area", "TOP LEFT", "left top");
	chk("position-area", "left  top", "left top");
	chk("position-area", "left 10px", NULL);
	chk("position-area", "\"left\"", NULL);
}

/* css/css-fonts/test_font_family_parsing.html
 *
 * Transcribed from that file's `testFontFamilyLists` table, including the
 * rows it marks invalid. The three corners it is really about:
 *
 *   - one family name may be SEVERAL identifiers (`quite simple`), while a
 *     quoted name is exactly one token -- so mixing them in one slot is
 *     invalid, which is what makes `'times' new roman` invalid inside an
 *     otherwise fine list;
 *   - an identifier is not a word. `0simple` is a DIMENSION and `#simple` a
 *     hash, so neither can be a family name;
 *   - `\073 imple` is the ONE identifier `simple` (a hex escape eats one
 *     following space) while `\s imple` is the TWO identifiers `s` and
 *     `imple`. Same-looking input, different values.
 */
static void t_font_family(void)
{
	group("font-family");

	/* basic syntax */
	chk_rt("font-family", "simple", "simple");
	chk("font-family", "'simple'", "\"simple\"");
	chk_rt("font-family", "\"simple\"", "\"simple\"");
	chk_rt("font-family", "-simple", "-simple");
	chk_rt("font-family", "_simple", "_simple");
	chk_rt("font-family", "quite simple", "quite simple");
	chk_rt("font-family", "quite _simple", "quite _simple");
	chk_rt("font-family", "quite -simple", "quite -simple");
	chk("font-family", "0simple", NULL);
	chk("font-family", "simple!", NULL);
	chk("font-family", "simple()", NULL);
	chk("font-family", "quite@simple", NULL);
	chk("font-family", "#simple", NULL);
	chk("font-family", "quite 0simple", NULL);

	/* non-ASCII identifiers */
	chk_rt("font-family", "\xE7\xB4\x8D\xE8\xB1\x86\xE5\xAB\x8C\xE3\x81\x84",
	    "\xE7\xB4\x8D\xE8\xB1\x86\xE5\xAB\x8C\xE3\x81\x84");
	chk_rt("font-family",
	    "\xE7\xB4\x8D\xE8\xB1\x86\xE5\xAB\x8C\xE3\x81\x84, ick, patooey",
	    "\xE7\xB4\x8D\xE8\xB1\x86\xE5\xAB\x8C\xE3\x81\x84, ick, patooey");
	chk_rt("font-family", "\xC4\xB0simple", "\xC4\xB0simple");
	chk_rt("font-family", "\xC3\x9Fsimple", "\xC3\x9Fsimple");

	chk_rt("font-family", "arial, helvetica, sans-serif",
	    "arial, helvetica, sans-serif");
	chk("font-family", "arial,helvetica,sans-serif",
	    "arial, helvetica, sans-serif");

	/* A quoted name is the WHOLE slot. */
	chk("font-family", "arial, helvetica, 'times' new roman, sans-serif", NULL);
	chk("font-family", "arial, helvetica, \"times\" new roman, sans-serif", NULL);
	chk("font-family", "arial, helvetica, times 'new' roman, sans-serif", NULL);
	chk("font-family", "arial, helvetica, times \"new\" roman, sans-serif", NULL);

	/* A quote inside a quoted name survives, re-quoted. */
	chk("font-family", "arial, helvetica, '\\\"times new roman', sans-serif",
	    "arial, helvetica, \"\\\"times new roman\", sans-serif");

	/* escapes */
	chk("font-family", "\\s imple", "s imple");
	chk("font-family", "\\073 imple", "simple");
	chk("font-family", "sim\\035 ple", "sim5ple");
	chk("font-family", "\\1f4a9", "\xF0\x9F\x92\xA9");
	/* A leading zero does not change the value: still six digits or
	 * fewer, still U+1F4A9. */
	chk("font-family", "\\01f4a9", "\xF0\x9F\x92\xA9");
	/* THE SIX-DIGIT BOUNDARY, and it is not a corner case for its own
	 * sake -- WPT lists this row next to the two above as if it were the
	 * same character, and it is not. An escape takes AT MOST six hex
	 * digits, so `\0001f4a9` is U+0001F4 followed by the literal name
	 * characters `a`,`9`: the identifier is three characters long and
	 * begins with a G-acute. A scanner that keeps consuming hex digits
	 * gets a plausible pile of poo instead and nothing else in the suite
	 * would notice. */
	chk("font-family", "\\0001f4a9", "\xC7\xB4" "a9");
	chk("font-family", "\\AbAb", "\xEA\xAE\xAB");

	/* keywords that are ordinary family names */
	chk_rt("font-family", "italic", "italic");
	chk_rt("font-family", "bold", "bold");
	chk_rt("font-family", "bold italic", "bold italic");
	chk_rt("font-family", "italic bold", "italic bold");
	chk_rt("font-family", "larger", "larger");
	chk_rt("font-family", "smaller", "smaller");
	chk_rt("font-family", "bolder", "bolder");
	chk_rt("font-family", "lighter", "lighter");
	chk_rt("font-family", "normal", "normal");
	chk_rt("font-family", "caption", "caption");
	chk_rt("font-family", "icon", "icon");
	chk_rt("font-family", "menu", "menu");

	/* A CSS-wide keyword as the WHOLE value is LibCSS's; the same word in a
	 * list is a parse error; two or more identifiers make an ordinary
	 * family name again. */
	chk("font-family", "initial", PASS_);
	chk("font-family", "inherit", PASS_);
	chk("font-family", "unset", PASS_);
	chk("font-family", "revert", PASS_);
	chk("font-family", "default", NULL);

	chk("font-family", "default, simple", NULL);
	chk("font-family", "initial, simple", NULL);
	chk("font-family", "inherit, simple", NULL);
	chk("font-family", "unset, simple", NULL);
	chk("font-family", "simple, default", NULL);
	chk("font-family", "simple, initial", NULL);
	chk("font-family", "simple, inherit", NULL);
	chk("font-family", "simple, unset", NULL);

	chk_rt("font-family", "normal, simple", "normal, simple");
	chk_rt("font-family", "simple, normal", "simple, normal");
	chk_rt("font-family", "simple, default bongo", "simple, default bongo");
	chk_rt("font-family", "simple, initial bongo", "simple, initial bongo");
	chk_rt("font-family", "simple, inherit bongo", "simple, inherit bongo");
	chk_rt("font-family", "simple, bongo default", "simple, bongo default");
	chk_rt("font-family", "simple, bongo initial", "simple, bongo initial");
	chk_rt("font-family", "simple, bongo inherit", "simple, bongo inherit");
	chk_rt("font-family", "simple, unset bongo", "simple, unset bongo");
	chk_rt("font-family", "simple, bongo unset", "simple, bongo unset");
	chk_rt("font-family", "simple default", "simple default");
	chk_rt("font-family", "simple initial", "simple initial");
	chk_rt("font-family", "simple inherit", "simple inherit");
	chk_rt("font-family", "simple normal", "simple normal");
	chk_rt("font-family", "simple unset", "simple unset");
	chk_rt("font-family", "default simple", "default simple");
	chk_rt("font-family", "initial simple", "initial simple");
	chk_rt("font-family", "inherit simple", "inherit simple");
	chk_rt("font-family", "normal simple", "normal simple");
	chk_rt("font-family", "unset simple", "unset simple");

	group("font-family/invalid");
	chk("font-family", ",", NULL);
	chk("font-family", "simple,", NULL);
	chk("font-family", ",simple", NULL);
	chk("font-family", "simple,,arial", NULL);
	chk("font-family", "16px", NULL);
	chk("font-family", "\"a\" \"b\"", NULL);
	chk("font-family", "simple \"a\"", NULL);
}

/* css/css-color/parsing/color-valid-color-function.html and its -invalid
 * sibling, run over the same colour-space list the corpus iterates.
 *
 * None of these serialization rules is guessable, and each is a separate way
 * for a plausible implementation to be wrong: percentages become numbers,
 * channels are NOT clamped, alpha IS clamped and then vanishes when it is 1,
 * `none` survives, and `xyz` is spelled `xyz-d65` on the way out. */
static void t_color_function(void)
{
	static const char *const spaces[] = {
		"srgb", "srgb-linear", "a98-rgb", "rec2020", "prophoto-rgb",
		"display-p3", "display-p3-linear", "xyz", "xyz-d50",
		"xyz-d65", NULL
	};
	int i;

	group("color()");

	for (i = 0; spaces[i] != NULL; i++) {
		const char *s = spaces[i];
		/* `xyz` is an alias and serializes as `xyz-d65`. */
		const char *r = strcmp(s, "xyz") == 0 ? "xyz-d65" : s;
		char in[256], exp[256];

		/* The channel text goes in as an ARGUMENT, never spliced into
		 * the format string: these values are full of `%` and a
		 * concatenated literal turns each one into a conversion
		 * specifier reading off the end of the varargs. That is not a
		 * hypothetical -- the first version of this file did exactly
		 * that and segfaulted the suite. */
#define CC(sin, sexp) do { \
	snprintf(in, sizeof in, "color(%s %s)", s, sin); \
	snprintf(exp, sizeof exp, "color(%s %s)", r, sexp); \
	chk_rt("color", in, exp); \
} while (0)

		CC("0% 0% 0%",            "0 0 0");
		CC("10% 10% 10%",         "0.1 0.1 0.1");
		CC(".2 .2 25%",           "0.2 0.2 0.25");
		CC("0 0 0 / 1",           "0 0 0");
		CC("0% 0 0 / 0.5",        "0 0 0 / 0.5");
		CC("20% 0 10/0.5",        "0.2 0 10 / 0.5");
		CC("20% 0 10/50%",        "0.2 0 10 / 0.5");
		CC("400% 0 10/50%",       "4 0 10 / 0.5");
		CC("50% -160 160",        "0.5 -160 160");
		CC("50% -200 200",        "0.5 -200 200");
		CC("0 0 0 / -10%",        "0 0 0 / 0");
		CC("0 0 0 / 110%",        "0 0 0");
		CC("0 0 0 / 300%",        "0 0 0");
		CC("200 200 200",         "200 200 200");
		CC("200 200 200 / 200",   "200 200 200");
		CC("-200 -200 -200",      "-200 -200 -200");
		CC("-200 -200 -200 / -200", "-200 -200 -200 / 0");
		CC("200% 200% 200%",      "2 2 2");
		CC("200% 200% 200% / 200%", "2 2 2");
		CC("-200% -200% -200% / -200%", "-2 -2 -2 / 0");
		CC("none none none / none", "none none none / none");
		CC("none none none",      "none none none");
		CC("10% none none / none", "0.1 none none / none");
		CC("none none none / 0.5", "none none none / 0.5");
		CC("0 0 0 / none",        "0 0 0 / none");
#undef CC

		/* Invalid: wrong channel count, a bare fourth component, a
		 * dimension where a number belongs, a missing channel before
		 * the slash. */
#define CBAD(sin) do { \
	snprintf(in, sizeof in, "color(%s%s)", s, sin); \
	chk("color", in, NULL); \
} while (0)
		CBAD("");
		CBAD(" 1");
		CBAD(" 1 1");
		CBAD(" 50%");
		CBAD(" 50% -200");
		CBAD(" 0 0 0 0");
		CBAD(" 0deg 0% 0");
		CBAD(" 0% 0 0 1");
		CBAD(" 0% 0 0 10%");
		CBAD(" 0% 0 0deg");
		CBAD(" 0% 0% 0deg");
		CBAD(" 40% 0 0deg");
		CBAD(" 1 / 0.5");
		CBAD(" 1 1 / .5");
		CBAD(" 50% / 0.5");
		CBAD(" 50% -200 / 0.5");
		CBAD(" / 0.5");
		CBAD(" / 50%");
#undef CBAD

		/* WHERE THIS FILE'S AUTHORITY STOPS, and it is worth being
		 * exact about because the corpus and this API disagree here
		 * for a good reason.
		 *
		 * The corpus says `srgb(0 0 0)` is INVALID -- there is no
		 * `srgb()` function -- and it is right. But CSS_CANON_INVALID
		 * means "NEITHER this parser NOR LibCSS can take it", and from
		 * inside this file the second half of that is unknowable: an
		 * unrecognised colour function might be `lab()`, `oklch()`,
		 * `color-mix()` or `light-dark()`, all of them real values
		 * that a newer LibCSS could accept. Answering INVALID for
		 * every function this file does not personally implement would
		 * make the CSSOM refuse valid CSS the cascade honours, which
		 * is a far worse failure than the one it fixes.
		 *
		 * So the answer is PASS, and the verdict is LibCSS's to give
		 * at the call site. Asserting INVALID here would be asserting
		 * what this author thinks the answer should be rather than
		 * what this API is for -- a test that ratifies the
		 * implementation instead of checking it. */
		snprintf(in, sizeof in, "%s(0 0 0)", s);
		chk("color", in, PASS_);
	}

	group("color()/other");

	/* An unknown colour space is not a colour. */
	chk("color", "color(bogus 0 0 0)", NULL);
	chk("color", "color(--custom 0 0 0)", NULL);

	/* Case folding: the space name and the function are ASCII
	 * case-insensitive, and both serialize lowercase. */
	chk("color", "COLOR(SRGB 0 0 0)", "color(srgb 0 0 0)");
	chk("color", "color(  srgb   0   0   0  )", "color(srgb 0 0 0)");

	/* It applies to every property that takes a <color>, and to none that
	 * does not. */
	chk("background-color", "color(srgb 10% 0 0)", "color(srgb 0.1 0 0)");
	chk("border-top-color", "color(xyz 0 0 0)", "color(xyz-d65 0 0 0)");
	chk("width", "color(srgb 0 0 0)", PASS_);

	group("color()/color-4-5");

	/* The CSS Color 4/5 functions. These are NOT canonically serialized --
	 * see the comment on color_fns[] -- so what is asserted is exactly
	 * what is delivered: the value survives, whitespace-normalized, and
	 * round-trips. Asserting a canonical spelling here would be asserting
	 * something this file does not do.
	 *
	 * They are kept rather than passed on because a caller that falls back
	 * to LibCSS for a PASS drops every one of them: LibCSS predates them
	 * all. A valid value a script sets and cannot read back is a broken
	 * CSSOM, which is a worse failure than an unprettified spelling. */
	chk_rt("color", "color-mix(in srgb, red, blue)",
	    "color-mix(in srgb, red, blue)");
	chk("color", "color-mix(in srgb,red,blue)",
	    "color-mix(in srgb, red, blue)");
	chk_rt("color", "rgb(from red r g b)", "rgb(from red r g b)");
	chk_rt("color", "alpha(from red / 0.5)", "alpha(from red / 0.5)");
	chk_rt("color", "alpha(from currentcolor / calc(alpha * 0.5))",
	    "alpha(from currentcolor / calc(alpha * 0.5))");
	chk_rt("color", "lab(50 20 -30)", "lab(50 20 -30)");
	chk_rt("color", "oklch(0.5 0.2 120)", "oklch(0.5 0.2 120)");
	/* The legacy forms stay LibCSS's; only the RELATIVE form is kept
	 * here. `from` as the first argument is the whole distinction. */
	chk("color", "hsl(120deg 50% 50%)", PASS_);
	chk_rt("color", "hsl(from red h s l)", "hsl(from red h s l)");
	chk_rt("color", "rgba(from red r g b / 0.5)",
	    "rgba(from red r g b / 0.5)");
	chk_rt("color", "light-dark(white, black)", "light-dark(white, black)");
	chk_rt("color", "alpha(from color(display-p3 1 0 0) / 0.5)",
	    "alpha(from color(display-p3 1 0 0) / 0.5)");
	chk_rt("color", "rgb(from alpha(from currentcolor / 0.5) r g b)",
	    "rgb(from alpha(from currentcolor / 0.5) r g b)");

	/* Recognising the NAME is not accepting anything behind it. */
	chk("color", "color-mix()", NULL);
	chk("color", "color-mix(", NULL);
	chk("color", "lab(\"x\")", NULL);
	chk("color", "oklch(1 2 3))", NULL);
	chk("color", "color-mix(in srgb, red, blue) extra", NULL);

	/* THE SAFETY PROPERTY for colours: everything else on these
	 * properties is LibCSS's and must be untouched. */
	chk("color", "red", PASS_);
	chk("color", "#fff", PASS_);
	chk("color", "rgb(1, 2, 3)", PASS_);
	chk("color", "rgba(1,2,3,.5)", PASS_);
	chk("color", "currentColor", PASS_);
	chk("color", "inherit", PASS_);
	chk("background-color", "transparent", PASS_);
}

/* THE SAFETY PROPERTY. Everything LibCSS already owns must answer PASS, or
 * wiring this into el.style would reroute ordinary CSS through a parser that
 * does not implement it. */
static void t_passthrough(void)
{
	group("passthrough");

	chk("width", "10px", PASS_);
	chk("width", "auto", PASS_);
	chk("width", "calc(100% - 10px)", PASS_);
	chk("width", "min(1px, 2px)", PASS_);
	chk("margin", "0 auto", PASS_);
	chk("top", "50%", PASS_);
	chk("top", "var(--x)", PASS_);
	chk("--custom", "anything at all (", PASS_);
	chk("--custom", "anchor-size(width --foo)", PASS_);

	/* Properties this file has never heard of stay someone else's. */
	chk("color", "red", PASS_);
	chk("display", "block", PASS_);
	chk("wibble", "anchor-size(--foo width)", PASS_);

	/* A value with no anchor function in it is LibCSS's even on a
	 * property we claim -- claiming the property is not claiming every
	 * value of it. */
	chk("width", "banana", PASS_);
}

/* Adversarial input. Nothing here may crash, hang or read out of bounds; the
 * answer only has to be one of the three. The corpus is fed to this parser by
 * scripts, and a parser that segfaults costs a whole measurement rather than
 * one test -- which is exactly what mq_parse_media_query did at file 33,379
 * of 34,352 before b7a8d2c. */
static void t_fuzz(void)
{
	static const char *const nasty[] = {
		"", " ", "(", ")", "((((", "))))", ",", ",,,,",
		"anchor-size(", "anchor-size((", "anchor-size(,",
		"anchor-size(--", "anchor-size(-", "anchor-size(\\",
		"anchor(", "anchor(,)", "anchor(--foo,",
		"\"", "'", "\"unterminated", "'unterminated",
		"/*", "/*unterminated", "a/*b*/c",
		"\\", "\\\\", "\\41", "\\41 ", "--\\41",
		"1e", "1e+", ".", "-.", "+.", "1.e5", "0x",
		"anchor-size(--foo width, anchor-size(--a width, anchor-size(--b width,"
		"anchor-size(--c width, anchor-size(--d width))))))",
		"anchor-size(--foo width, calc(calc(calc(calc(calc(1px))))))",
		"\xff\xfe\xfd", "\xc3", "\xe2\x82",
		NULL
	};
	static const char *const props[] = {
		"width", "top", "anchor-name", "position-anchor",
		"margin", "--x", "", NULL
	};
	int i, j;
	char out[1024];

	group("fuzz");
	for (i = 0; nasty[i] != NULL; i++) {
		for (j = 0; props[j] != NULL; j++) {
			int rc = css_canon_decl(props[j], -1, nasty[i], -1,
					out, (int)sizeof out, NULL);
			if (rc != CSS_CANON_OK && rc != CSS_CANON_PASS &&
			    rc != CSS_CANON_INVALID) {
				g_fail++;
				printf("  FAIL [fuzz] %s: %s -> rc=%d\n",
					props[j], nasty[i], rc);
			} else {
				g_pass++;
			}
		}
	}

	/* Deep nesting must be refused, not recursed into. */
	{
		char deep[4096];
		int n = 0, k;
		for (k = 0; k < 200; k++)
			n += snprintf(deep + n, sizeof deep - (size_t)n, "calc(");
		n += snprintf(deep + n, sizeof deep - (size_t)n, "anchor-size(--a width)");
		for (k = 0; k < 200; k++)
			n += snprintf(deep + n, sizeof deep - (size_t)n, ")");
		if (css_canon_decl("width", -1, deep, -1, out, (int)sizeof out,
				NULL) == CSS_CANON_OK) {
			g_fail++;
			printf("  FAIL [fuzz] 200-deep calc accepted\n");
		} else {
			g_pass++;
		}
	}

	/* A buffer too small must not truncate: PASS (keep the author's
	 * bytes) is the only safe answer, because a truncated declaration is
	 * silently a DIFFERENT one. */
	{
		char tiny[8];
		int rc = css_canon_decl("width", -1,
			"anchor-size(width --averylongname)", -1,
			tiny, (int)sizeof tiny, NULL);
		if (rc == CSS_CANON_OK) {
			g_fail++;
			printf("  FAIL [fuzz] truncating serialization accepted\n");
		} else {
			g_pass++;
		}
	}

	/* NULLs are not a crash. */
	{
		int rc = css_canon_decl(NULL, -1, "x", -1, out,
				(int)sizeof out, NULL);
		rc |= css_canon_decl("width", -1, NULL, -1, out,
				(int)sizeof out, NULL);
		rc |= css_canon_decl("width", -1, "x", -1, NULL, 0, NULL);
		g_pass++;
		(void)rc;
	}
}

int main(void)
{
	printf("cssparse: the specified-value parser and canonical serializer\n");

	t_anchor_size();
	t_anchor_size_invalid();
	t_anchor();
	t_anchor_invalid();
	t_anchor_name();
	t_position_area();
	t_font_family();
	t_color_function();
	t_passthrough();
	t_fuzz();

	printf("cssparse: %d passed, %d failed (%d checks)\n",
		g_pass, g_fail, g_pass + g_fail);
	if (g_fail != 0) {
		printf("cssparse: FAIL\n");
		return 1;
	}
	printf("cssparse: ok\n");
	return 0;
}
