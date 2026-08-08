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
	chk("font-family", "serif", PASS_);
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
