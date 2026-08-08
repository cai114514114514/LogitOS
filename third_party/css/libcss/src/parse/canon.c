/*
 * canon.c -- LogitOS addition to LibCSS.
 *
 * The specified-value parser and canonical serializer described in
 * <libcss/canon.h>. Read that header first: it says why this is a separate
 * entry point into the grammar rather than an extension of the bytecode, and
 * why the three-way answer (OK / PASS / INVALID) is the load-bearing part of
 * the contract.
 *
 * WHY THE TOKENIZER IS LOCAL AND NOT LibCSS's.
 *
 * LibCSS's lexer is right there in src/lex, is well tested, and is the wrong
 * tool here for two reasons that are not stylistic:
 *
 *   1. It is a CSS 2.1 lexer. `--foo` is not an identifier to it -- custom
 *      properties postdate it, which is why c/apps/browser/css_vars.c exists
 *      as a separate pre-pass rather than as a LibCSS feature. Every grammar
 *      in this file is built on <dashed-ident>, so a lexer that cannot spell
 *      one is not a starting point.
 *   2. It emits tokens through a parserutils_inputstream, which means an
 *      allocator, a charset, and a lifetime. This file is called from
 *      `el.style.foo = x` -- once per assignment, on a string that is
 *      typically twenty bytes -- and must not pull that machinery in.
 *
 * The scanner below is CSS Syntax Level 3, restricted to what a declaration
 * value can contain. It is ~200 lines and has no dependency on anything in
 * LibCSS at all, which is also what lets tests/cssparse.mk build it in a
 * second flat rather than linking the whole engine.
 *
 * WHAT `unsafe` MEANS IN THE SCANNER: nothing here trusts its input. This is
 * fed adversarial strings from a test corpus and, in the shipping browser,
 * from any script on any page; every loop below is bounded by the input
 * length and every lookahead is guarded. The one previous parser in this tree
 * that took a NULL peek on trust (mq_parse_media_query, on an empty query)
 * segfaulted a 34,352-file corpus run at file 33,379 and produced no summary
 * line at all.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libcss/canon.h>

/* ====================================================================
 * Scanner
 * ==================================================================== */

enum tk {
	T_EOF = 0,
	T_IDENT,	/* foo, --foo, -webkit-foo */
	T_FUNC,		/* foo(  -- `s`/`len` name the function, paren consumed */
	T_NUM,		/* 1, -2.5, .3 */
	T_PCT,		/* 50% */
	T_DIM,		/* 10px -- `s`/`len` name the unit */
	T_STR,		/* "foo" / 'foo' -- `s`/`len` are the DECODED contents */
	T_HASH,		/* #abc -- `s`/`len` are the characters after the # */
	T_COMMA,
	T_RPAREN,
	T_DELIM		/* any other single character; `delim` holds it */
};

typedef struct {
	int kind;
	const char *s;
	int len;
	double num;
	int isint;	/* the number was written without a `.` or exponent */
	char delim;
	int wsbefore;	/* whitespace (or a comment) preceded this token */
	char *owned;	/* decoded storage for T_STR/escaped T_IDENT, or NULL */
} tok;

typedef struct {
	tok *t;
	int n;
	int i;
	int bad;	/* the input could not be tokenized at all */
} lexed;

static int is_ws(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

static int is_hex(int c)
{
	return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* CSS "name code point": letters, digits, -, _, U+0080 and above. The high
 * range is byte-wise here because the input is UTF-8 and every continuation
 * byte is >= 0x80, so no decode is needed to answer this question. */
static int is_name(int c)
{
	unsigned char u = (unsigned char)c;
	return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
	       is_digit(u) || u == '-' || u == '_' || u >= 0x80;
}

static int is_name_start(int c)
{
	unsigned char u = (unsigned char)c;
	return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
	       u == '_' || u >= 0x80;
}

/* Encode one code point as UTF-8 into `o`; returns bytes written. */
static int utf8_put(char *o, unsigned long cp)
{
	if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
		cp = 0xFFFD;	/* CSS replaces these on escape decode */
	if (cp < 0x80) { o[0] = (char)cp; return 1; }
	if (cp < 0x800) {
		o[0] = (char)(0xC0 | (cp >> 6));
		o[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (char)(0xE0 | (cp >> 12));
		o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		o[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	o[0] = (char)(0xF0 | (cp >> 18));
	o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	o[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

/* Consume a name (identifier body) starting at p, decoding \ escapes.
 * Writes the decoded bytes into `dst` (which the caller sized at >= the
 * remaining input length * 4) and returns the number of INPUT bytes eaten. */
static int scan_name(const char *p, const char *end, char *dst, int *dlen)
{
	const char *start = p;
	int o = 0;

	while (p < end) {
		if (*p == '\\' && p + 1 < end && p[1] != '\n') {
			p++;
			if (is_hex((unsigned char)*p)) {
				unsigned long cp = 0;
				int k = 0;
				while (p < end && k < 6 &&
						is_hex((unsigned char)*p)) {
					char c = *p++;
					cp = cp * 16 + (unsigned long)(
						is_digit((unsigned char)c)
						? c - '0'
						: (c | 32) - 'a' + 10);
					k++;
				}
				if (p < end && is_ws((unsigned char)*p)) p++;
				o += utf8_put(dst + o, cp);
			} else {
				dst[o++] = *p++;
			}
		} else if (is_name((unsigned char)*p)) {
			dst[o++] = *p++;
		} else {
			break;
		}
	}
	*dlen = o;
	return (int)(p - start);
}

static void lex_free(lexed *lx)
{
	int i;
	if (lx->t == NULL) return;
	for (i = 0; i < lx->n; i++)
		free(lx->t[i].owned);
	free(lx->t);
	lx->t = NULL;
	lx->n = 0;
}

/* Tokenize `src`. Returns 0 on success. The token array is over-allocated at
 * one slot per input byte plus one: every token consumes at least one byte,
 * so it cannot overflow, and sizing it up front removes a growth path from a
 * function that runs on untrusted input. */
static int lex(const char *src, int len, lexed *lx)
{
	const char *p = src, *end = src + len;
	int ws = 0;

	memset(lx, 0, sizeof *lx);
	if (len < 0) return -1;
	lx->t = calloc((size_t)len + 2, sizeof *lx->t);
	if (lx->t == NULL) return -1;

	while (p < end) {
		tok *t;

		if (is_ws((unsigned char)*p)) { ws = 1; p++; continue; }
		if (*p == '/' && p + 1 < end && p[1] == '*') {
			const char *q = p + 2;
			while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) q++;
			p = (q + 1 < end) ? q + 2 : end;
			ws = 1;
			continue;
		}

		t = &lx->t[lx->n];
		t->wsbefore = ws;
		ws = 0;

		/* number / dimension / percentage */
		if (is_digit((unsigned char)*p) ||
		    ((*p == '.' || *p == '+' || *p == '-') && p + 1 < end &&
		     (is_digit((unsigned char)p[1]) ||
		      (p[1] == '.' && p + 2 < end &&
		       is_digit((unsigned char)p[2]))))) {
			char *stop = NULL;
			const char *ns = p;
			int isint = 1;
			double v;

			if (*p == '+' || *p == '-') p++;
			while (p < end && is_digit((unsigned char)*p)) p++;
			if (p < end && *p == '.' && p + 1 < end &&
					is_digit((unsigned char)p[1])) {
				isint = 0;
				p++;
				while (p < end && is_digit((unsigned char)*p)) p++;
			}
			if (p < end && (*p == 'e' || *p == 'E')) {
				const char *e = p + 1;
				if (e < end && (*e == '+' || *e == '-')) e++;
				if (e < end && is_digit((unsigned char)*e)) {
					isint = 0;
					p = e;
					while (p < end &&
					       is_digit((unsigned char)*p)) p++;
				}
			}
			v = strtod(ns, &stop);
			t->num = v;
			t->isint = isint;

			if (p < end && *p == '%') {
				t->kind = T_PCT;
				p++;
			} else if (p < end && (is_name_start((unsigned char)*p) ||
					*p == '\\' ||
					(*p == '-' && p + 1 < end &&
					 (is_name_start((unsigned char)p[1]) ||
					  p[1] == '-' || p[1] == '\\')))) {
				char *buf = malloc((size_t)(end - p) * 4 + 4);
				int dl = 0;
				if (buf == NULL) { lex_free(lx); return -1; }
				p += scan_name(p, end, buf, &dl);
				buf[dl] = 0;
				if (dl == 0) {
					/* A `\` with nothing after it is not a
					 * unit. Without this the token is a
					 * dimension with an empty unit and, on
					 * the ident path below, a token that
					 * consumed no input at all. */
					free(buf);
					t->kind = T_NUM;
				} else {
					t->kind = T_DIM;
					t->owned = buf;
					t->s = buf;
					t->len = dl;
				}
			} else {
				t->kind = T_NUM;
			}
			lx->n++;
			continue;
		}

		/* identifier / function */
		if (is_name_start((unsigned char)*p) || *p == '\\' ||
		    (*p == '-' && p + 1 < end &&
		     (is_name_start((unsigned char)p[1]) || p[1] == '-' ||
		      p[1] == '\\'))) {
			char *buf = malloc((size_t)(end - p) * 4 + 4);
			int dl = 0, eaten;
			if (buf == NULL) { lex_free(lx); return -1; }
			eaten = scan_name(p, end, buf, &dl);
			buf[dl] = 0;
			if (eaten == 0) {
				/* THE ZERO-ADVANCE CASE, and it is not
				 * theoretical: a trailing `\` reaches here,
				 * scan_name declines it (there is nothing to
				 * escape), and without this arm the loop emits
				 * an infinite run of empty identifiers and
				 * walks off the token array. The array is
				 * sized on "every token eats at least one
				 * byte", so this is the assumption that keeps
				 * that sizing true. Found by the fuzz table in
				 * tests/unit/cssparse_test.c under ASan. */
				free(buf);
				t->kind = T_DELIM;
				t->delim = *p++;
				lx->n++;
				continue;
			}
			p += eaten;
			t->owned = buf;
			t->s = buf;
			t->len = dl;
			if (p < end && *p == '(') {
				t->kind = T_FUNC;
				p++;
			} else {
				t->kind = T_IDENT;
			}
			lx->n++;
			continue;
		}

		/* string */
		if (*p == '"' || *p == '\'') {
			char q = *p++;
			char *buf = malloc((size_t)(end - p) * 4 + 4);
			int o = 0, closed = 0;
			if (buf == NULL) { lex_free(lx); return -1; }
			while (p < end) {
				if (*p == q) { p++; closed = 1; break; }
				if (*p == '\n') break;	/* bad string */
				if (*p == '\\' && p + 1 < end) {
					p++;
					if (*p == '\n') { p++; continue; }
					if (is_hex((unsigned char)*p)) {
						unsigned long cp = 0;
						int k = 0;
						while (p < end && k < 6 &&
						    is_hex((unsigned char)*p)) {
							char c = *p++;
							cp = cp * 16 +
							  (unsigned long)(
							  is_digit(
							   (unsigned char)c)
							   ? c - '0'
							   : (c | 32) - 'a' + 10);
							k++;
						}
						if (p < end &&
						    is_ws((unsigned char)*p)) p++;
						o += utf8_put(buf + o, cp);
						continue;
					}
					buf[o++] = *p++;
					continue;
				}
				buf[o++] = *p++;
			}
			buf[o] = 0;
			if (!closed) {
				/* An unterminated string is a parse error for
				 * the whole declaration, not a token we can
				 * paper over. */
				free(buf);
				lx->bad = 1;
				lex_free(lx);
				return -1;
			}
			t->kind = T_STR;
			t->owned = buf;
			t->s = buf;
			t->len = o;
			lx->n++;
			continue;
		}

		if (*p == '#') {
			char *buf;
			int dl = 0;
			p++;
			buf = malloc((size_t)(end - p) * 4 + 4);
			if (buf == NULL) { lex_free(lx); return -1; }
			p += scan_name(p, end, buf, &dl);
			buf[dl] = 0;
			t->kind = T_HASH;
			t->owned = buf;
			t->s = buf;
			t->len = dl;
			lx->n++;
			continue;
		}

		if (*p == ',') { t->kind = T_COMMA; p++; lx->n++; continue; }
		if (*p == ')') { t->kind = T_RPAREN; p++; lx->n++; continue; }

		t->kind = T_DELIM;
		t->delim = *p++;
		lx->n++;
	}

	lx->t[lx->n].kind = T_EOF;
	lx->t[lx->n].wsbefore = ws;
	return 0;
}

/* ---- cursor helpers. Every one of these is safe at and past the end. ---- */

static const tok *pk(lexed *lx, int off)
{
	int j = lx->i + off;
	if (j < 0 || j > lx->n) return &lx->t[lx->n];	/* the EOF slot */
	return &lx->t[j];
}

static const tok *cur(lexed *lx) { return pk(lx, 0); }

static void adv(lexed *lx) { if (lx->i < lx->n) lx->i++; }

static int at_end(lexed *lx) { return lx->i >= lx->n; }

/* ====================================================================
 * Output buffer
 * ==================================================================== */

typedef struct {
	char *p;
	int len;
	int cap;
	int ovf;	/* ran out of room -- the caller must not use `p` */
} buf;

static void bput(buf *b, const char *s, int n)
{
	if (b->ovf) return;
	if (n < 0) n = (int)strlen(s);
	if (b->len + n + 1 > b->cap) { b->ovf = 1; return; }
	memcpy(b->p + b->len, s, (size_t)n);
	b->len += n;
	b->p[b->len] = 0;
}

static void bputc(buf *b, char c) { bput(b, &c, 1); }

/* ---- THE NEGATIVE CONTROL ------------------------------------------------
 *
 * -DCANON_NEGCTL does NOT remove a feature. Deleting the anchor grammar would
 * make the suite go red for the least interesting reason available -- values
 * stop being accepted, everything throws, and a broken test harness would
 * "catch" it just as well.
 *
 * What it does instead is what a careful-looking implementation actually gets
 * wrong: it parses every value correctly, rejects exactly the same invalid
 * ones, and then SPELLS the result plausibly rather than canonically. Three
 * changes, each of which reads as a defensible choice in isolation:
 *
 *   - the anchor operands come out in the order the author wrote them, which
 *     is the obvious thing to do and is wrong: the spec fixes name-first
 *   - a comma separator loses its space, `a,b` rather than `a, b`
 *   - the typed zero stays untyped, `0` rather than `0px`
 *
 * Nothing throws, nothing is missing, and every value still round-trips
 * through this parser -- so a suite that only checked "is it accepted" or
 * "does it round-trip" stays green. Only comparing BYTES catches it, which is
 * what WPT does and what this suite therefore has to do.
 * ------------------------------------------------------------------------ */
#ifdef CANON_NEGCTL
#define CANON_COMMA	","
#define CANON_ZERO	"0"
#define CANON_NAME_FIRST 0
#else
#define CANON_COMMA	", "
#define CANON_ZERO	"0px"
#define CANON_NAME_FIRST 1
#endif

static void bcomma(buf *b) { bput(b, CANON_COMMA, -1); }

/* ====================================================================
 * Number and identifier serialization
 * ==================================================================== */

/* CSSOM number serialization: the shortest decimal that round-trips, with no
 * trailing zeros and no exponent for magnitudes a stylesheet can express.
 *
 * The `%.17g`-and-trim shortcut is wrong in a way that shows up immediately in
 * this corpus: 0.1 becomes "0.10000000000000001". Widening the precision until
 * the value round-trips is the standard fix and is what every engine does. */
static void num_str(double v, char *o, int cap)
{
	int p;

	if (v == 0) { snprintf(o, (size_t)cap, "0"); return; }	/* also -0 */
	if (isnan(v)) { snprintf(o, (size_t)cap, "NaN"); return; }
	if (isinf(v)) {
		snprintf(o, (size_t)cap, v > 0 ? "infinity" : "-infinity");
		return;
	}
	if (v == floor(v) && fabs(v) < 1e15) {
		snprintf(o, (size_t)cap, "%lld", (long long)v);
		return;
	}
	for (p = 1; p <= 17; p++) {
		snprintf(o, (size_t)cap, "%.*g", p, v);
		if (strtod(o, NULL) == v) break;
	}
}

static void bnum(buf *b, double v)
{
	char n[64];
	num_str(v, n, (int)sizeof n);
	bput(b, n, -1);
}

/* CSSOM "serialize an identifier". Used by font-family, where the difference
 * between `Foo Bar` and `"Foo Bar"` on the way back out is the whole test. */
static void bident(buf *b, const char *s, int n)
{
	int i;

	if (n == 0) { bput(b, "\\", 1); return; }
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		char e[16];

		if (c == 0) { bput(b, "\xEF\xBF\xBD", 3); continue; }
		if ((c >= 0x01 && c <= 0x1F) || c == 0x7F ||
		    (i == 0 && c >= '0' && c <= '9') ||
		    (i == 1 && c >= '0' && c <= '9' &&
		     (unsigned char)s[0] == '-')) {
			snprintf(e, sizeof e, "\\%x ", (unsigned)c);
			bput(b, e, -1);
			continue;
		}
		if (i == 0 && c == '-' && n == 1) { bput(b, "\\-", 2); continue; }
		if (c >= 0x80 || c == '-' || c == '_' ||
		    (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
		    (c >= 'a' && c <= 'z')) {
			bputc(b, (char)c);
			continue;
		}
		bputc(b, '\\');
		bputc(b, (char)c);
	}
}

/* CSSOM "serialize a string": always double quotes, backslash and quote
 * escaped. */
static void bstring(buf *b, const char *s, int n)
{
	int i;
	bputc(b, '"');
	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		char e[16];
		if (c == 0) { bput(b, "\xEF\xBF\xBD", 3); continue; }
		if (c <= 0x1F || c == 0x7F) {
			snprintf(e, sizeof e, "\\%x ", (unsigned)c);
			bput(b, e, -1);
			continue;
		}
		if (c == '"' || c == '\\') bputc(b, '\\');
		bputc(b, (char)c);
	}
	bputc(b, '"');
}

/* ====================================================================
 * Small comparison helpers
 * ==================================================================== */

static int ieq(const char *a, int alen, const char *b)
{
	int i;
	int blen = (int)strlen(b);
	if (alen != blen) return 0;
	for (i = 0; i < alen; i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
		if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
		if (x != y) return 0;
	}
	return 1;
}

static int tok_is_ident(const tok *t, const char *name)
{
	return t->kind == T_IDENT && ieq(t->s, t->len, name);
}

static int is_dashed_ident(const tok *t)
{
	return t->kind == T_IDENT && t->len >= 2 &&
	       t->s[0] == '-' && t->s[1] == '-';
}

/* A keyword out of a NUL-terminated table; returns the index or -1. The table
 * spelling is the canonical one, so callers emit `tab[idx]` rather than the
 * author's bytes -- which is how `ANCHOR-SIZE(WIDTH --foo)` comes back
 * lowercased without a second pass. */
static int kw_index(const tok *t, const char *const *tab)
{
	int i;
	if (t->kind != T_IDENT) return -1;
	for (i = 0; tab[i] != NULL; i++)
		if (ieq(t->s, t->len, tab[i])) return i;
	return -1;
}

/* ====================================================================
 * Generic component-value serialization
 *
 * Used for everything this file accepts but has no opinion about: the fallback
 * argument of anchor(), the inside of min()/max()/clamp()/calc(). The rule is
 * whitespace normalization plus recursion, so a nested anchor() inside a
 * fallback still gets its arguments reordered.
 * ==================================================================== */

#define CANON_MAXDEPTH 32

static int emit_function(lexed *lx, buf *b, int depth);
static int anchor_fn(lexed *lx, buf *b, int depth, int which);

/* The math functions whose contents this file will normalize rather than
 * refuse. Anything else with a `(` is refused: accepting an unknown function
 * would let `el.style.width = 'wibble(1)'` stick, which is the exact bug this
 * file exists to remove. */
static const char *const math_fns[] = {
	"calc", "min", "max", "clamp", "round", "mod", "rem",
	"sin", "cos", "tan", "asin", "acos", "atan", "atan2",
	"pow", "sqrt", "hypot", "log", "exp", "abs", "sign",
	"sibling-index", "sibling-count",
	"var", "env", "attr", NULL
};

/* Emit ONE token, canonically. Returns 0 on success. */
static int emit_token(lexed *lx, buf *b, int depth)
{
	const tok *t = cur(lx);

	switch (t->kind) {
	case T_NUM:
		bnum(b, t->num);
		adv(lx);
		return 0;
	case T_PCT:
		bnum(b, t->num);
		bputc(b, '%');
		adv(lx);
		return 0;
	case T_DIM: {
		int i;
		bnum(b, t->num);
		/* Units are ASCII case-insensitive and serialize lowercase.
		 * `1PX` and `1px` are the same declaration and must read back
		 * the same way. */
		for (i = 0; i < t->len; i++) {
			char c = t->s[i];
			bputc(b, (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
		}
		adv(lx);
		return 0;
	}
	case T_IDENT:
		bident(b, t->s, t->len);
		adv(lx);
		return 0;
	case T_STR:
		bstring(b, t->s, t->len);
		adv(lx);
		return 0;
	case T_HASH:
		bputc(b, '#');
		bput(b, t->s, t->len);
		adv(lx);
		return 0;
	case T_DELIM:
		bputc(b, t->delim);
		adv(lx);
		return 0;
	case T_FUNC:
		return emit_function(lx, b, depth);
	default:
		return -1;
	}
}

/* Emit `name( ... )`. The cursor is on the T_FUNC token. */
static int emit_function(lexed *lx, buf *b, int depth)
{
	const tok *f = cur(lx);
	int i;

	if (depth >= CANON_MAXDEPTH) return -1;

	if (ieq(f->s, f->len, "anchor")) return anchor_fn(lx, b, depth, 0);
	if (ieq(f->s, f->len, "anchor-size")) return anchor_fn(lx, b, depth, 1);

	for (i = 0; math_fns[i] != NULL; i++)
		if (ieq(f->s, f->len, math_fns[i])) break;
	if (math_fns[i] == NULL) return -1;

	bput(b, math_fns[i], -1);
	bputc(b, '(');
	adv(lx);

	/* Contents, with the source's whitespace normalized to single spaces
	 * and commas to ", ". An empty argument list is fine (`calc()` is not,
	 * but that is the math parser's business, not the serializer's, and
	 * this file does not claim to validate calc arithmetic). */
	{
		int first = 1;
		while (!at_end(lx) && cur(lx)->kind != T_RPAREN) {
			const tok *t = cur(lx);
			if (t->kind == T_COMMA) {
				bcomma(b);
				adv(lx);
				first = 1;
				continue;
			}
			if (!first && t->wsbefore) bputc(b, ' ');
			if (emit_token(lx, b, depth + 1) != 0) return -1;
			first = 0;
		}
	}
	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	bputc(b, ')');
	return 0;
}

/* ====================================================================
 * css-anchor-position
 *
 *   anchor()      = anchor( <anchor-element>? && <anchor-side>,
 *                           <length-percentage>? )
 *   anchor-size() = anchor-size( [ <anchor-element> || <anchor-size> ]?,
 *                                <length-percentage>? )
 *
 * The `&&`/`||` are why this is not a straight-line parse and why the tests
 * are 3,116 lines long: <anchor-element> may appear on EITHER side of the
 * keyword, and both orders are valid input, but only ONE of them is the
 * serialization. Canonical is name-first -- `anchor-size(width --foo)` reads
 * back as `anchor-size(--foo width)`. That single reordering is 596 of the
 * failures in anchor-size-parse-valid.html on its own.
 * ==================================================================== */

static const char *const anchor_sides[] = {
	"inside", "outside", "top", "left", "right", "bottom",
	"start", "end", "self-start", "self-end", "center", NULL
};

static const char *const anchor_sizes[] = {
	"width", "height", "block", "inline", "self-block", "self-inline", NULL
};

/* The fallback argument. `zero_is_px` implements the one place the spec makes
 * a bare 0 grow a unit: a <length-percentage> fallback of `0` serializes as
 * `0px`, because the fallback slot is typed and an untyped zero takes the
 * type. */
/* calc() wrapping a single anchor function collapses: per the calc
 * simplification rules a math function that IS the whole expression is
 * replaced by itself. `calc(anchor-size(--foo width, 0))` is therefore
 * `anchor-size(--foo width, 0px)`. A plain `calc(50%)` does NOT collapse --
 * there is no math function inside to promote -- which is why this checks the
 * INNER token rather than just unwrapping any single-argument calc.
 *
 * Returns 1 if it consumed a collapsed calc(), 0 if it did not (cursor and
 * buffer both unchanged). */
static int try_collapse_calc(lexed *lx, buf *b, int depth)
{
	const tok *t = cur(lx);
	const tok *in;
	int save;
	buf probe;

	if (t->kind != T_FUNC || !ieq(t->s, t->len, "calc")) return 0;
	in = pk(lx, 1);
	if (in->kind != T_FUNC ||
	    (!ieq(in->s, in->len, "anchor") &&
	     !ieq(in->s, in->len, "anchor-size")))
		return 0;

	save = lx->i;
	probe = *b;
	adv(lx);	/* past calc( */
	if (emit_function(lx, &probe, depth + 1) == 0 &&
	    !at_end(lx) && cur(lx)->kind == T_RPAREN) {
		adv(lx);
		if (at_end(lx) || cur(lx)->kind == T_RPAREN ||
		    cur(lx)->kind == T_COMMA) {
			*b = probe;
			return 1;
		}
	}
	lx->i = save;
	return 0;
}

static int emit_fallback(lexed *lx, buf *b, int depth)
{
	const tok *t = cur(lx);

	if (try_collapse_calc(lx, b, depth)) return 0;
	t = cur(lx);

	/* The fallback is ONE <length-percentage>, and typing it is what makes
	 * `anchor-size(foo width)` invalid: with an untyped emitter the
	 * undashed `foo` is not a name and not a keyword, so it falls through
	 * to the fallback slot and a generic token normalizer spells it right
	 * back out. The declaration then sticks, looking perfectly canonical,
	 * for a value no engine can resolve. */
	if (t->kind == T_NUM && t->num == 0) {
		bput(b, CANON_ZERO, -1);  /* an untyped zero takes the type */
		adv(lx);
		return 0;
	}
	switch (t->kind) {
	case T_DIM:
	case T_PCT:
		return emit_token(lx, b, depth + 1);
	case T_FUNC:
		return emit_function(lx, b, depth + 1);
	default:
		return -1;
	}
}

/* `which` 0 = anchor(), 1 = anchor-size(). Cursor is on the T_FUNC token. */
static int anchor_fn(lexed *lx, buf *b, int depth, int which)
{
	const char *const *kws = which ? anchor_sizes : anchor_sides;
	const tok *name = NULL;
	int kw = -1;
	int pct_side = 0;		/* anchor()'s <percentage> side form */
	double pct = 0;
	buf sidebuf;
	char sidestore[256];
	int have_side = 0;

	if (depth >= CANON_MAXDEPTH) return -1;
	adv(lx);	/* past the function token */

	sidebuf.p = sidestore;
	sidebuf.len = 0;
	sidebuf.cap = (int)sizeof sidestore;
	sidebuf.ovf = 0;

	/* Up to two leading components in either order: the dashed-ident and
	 * the keyword (or, for anchor(), a <length-percentage> side). */
	for (;;) {
		const tok *t = cur(lx);
		int k;

		if (t->kind == T_EOF || t->kind == T_RPAREN ||
		    t->kind == T_COMMA) break;

		if (is_dashed_ident(t) && name == NULL) { name = t; adv(lx);
			continue; }

		k = kw_index(t, kws);
		if (k >= 0 && kw < 0) { kw = k; adv(lx); continue; }

		/* anchor()'s side may be a <percentage> or a math function
		 * resolving to one. anchor-size() has no such form. */
		if (which == 0 && !have_side && kw < 0) {
			if (t->kind == T_PCT) {
				pct_side = 1;
				pct = t->num;
				have_side = 1;
				adv(lx);
				continue;
			}
			if (t->kind == T_FUNC) {
				int save = lx->i;
				sidebuf.len = 0;
				sidebuf.ovf = 0;
				if (emit_function(lx, &sidebuf, depth + 1) == 0
						&& !sidebuf.ovf) {
					have_side = 1;
					continue;
				}
				lx->i = save;
			}
		}
		break;
	}

	/* anchor() REQUIRES a side; anchor-size() does not (`anchor-size()`
	 * and `anchor-size(--foo)` are both valid, and so is a bare fallback:
	 * `anchor-size(10px)`). */
	if (which == 0 && kw < 0 && !have_side) {
		/* No side found. The only remaining legal shape is nothing at
		 * all before the comma, which anchor() does not allow. */
		return -1;
	}

	bput(b, which ? "anchor-size(" : "anchor(", -1);
	{
		int wrote = 0;
		int pass;

		/* Two passes over the same two operands, so the ORDER is the
		 * only thing CANON_NAME_FIRST changes. The canonical order is
		 * name then keyword regardless of how the author wrote them;
		 * the negative control reverses it, which is exactly the
		 * plausible-but-wrong spelling a reader would not notice. */
		for (pass = 0; pass < 2; pass++) {
			int want_name = CANON_NAME_FIRST ? (pass == 0)
							 : (pass == 1);
			if (want_name) {
				if (name == NULL) continue;
				if (wrote) bputc(b, ' ');
				bident(b, name->s, name->len);
				wrote = 1;
				continue;
			}
			if (kw >= 0) {
				if (wrote) bputc(b, ' ');
				bput(b, kws[kw], -1);
				wrote = 1;
			} else if (pct_side) {
				if (wrote) bputc(b, ' ');
				bnum(b, pct);
				bputc(b, '%');
				wrote = 1;
			} else if (have_side) {
				if (wrote) bputc(b, ' ');
				bput(b, sidebuf.p, sidebuf.len);
				wrote = 1;
			}
		}

		/* The fallback, if any. */
		if (!at_end(lx) && cur(lx)->kind == T_COMMA) {
			adv(lx);
			if (wrote) bcomma(b);
			if (emit_fallback(lx, b, depth + 1) != 0) return -1;
		} else if (!wrote) {
			/* Nothing before the comma AND no comma either. For
			 * anchor-size() the entire first group is optional, and
			 * CSS lets the comma go with it, so what is left is a
			 * bare fallback: `anchor-size(10px)`. An empty
			 * `anchor-size()` is legal too. anchor() has no such
			 * form -- its <anchor-side> is required -- and any
			 * anchor() that reached here was already refused. */
			if (which != 1) return -1;
			if (!at_end(lx) && cur(lx)->kind != T_RPAREN &&
			    emit_fallback(lx, b, depth + 1) != 0)
				return -1;
		}
	}

	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	bputc(b, ')');
	return 0;
}

/* ====================================================================
 * CSS Color 4/5: a <color> value model, and its SPECIFIED serialization
 *
 * WHAT CHANGED HERE, AND WHY IT HAD TO. The first version of this section
 * spelled exactly one function -- color() -- and handed every other Color 4/5
 * function back with its whitespace normalised and nothing else touched. That
 * was the right first move (the alternative, dropping the declaration, was a
 * measured 2,687-subtest regression) but it is not a serializer, and the
 * corpus says so: 929 subtests of relative colour, 418 of color-mix(), 96 of
 * lab()/lch()/oklab()/oklch() all compare BYTES against a spelling this file
 * was not producing.
 *
 * THE ONE RULE THAT ORGANISES EVERYTHING BELOW: a specified <color> is
 * serialized by RESOLVING it as far as the colour space it was written in
 * allows, and NO FURTHER. Concretely, and every one of these is transcribed
 * from the corpus rather than derived:
 *
 *   - rgb()/rgba(), and hsl()/hwb() whose channels are all real numbers,
 *     resolve to sRGB and come out as the LEGACY form: `rgb(r, g, b)` with
 *     integer channels, or `rgba(r, g, b, a)` when alpha is not 1.
 *     `hsl(120 30% 50%)` is `rgb(89, 166, 89)`.
 *   - hsl()/hwb() with a `none` channel CANNOT resolve -- `none` is a real
 *     value, not a missing one, and the legacy form has no spelling for it --
 *     so they stay in their own space with the percent signs dropped:
 *     `hsl(120 80% none)` is `hsl(120 80 none)`.
 *   - rgb() is the exception and it is not derivable: `none` there resolves to
 *     zero. `rgb(none none none / none)` is `rgba(0, 0, 0, 0)`.
 *   - lab()/lch()/oklab()/oklch() NEVER resolve. They keep their own space and
 *     their percentages become numbers on that space's scale, which differs
 *     per function and per channel: lab's a/b are 125 to the 100%, oklab's are
 *     0.4, lch's C is 150, oklch's is 0.4.
 *   - color() never resolves either, and its percentage channels become
 *     numbers divided by 100.
 *   - a keyword -- `red`, `currentcolor`, `ActiveText` -- stays a keyword,
 *     lowercased. This is why relative colour can say
 *     `rgb(from rebeccapurple r g b)` and get the name back verbatim.
 *   - relative colour and color-mix() are NOT resolved at all: they are
 *     structures whose *arguments* are serialized by these same rules.
 *     `rgb(from rgb(20%, 40%, 60%, 80%) r g b)` is
 *     `rgb(from rgba(51, 102, 153, 0.8) r g b)` -- the origin canonicalised,
 *     the channel expressions left as written.
 *
 * NUMBERS ARE ROUNDED TO SIX SIGNIFICANT DIGITS, and that is a finding rather
 * than a taste. `lch(10 20 1.28rad)` is `lch(10 20 73.3386)`, not
 * `lch(10 20 73.33859777674537)`; `oklch(20% 60% 10)` is `oklch(0.2 0.24 10)`
 * even though 0.6 * 0.4 is 0.24000000000000002 in a double. The shortest
 * round-tripping decimal that num_str() produces -- correct everywhere else in
 * this file -- is WRONG here, and only comparing bytes shows it.
 *
 * WHAT THIS STILL DOES NOT DO, said plainly: it does not evaluate calc(). A
 * channel that is a math function is normalised and kept, so
 * `lab(200 calc(50%) 0.5)` comes out as `lab(100 calc(50%) 0.5)` -- the
 * neighbours resolve, the calc does not. The corpus rows that expect
 * `calc(50 * 3)` to become `calc(150)` fail here exactly as they failed
 * before; nothing regressed and the simplification is honestly not done.
 * A calc anywhere in an hsl()/hwb()/rgb() also blocks the resolve to sRGB,
 * because the value genuinely is not known yet.
 * ==================================================================== */

/* The named colours. Values are deliberately ABSENT: a named colour's
 * specified serialization is its own name, so nothing here ever needs to know
 * what colour `rebeccapurple` is. What this table is for is REFUSAL --
 * `rgb(from banana r g b)` has to be invalid, and without a list of the names
 * that are colours there is no way to say so. */
static const char *const named_colors[] = {
	"aliceblue", "antiquewhite", "aqua", "aquamarine", "azure", "beige",
	"bisque", "black", "blanchedalmond", "blue", "blueviolet", "brown",
	"burlywood", "cadetblue", "chartreuse", "chocolate", "coral",
	"cornflowerblue", "cornsilk", "crimson", "cyan", "darkblue",
	"darkcyan", "darkgoldenrod", "darkgray", "darkgreen", "darkgrey",
	"darkkhaki", "darkmagenta", "darkolivegreen", "darkorange",
	"darkorchid", "darkred", "darksalmon", "darkseagreen",
	"darkslateblue", "darkslategray", "darkslategrey", "darkturquoise",
	"darkviolet", "deeppink", "deepskyblue", "dimgray", "dimgrey",
	"dodgerblue", "firebrick", "floralwhite", "forestgreen", "fuchsia",
	"gainsboro", "ghostwhite", "gold", "goldenrod", "gray", "green",
	"greenyellow", "grey", "honeydew", "hotpink", "indianred", "indigo",
	"ivory", "khaki", "lavender", "lavenderblush", "lawngreen",
	"lemonchiffon", "lightblue", "lightcoral", "lightcyan",
	"lightgoldenrodyellow", "lightgray", "lightgreen", "lightgrey",
	"lightpink", "lightsalmon", "lightseagreen", "lightskyblue",
	"lightslategray", "lightslategrey", "lightsteelblue", "lightyellow",
	"lime", "limegreen", "linen", "magenta", "maroon",
	"mediumaquamarine", "mediumblue", "mediumorchid", "mediumpurple",
	"mediumseagreen", "mediumslateblue", "mediumspringgreen",
	"mediumturquoise", "mediumvioletred", "midnightblue", "mintcream",
	"mistyrose", "moccasin", "navajowhite", "navy", "oldlace", "olive",
	"olivedrab", "orange", "orangered", "orchid", "palegoldenrod",
	"palegreen", "paleturquoise", "palevioletred", "papayawhip",
	"peachpuff", "peru", "pink", "plum", "powderblue", "purple",
	"rebeccapurple", "red", "rosybrown", "royalblue", "saddlebrown",
	"salmon", "sandybrown", "seagreen", "seashell", "sienna", "silver",
	"skyblue", "slateblue", "slategray", "slategrey", "snow",
	"springgreen", "steelblue", "tan", "teal", "thistle", "tomato",
	"turquoise", "violet", "wheat", "white", "whitesmoke", "yellow",
	"yellowgreen", NULL
};

/* The CSS Color 4 system colours. LibCSS predates every one of them, which is
 * why `el.style.color = 'Canvas'` does not stick today -- 19 subtests of
 * color-valid-system-color.html, all of them the same one failure. They
 * serialize LOWERCASED, which is the whole of what the file checks. */
static const char *const system_colors[] = {
	"accentcolor", "accentcolortext", "activetext", "buttonborder",
	"buttonface", "buttontext", "canvas", "canvastext", "field",
	"fieldtext", "graytext", "highlight", "highlighttext", "linktext",
	"mark", "marktext", "selecteditem", "selecteditemtext",
	"visitedtext", NULL
};

/* `currentcolor` and `transparent` are colours too, and neither is in either
 * list above. Kept separate because they are the two a caller is most likely
 * to want to special-case. */
static const char *const special_colors[] = {
	"currentcolor", "transparent", NULL
};

static const char *tab_lookup(const tok *t, const char *const *tab)
{
	int i;
	if (t->kind != T_IDENT) return NULL;
	for (i = 0; tab[i] != NULL; i++)
		if (ieq(t->s, t->len, tab[i])) return tab[i];
	return NULL;
}

/* The canonical lowercase spelling of a colour KEYWORD, or NULL. */
static const char *color_keyword(const tok *t)
{
	const char *k;
	if ((k = tab_lookup(t, special_colors)) != NULL) return k;
	if ((k = tab_lookup(t, named_colors)) != NULL) return k;
	if ((k = tab_lookup(t, system_colors)) != NULL) return k;
	return NULL;
}

/* ---- numbers ------------------------------------------------------------
 *
 * Six significant digits, then the shortest decimal that round-trips through
 * THAT. Rounding first is the point: it is what turns 0.24000000000000002
 * into 0.24 without also turning 0.1 into 0.100000. */
static double sig6(double v)
{
	double e, f, s;

	if (v == 0 || isnan(v) || isinf(v)) return v;
	e = floor(log10(fabs(v)));
	if (e > 14 || e < -14) return v;	/* out of the range CSS writes */
	f = pow(10.0, 5.0 - e);
	s = v * f;
	return (s < 0 ? -floor(-s + 0.5) : floor(s + 0.5)) / f;
}

static void bnum6(buf *b, double v) { bnum(b, sig6(v)); }

static double clampd(double v, double lo, double hi)
{
	if (isnan(v)) return lo;
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static void bint(buf *b, int v)
{
	char t[24];
	snprintf(t, sizeof t, "%d", v);
	bput(b, t, -1);
}

/* An sRGB channel on the 0..255 scale, as the legacy form spells it.
 *
 * Half rounds UP, and the corpus tests exactly that: hwb(320deg 30% 40%) has
 * a blue channel of precisely 127.5 and must come out 128. Truncating is the
 * obvious implementation and is wrong by one on the case that was written to
 * catch it -- which is why CANON_NEGCTL truncates. */
/* The epsilon is not decoration and it is not slop. hwb(120 30% 50%) has a
 * green channel that is exactly 0.5 in arithmetic and 0.49999999999999994 in
 * a double, because `1 - 0.3 - 0.5` cannot be represented; scaled to 255 that
 * is 127.49999999999999 and rounds DOWN, giving 127 where every engine says
 * 128. Real browsers do not hit this because they carry colour channels as
 * FLOAT, whose 24-bit mantissa swallows the error. Nudging by 1e-9 -- four
 * hundred times the largest error a double accumulates over these
 * conversions, and still nowhere near a value a stylesheet could write -- is
 * the same fix without changing the type of everything. */
#define CHAN_EPS 1e-9

static int chan255(double v)
{
	double r;
	if (isnan(v)) return 0;
#if defined(CANON_NEGCTL) || defined(CANON_NEGCTL_ROUND)
	r = floor(v + CHAN_EPS);
#else
	r = floor(v + 0.5 + CHAN_EPS);
#endif
	if (!(r > 0)) return 0;
	if (r > 255) return 255;
	return (int)r;
}

/* Legacy alpha: thousandths, trailing zeros trimmed. NOT a byte -- a hex
 * alpha of 0x80 is 128/255 = 0.50196 and every engine prints `0.502` there,
 * while an authored `rgba(2, 3, 4, 0.5)` prints `0.5`. Both are true at once
 * only if the quantum is a thousandth. */
static void balpha_legacy(buf *b, double a)
{
	int m, at;
	if (isnan(a)) a = 0;
	if (a >= 1) { bputc(b, '1'); return; }
	if (a <= 0) { bputc(b, '0'); return; }
	m = (int)floor(a * 1000.0 + 0.5);
	if (m >= 1000) { bputc(b, '1'); return; }
	if (m <= 0) { bputc(b, '0'); return; }
	at = b->len;
	bputc(b, '0');
	bputc(b, '.');
	bputc(b, (char)('0' + (m / 100) % 10));
	bputc(b, (char)('0' + (m / 10) % 10));
	bputc(b, (char)('0' + m % 10));
	while (b->len > at + 2 && b->p[b->len - 1] == '0') {
		b->len--;
		b->p[b->len] = 0;
	}
}

/* ---- angles ---- */
static int angle_deg(const tok *t, double *out)
{
	if (ieq(t->s, t->len, "deg")) *out = t->num;
	else if (ieq(t->s, t->len, "grad")) *out = t->num * 0.9;
	else if (ieq(t->s, t->len, "rad"))
		*out = t->num * (180.0 / 3.14159265358979323846);
	else if (ieq(t->s, t->len, "turn")) *out = t->num * 360.0;
	else return -1;
	return 0;
}

/* A hue, in degrees, brought into [0, 360).
 *
 * A NON-FINITE hue becomes zero, and that is transcribed rather than derived.
 * The corpus asserts that `hsl(calc(infinity) 100% 50%)`,
 * `hsl(calc(-infinity) 100% 50%)` and `hsl(calc(0 / 0) 100% 50%)` are ALL
 * `rgb(255, 0, 0)`, with a comment saying infinity goes to the upper bound and
 * -infinity and NaN to the lower one. A hue has no upper bound, but 360 and 0
 * are the same hue, so all three land on red and zero is the one answer that
 * produces it for all three. Letting the infinity through instead makes
 * fmod() return NaN and paints the colour black. */
static double norm_hue(double h)
{
	if (isnan(h) || isinf(h)) return 0.0;
	h = fmod(h, 360.0);
	if (h < 0) h += 360.0;
	return h;
}

/* ---- colour-space conversion ----
 *
 * Only the two that a specified value ever has to perform: hsl -> sRGB and
 * hwb -> sRGB. Everything else in Color 4 keeps its own space in a specified
 * serialization, so the matrices those conversions would need are not
 * reachable from here and are deliberately absent. */
static double hsl_f(double n, double h, double s, double l)
{
	double k = fmod(n + h / 30.0, 12.0);
	double a = s * (l < 1.0 - l ? l : 1.0 - l);
	double q = (k - 3.0 < 9.0 - k) ? k - 3.0 : 9.0 - k;
	if (q > 1.0) q = 1.0;
	if (q < -1.0) q = -1.0;
	return l - a * q;
}

static void hsl_to_rgb(double h, double s, double l, double *o)
{
	h = norm_hue(h);
	o[0] = hsl_f(0.0, h, s, l);
	o[1] = hsl_f(8.0, h, s, l);
	o[2] = hsl_f(4.0, h, s, l);
}

static void hwb_to_rgb(double h, double w, double bk, double *o)
{
	int i;
	if (w + bk >= 1.0) {
		double g = (w + bk) > 0 ? w / (w + bk) : 0.0;
		o[0] = o[1] = o[2] = g;
		return;
	}
	hsl_to_rgb(h, 1.0, 0.5, o);
	for (i = 0; i < 3; i++) o[i] = o[i] * (1.0 - w - bk) + w;
}

/* ====================================================================
 * One parsed absolute colour
 * ==================================================================== */

#define CTEXT 200

enum {
	K_NUM = 0, K_PCT, K_ANG, K_NONE, K_IDENT, K_TEXT
};

typedef struct {
	int kind;
	double num;		/* K_ANG: already in degrees */
	char text[CTEXT];	/* K_IDENT (lowercased) / K_TEXT (serialized) */
} comp;

/* ====================================================================
 * calc(): the calculation tree, its unit algebra, and its serialization
 *
 * WHY THIS IS ONE PIECE OF WORK AND NOT TWO. Up to here a math function in a
 * colour channel was kept VERBATIM, whitespace-normalised and otherwise
 * untouched. That loses on both sides of the same coin:
 *
 *   - the serialization is wrong. `calc(50 * 3)` reads back as `calc(150)`,
 *     `calc(g * 2)` as `calc(2 * g)`, `calc(c / 2)` as `calc(0.5 * c)`, and a
 *     product inside a sum grows parentheses it was not written with.
 *   - the REFUSALS are missing, and for exactly the same reason.
 *     `rgb(sign(0% - 0px), 0, 0)` is invalid because a percentage and a
 *     length do not combine, and `color(srgb calc(1px * sibling-index()) 0 0)`
 *     because a colour channel is not a length. A serializer that keeps the
 *     math as written cannot see either: it never computes a type.
 *
 * So the fix for the spelling and the fix for the validity are the same fix,
 * and it is a tree.
 *
 * THE SHAPE. CSS Values 4 says a simplified calculation is a SUM OF PRODUCTS,
 * and each product is one numeric coefficient times some number of opaque
 * factors -- a channel keyword, a `sign()`, a `var()`. That is exactly the
 * representation below (`cterm` and `csum`), and choosing it is what makes the
 * serialization fall out rather than needing rules:
 *
 *   - a term with no factors is a plain number: `150`, `150%`, `40deg`.
 *   - a term with factors prints its coefficient FIRST -- `2 * g`, never
 *     `g * 2` -- because the coefficient is a field and the factors are a
 *     list, so there is no other order available.
 *   - a coefficient of exactly 1 on a unit-less term disappears, which is why
 *     `calc(r)` stays `calc(r)` and does not become `calc(1 * r)`.
 *   - division by a number is multiplication by its reciprocal, because that
 *     is the only thing a coefficient can hold. `c / 2` is `0.5 * c` for the
 *     same reason.
 *   - inside a sum of more than one term, a term with more than one factor is
 *     parenthesised: `calc((0.5 * g) + (0.5 * g))`.
 *   - terms fold into each other only when they have no factors and the same
 *     unit. `0.5 - 1` is `-0.5`; `1em - 10px` is still `1em - 10px`, because
 *     nothing here knows how many pixels an em is; and `g * .5 + g * .5` stays
 *     two terms, because folding them would change `(0.5 * g) + (0.5 * g)`
 *     into `1 * g` and the corpus says it must not.
 *
 * THE UNIT ALGEBRA is small and strict. A product of two dimensions is refused
 * outright (`0.56turn * -0.43turn` has no type), a sum requires every term to
 * be the SAME category, and the finished expression must have a category the
 * CHANNEL accepts -- number or percentage for an rgb channel, number or angle
 * for a hue, and never a length, which is what refuses
 * `color(srgb calc(1px * sibling-index()) 0 0)`.
 *
 * The tempting loosening is to let a number and a percentage add, on the
 * grounds that in a colour channel a percentage IS a number once resolved.
 * The corpus refuses it, and refuses it through relative colour: a channel
 * keyword is a <number>, and `rgb(from rebeccapurple calc(r + 1%) g b)` is
 * invalid. No table accepts that sum and still rejects this one.
 *
 * A PERCENTAGE THAT HAS NOTHING TO RESOLVE AGAINST is refused wherever it
 * appears, not just at the top: `hsl(calc(sign(50%) * 1deg) 82% 43%)` is
 * invalid even though `sign()` returns a number and number times angle is an
 * angle. A hue is <number> | <angle> and never a percentage, so the 50% has
 * no meaning to compute with. `pct_ok` therefore travels all the way down the
 * expression rather than being checked on the result.
 *
 * WHAT IS STILL OPAQUE. `min()`, `clamp()`, `sign()`, `var()` and the rest are
 * factors, not nodes: their arguments are PARSED (so the units inside them are
 * checked -- that is what catches `sign(0% - 0px)`) and then re-emitted from
 * the source tokens rather than from a tree, because their own serialization
 * rules are not this one and guessing them would be a new class of wrong.
 * var()/env()/attr() are not even parsed: their contents are an arbitrary
 * token stream by definition.
 * ==================================================================== */

#define CALC_MAXTERM 16
#define CALC_MAXFACT 4
#define CALC_MAXDEPTH 12

/* Unit CATEGORIES. Two dimensions add only if they are the same category;
 * they never multiply. The list is what a colour value can plausibly contain
 * plus the categories a wrong one lands in, because the point of naming
 * lengths and times separately is to be able to REFUSE them. */
enum {
	U_NUM = 0, U_PCT, U_LEN, U_ANG, U_TIME, U_FREQ, U_RES, U_FLEX,
	U_UNKNOWN
};

static const char *const u_len[] = {
	"px", "em", "rem", "ex", "rex", "ch", "rch", "ic", "ric", "lh", "rlh",
	"cap", "rcap", "vw", "vh", "vi", "vb", "vmin", "vmax",
	"svw", "svh", "svi", "svb", "svmin", "svmax",
	"lvw", "lvh", "lvi", "lvb", "lvmin", "lvmax",
	"dvw", "dvh", "dvi", "dvb", "dvmin", "dvmax",
	"cm", "mm", "q", "in", "pt", "pc", NULL
};
static const char *const u_ang[] = { "deg", "grad", "rad", "turn", NULL };
static const char *const u_time[] = { "s", "ms", NULL };
static const char *const u_freq[] = { "hz", "khz", NULL };
static const char *const u_res[] = { "dpi", "dpcm", "dppx", "x", NULL };

static int str_in(const char *s, const char *const *tab)
{
	int i;
	for (i = 0; tab[i] != NULL; i++)
		if (ieq(s, (int)strlen(s), tab[i])) return 1;
	return 0;
}

static int unit_cat(const char *u)
{
	if (u[0] == 0) return U_NUM;
	if (u[0] == '%' && u[1] == 0) return U_PCT;
	if (str_in(u, u_len)) return U_LEN;
	if (str_in(u, u_ang)) return U_ANG;
	if (str_in(u, u_time)) return U_TIME;
	if (str_in(u, u_freq)) return U_FREQ;
	if (str_in(u, u_res)) return U_RES;
	if (ieq(u, (int)strlen(u), "fr")) return U_FLEX;
	return U_UNKNOWN;
}

/* Which categories may share a sum: only their own.
 *
 * The tempting loosening is to let a NUMBER and a PERCENTAGE add, on the
 * grounds that in a colour channel a percentage IS a number once resolved.
 * The corpus says no, and says it through relative colour: a channel keyword
 * is a <number>, and `rgb(from rebeccapurple calc(r + 1%) g b)` is invalid.
 * There is no version of this table that accepts that and still refuses it. */
static int cat_combine(int a, int b, int lp)
{
	if (a == b) return a;
	/* In a <length-percentage> context -- a grid track size, a width -- a
	 * percentage IS a length and the two add: `calc(30% + 40vw)` is a
	 * valid track size. In a colour channel it is not, and that is the
	 * whole reason this takes a flag instead of being a table. */
	if (lp && (a == U_LEN || a == U_PCT) && (b == U_LEN || b == U_PCT))
		return U_LEN;
	return -1;
}

typedef struct {
	double coef;
	char unit[24];
	int nfact;
	char fact[CALC_MAXFACT][CTEXT];
	int ndiv;			/* factors under the division bar */
	char div[CALC_MAXFACT][CTEXT];
} cterm;

typedef struct {
	int n;
	cterm t[CALC_MAXTERM];
} csum;

static void term_num(cterm *t, double v, const char *unit)
{
	memset(t, 0, sizeof *t);
	t->coef = v;
	if (unit != NULL) {
		size_t n = strlen(unit);
		if (n >= sizeof t->unit) n = sizeof t->unit - 1;
		memcpy(t->unit, unit, n);
		t->unit[n] = 0;
	}
}

static int term_mul(cterm *dst, const cterm *a, const cterm *b)
{
	int i;

	/* A dimension times a dimension has no type CSS can express, and
	 * `hsl(calc(0.56turn * -0.43turn), ...)` is in the corpus to say so. */
	if (a->unit[0] && b->unit[0]) return -1;
	if (a->nfact + b->nfact > CALC_MAXFACT) return -1;

	if (a->ndiv + b->ndiv > CALC_MAXFACT) return -1;

	memset(dst, 0, sizeof *dst);
	dst->coef = a->coef * b->coef;
	memcpy(dst->unit, a->unit[0] ? a->unit : b->unit, sizeof dst->unit);
	for (i = 0; i < a->nfact; i++)
		memcpy(dst->fact[dst->nfact++], a->fact[i], CTEXT);
	for (i = 0; i < b->nfact; i++)
		memcpy(dst->fact[dst->nfact++], b->fact[i], CTEXT);
	for (i = 0; i < a->ndiv; i++)
		memcpy(dst->div[dst->ndiv++], a->div[i], CTEXT);
	for (i = 0; i < b->ndiv; i++)
		memcpy(dst->div[dst->ndiv++], b->div[i], CTEXT);
	return 0;
}

static int sum_one(csum *s, const cterm *t)
{
	if (s->n >= CALC_MAXTERM) return -1;
	s->t[s->n++] = *t;
	return 0;
}

/* Append `src` (optionally negated) to `dst`, folding a term into an existing
 * one when both are plain numbers of the same unit. */
static int sum_add(csum *dst, const csum *src, int neg)
{
	int i, j;

	for (i = 0; i < src->n; i++) {
		cterm t = src->t[i];
		if (neg) t.coef = -t.coef;
		if (t.nfact == 0 && t.ndiv == 0) {
			for (j = 0; j < dst->n; j++)
				if (dst->t[j].nfact == 0 &&
				    dst->t[j].ndiv == 0 &&
				    strcmp(dst->t[j].unit, t.unit) == 0)
					break;
			if (j < dst->n) { dst->t[j].coef += t.coef; continue; }
		}
		if (sum_one(dst, &t) != 0) return -1;
	}
	return 0;
}

/* A product of two sums is only linear when one of them is a single term --
 * which is all CSS allows anyway. */
static int sum_mul(csum *dst, const csum *a, const csum *b)
{
	const csum *many, *one;
	int i;

	if (a->n == 1) { one = a; many = b; }
	else if (b->n == 1) { one = b; many = a; }
	else return -1;

	memset(dst, 0, sizeof *dst);
	for (i = 0; i < many->n; i++) {
		cterm t;
		if (term_mul(&t, &many->t[i], &one->t[0]) != 0) return -1;
		if (sum_one(dst, &t) != 0) return -1;
	}
	return 0;
}

/* Division. Dividing by a plain NUMBER folds into the coefficient, which is
 * why `c / 2` is `0.5 * c`. Dividing by anything else -- `calc(1 / l)`, where
 * `l` is a channel whose value nobody knows yet -- cannot fold, so the
 * divisor goes under the bar and stays there. Rejecting that case outright
 * was the first version of this function, and it deleted a valid value. */
static int sum_div(csum *dst, const csum *a, const csum *b)
{
	const cterm *d;
	int i, j;

	if (b->n != 1) return -1;		/* non-linear */
	d = &b->t[0];
	if (d->unit[0]) return -1;		/* dividing by a dimension */

	*dst = *a;
	for (i = 0; i < dst->n; i++) {
		cterm *t = &dst->t[i];
		t->coef /= d->coef;
		if (t->ndiv + d->nfact > CALC_MAXFACT) return -1;
		if (t->nfact + d->ndiv > CALC_MAXFACT) return -1;
		for (j = 0; j < d->nfact; j++)
			memcpy(t->div[t->ndiv++], d->fact[j], CTEXT);
		for (j = 0; j < d->ndiv; j++)
			memcpy(t->fact[t->nfact++], d->div[j], CTEXT);
	}
	return 0;
}

static int sum_cat(const csum *s, int lp)
{
	int c = U_NUM, i;
	for (i = 0; i < s->n; i++) {
		int k = unit_cat(s->t[i].unit);
		c = (i == 0) ? k : cat_combine(c, k, lp);
		if (c < 0) return -1;
	}
	return c;
}

/* ---- serialization ---- */

static void bunit(buf *b, const char *u)
{
	int i;
	for (i = 0; u[i]; i++) {
		char c = u[i];
		bputc(b, (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
	}
}

/* Does this term print as more than one factor? That is exactly the condition
 * for parenthesising it inside a sum. */
static int term_compound(const cterm *t)
{
	int printed = t->nfact + t->ndiv;
	if (printed == 0) return 0;
	if (t->ndiv > 0) return 1;	/* the coefficient always prints */
	if (printed > 1) return 1;
	return !(t->coef == 1.0 && t->unit[0] == 0);
}

static void bterm(buf *b, const cterm *t, int use_abs)
{
	double c = use_abs ? fabs(t->coef) : t->coef;
	int shown = !(c == 1.0 && t->unit[0] == 0);
	int i;

	if (t->nfact == 0 && t->ndiv == 0) {
		bnum6(b, c);
		bunit(b, t->unit);
		return;
	}
	/* With a divisor and no numerator factor the coefficient has to print
	 * even when it is 1, or `calc(1 / l)` comes out as `calc(/ l)`. */
	if (t->nfact == 0) shown = 1;
#if defined(CANON_NEGCTL) || defined(CANON_NEGCTL_CALC)
	/* THE CALC SABOTAGE: the product comes out in SOURCE order, so
	 * `calc(g * 2)` reads back as `calc(g * 2)` rather than
	 * `calc(2 * g)`. Every other byte of every other value is identical,
	 * the arithmetic is the same arithmetic, the value means exactly what
	 * it meant, and every page renders the same. It is the most
	 * defensible-looking way to get this wrong, which is why it is the
	 * control. */
	for (i = 0; i < t->nfact; i++) {
		if (i) bput(b, " * ", 3);
		bput(b, t->fact[i], -1);
	}
	if (shown) {
		if (t->nfact) bput(b, " * ", 3);
		bnum6(b, c);
		bunit(b, t->unit);
	}
	for (i = 0; i < t->ndiv; i++) {
		bput(b, " / ", 3);
		bput(b, t->div[i], -1);
	}
#else
	if (shown) {
		bnum6(b, c);
		bunit(b, t->unit);
		if (t->nfact) bput(b, " * ", 3);
	}
	for (i = 0; i < t->nfact; i++) {
		if (i) bput(b, " * ", 3);
		bput(b, t->fact[i], -1);
	}
	for (i = 0; i < t->ndiv; i++) {
		bput(b, " / ", 3);
		bput(b, t->div[i], -1);
	}
#endif
}

static void bcsum(buf *b, const csum *s)
{
	int i;

	if (s->n == 0) { bputc(b, '0'); return; }
	for (i = 0; i < s->n; i++) {
		int par = (s->n > 1) && term_compound(&s->t[i]);
		if (i > 0) {
			/* The sign lives in the OPERATOR, not in the term:
			 * `l - 0.2`, never `l + -0.2`. A NaN coefficient has
			 * no sign to read, so it takes the `+` branch and
			 * prints itself. */
			bput(b, (s->t[i].coef < 0) ? " - " : " + ", 3);
		}
		if (par) bputc(b, '(');
		bterm(b, &s->t[i], i > 0);
		if (par) bputc(b, ')');
	}
}

/* ---- parsing ----
 *
 * `names` is the set of bare identifiers this expression may contain beyond
 * the numeric constants -- the channel keywords of a relative colour, or NULL
 * where there are none. It is also what refuses `none` inside a calc: `none`
 * is a channel VALUE and never a math term, and an identifier that is not in
 * the allowed set has nowhere else to go. */
/* What a resolved calc came out as, for a caller that can use the number. */
struct calcres {
	int resolved;
	double num;
	char unit[24];
};

struct calc_ctx {
	int pct_ok;
	int lp;			/* a <length-percentage> context */
	const char *const *names;	/* NULL-terminated, or NULL */
	int nnames;
};

static int calc_sum(lexed *lx, csum *out, const struct calc_ctx *cx, int depth);

/* The numeric constants CSS Values 4 defines. `-infinity` arrives as ONE
 * identifier token, for the same reason `-none` does. */
static int calc_const(const tok *t, double *v)
{
	if (ieq(t->s, t->len, "e")) { *v = 2.718281828459045235; return 1; }
	if (ieq(t->s, t->len, "pi")) { *v = 3.141592653589793238; return 1; }
	if (ieq(t->s, t->len, "infinity")) { *v = INFINITY; return 1; }
	if (ieq(t->s, t->len, "-infinity")) { *v = -INFINITY; return 1; }
	if (ieq(t->s, t->len, "nan")) { *v = NAN; return 1; }
	return 0;
}

/* The functions kept as opaque factors. Split from math_fns because these
 * three take an arbitrary token stream and must NOT have their contents
 * type-checked. */
static int fn_is_substitution(const tok *t)
{
	return ieq(t->s, t->len, "var") || ieq(t->s, t->len, "env") ||
	       ieq(t->s, t->len, "attr");
}

/* Validate the argument list of an opaque math function that has already been
 * emitted. `f0` is the index of its T_FUNC token; the cursor has moved past
 * its closing paren. A shallow copy of the token view is walked, so nothing
 * here owns or frees anything. */
static int calc_check_args(lexed *lx, int f0, int fend,
		const struct calc_ctx *cx, int depth)
{
	lexed sub = *lx;
	int d;

	sub.i = f0 + 1;
	sub.n = fend - 1;		/* stop before the closing paren */
	if (sub.n <= sub.i) return 0;	/* an empty argument list */
	for (;;) {
		csum s;
		if (calc_sum(&sub, &s, cx, depth + 1) != 0) return -1;
		if ((d = sum_cat(&s, cx->lp)) < 0) return -1;
		if (sub.i >= sub.n) break;
		if (sub.t[sub.i].kind != T_COMMA) return -1;
		sub.i++;
	}
	return 0;
}

static int calc_value(lexed *lx, csum *out, const struct calc_ctx *cx, int depth)
{
	const tok *t = cur(lx);
	cterm term;
	double v;

	memset(out, 0, sizeof *out);
	if (depth >= CALC_MAXDEPTH) return -1;

	switch (t->kind) {
	case T_NUM:
		term_num(&term, t->num, "");
		adv(lx);
		return sum_one(out, &term);
	case T_PCT:
		if (!cx->pct_ok) return -1;
		term_num(&term, t->num, "%");
		adv(lx);
		return sum_one(out, &term);
	case T_DIM: {
		char u[24];
		int n = t->len;
		if (n >= (int)sizeof u) return -1;
		memcpy(u, t->s, (size_t)n);
		u[n] = 0;
		if (unit_cat(u) == U_UNKNOWN) return -1;
		term_num(&term, t->num, u);
		adv(lx);
		return sum_one(out, &term);
	}
	case T_IDENT: {
		int i;
		if (calc_const(t, &v)) {
			term_num(&term, v, "");
			adv(lx);
			return sum_one(out, &term);
		}
		if (cx->names == NULL) return -1;
		for (i = 0; i < cx->nnames; i++)
			if (cx->names[i] != NULL &&
			    ieq(t->s, t->len, cx->names[i])) {
				memset(&term, 0, sizeof term);
				term.coef = 1;
				term.nfact = 1;
				snprintf(term.fact[0], CTEXT, "%s", cx->names[i]);
				adv(lx);
				return sum_one(out, &term);
			}
		return -1;
	}
	case T_DELIM:
		if (t->delim != '(') return -1;
		adv(lx);
		if (calc_sum(lx, out, cx, depth + 1) != 0) return -1;
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
		return 0;
	case T_FUNC: {
		int f0 = lx->i, fend;
		buf fb;

		/* A nested calc() is just a group. */
		if (ieq(t->s, t->len, "calc")) {
			adv(lx);
			if (calc_sum(lx, out, cx, depth + 1) != 0) return -1;
			if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
			adv(lx);
			return 0;
		}
		memset(&term, 0, sizeof term);
		term.coef = 1;
		term.nfact = 1;
		fb.p = term.fact[0];
		fb.len = 0;
		fb.cap = CTEXT;
		fb.ovf = 0;
		if (emit_function(lx, &fb, depth + 1) != 0) return -1;
		if (fb.ovf) return -1;
		fend = lx->i;
		if (!fn_is_substitution(&lx->t[f0]) &&
		    calc_check_args(lx, f0, fend, cx, depth) != 0)
			return -1;
		return sum_one(out, &term);
	}
	default:
		return -1;
	}
}

static int calc_product(lexed *lx, csum *out, const struct calc_ctx *cx,
		int depth)
{
	csum acc, rhs, tmp;

	if (calc_value(lx, &acc, cx, depth) != 0) return -1;
	for (;;) {
		char op;
		if (at_end(lx) || cur(lx)->kind != T_DELIM) break;
		op = cur(lx)->delim;
		if (op != '*' && op != '/') break;
		adv(lx);
		if (calc_value(lx, &rhs, cx, depth) != 0) return -1;
		if (op == '*') {
			if (sum_mul(&tmp, &acc, &rhs) != 0) return -1;
		} else {
			if (sum_div(&tmp, &acc, &rhs) != 0) return -1;
		}
		acc = tmp;
	}
	*out = acc;
	return 0;
}

static int calc_sum(lexed *lx, csum *out, const struct calc_ctx *cx, int depth)
{
	csum acc, rhs;

	if (depth >= CALC_MAXDEPTH) return -1;
	memset(&acc, 0, sizeof acc);
	if (calc_product(lx, &rhs, cx, depth) != 0) return -1;
	if (sum_add(&acc, &rhs, 0) != 0) return -1;
	for (;;) {
		int neg;
		if (at_end(lx) || cur(lx)->kind != T_DELIM) break;
		if (cur(lx)->delim != '+' && cur(lx)->delim != '-') break;
		neg = (cur(lx)->delim == '-');
		adv(lx);
		if (calc_product(lx, &rhs, cx, depth) != 0) return -1;
		if (sum_add(&acc, &rhs, neg) != 0) return -1;
	}
	if (sum_cat(&acc, cx->lp) < 0) return -1;

	/* THE CONSTANT SORTS FIRST. `calc(l - 20)` serializes as
	 * `calc(-20 + l)`, and `calc(50 + (10 * sign(1em - 10px)))` was
	 * already in that order in the source, which is why the rule was
	 * invisible until a row wrote the two the other way round. A stable
	 * partition, so everything else keeps the order it was written in --
	 * `(0.5 * b) - (0.5 * g)` must not become `(0.5 * g) - (0.5 * b)`. */
	{
		csum ord;
		int i;
		memset(&ord, 0, sizeof ord);
		for (i = 0; i < acc.n; i++)
			if (acc.t[i].nfact == 0 && acc.t[i].ndiv == 0)
				ord.t[ord.n++] = acc.t[i];
		for (i = 0; i < acc.n; i++)
			if (acc.t[i].nfact != 0 || acc.t[i].ndiv != 0)
				ord.t[ord.n++] = acc.t[i];
		acc = ord;
	}
	*out = acc;
	return 0;
}

/* THE ENTRY POINT for a channel whose value is a math function.
 *
 * `ang_ok` says the channel is a hue and may carry an angle; `pct_ok` says it
 * may carry a percentage. Both are checked on the RESULT -- and pct_ok also
 * travels down, because a percentage with nothing to resolve against is
 * meaningless wherever it sits, not only at the top.
 */
static int calc_channel_lp(lexed *lx, buf *b, int pct_ok, int ang_ok,
		const char *const *names, int nnames, struct calcres *out,
		int lp)
{
	const tok *t = cur(lx);
	struct calc_ctx cx;
	csum s;
	int cat;

	if (out != NULL) { out->resolved = 0; out->num = 0; out->unit[0] = 0; }
	cx.pct_ok = pct_ok;
	cx.lp = lp;
	cx.names = names;
	cx.nnames = nnames;

	if (t->kind != T_FUNC) return -1;

	/* Only calc() is rebuilt from a tree. min()/clamp()/sign() and the
	 * rest have their own serialization rules, so they are validated and
	 * re-emitted from the source tokens; var() is not even validated. */
	if (!ieq(t->s, t->len, "calc")) {
		int f0 = lx->i, fend;
		if (emit_function(lx, b, 1) != 0) return -1;
		fend = lx->i;
		if (fn_is_substitution(&lx->t[f0])) return 0;
		return calc_check_args(lx, f0, fend, &cx, 0);
	}

	adv(lx);
	if (calc_sum(lx, &s, &cx, 0) != 0) return -1;
	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);

	cat = sum_cat(&s, lp);
	if (cat < 0) return -1;
	if (lp) {
		/* A track size is a length or a percentage and never a bare
		 * number: `calc(2)` is not a width. */
		if (cat != U_LEN && cat != U_PCT) return -1;
	} else {
		if (cat == U_PCT && !pct_ok) return -1;
		if (cat == U_ANG && !ang_ok) return -1;
		if (cat != U_NUM && cat != U_PCT && cat != U_ANG) return -1;
	}

	/* A calc that came out as ONE plain number is RESOLVED, and the caller
	 * may want the number rather than the text. rgb()/hsl()/hwb() do:
	 * their canonical form is the legacy one, which has no spelling for a
	 * calc at all, so `rgb(calc(infinity), 0, 0)` has to become
	 * `rgb(255, 0, 0)`. lab() and color() do not: their form CAN hold a
	 * calc, and `lab(calc(infinity) 0 0)` keeps it. */
	if (out != NULL && s.n == 1 && s.t[0].nfact == 0 && s.t[0].ndiv == 0) {
		out->resolved = 1;
		out->num = s.t[0].coef;
		memcpy(out->unit, s.t[0].unit, sizeof out->unit);
	}

	bput(b, "calc(", 5);
	bcsum(b, &s);
	bputc(b, ')');
	return b->ovf ? -1 : 0;
}

static int calc_channel(lexed *lx, buf *b, int pct_ok, int ang_ok,
		const char *const *names, int nnames, struct calcres *out)
{
	return calc_channel_lp(lx, b, pct_ok, ang_ok, names, nnames, out, 0);
}

/* A math function in a <length-percentage> slot. */
static int calc_lp(lexed *lx, buf *b)
{
	return calc_channel_lp(lx, b, 1, 0, NULL, 0, NULL, 1);
}


/* An angle in whatever unit was written, in degrees. */
static double unit_to_deg(double v, const char *u)
{
	if (ieq(u, (int)strlen(u), "grad")) return v * 0.9;
	if (ieq(u, (int)strlen(u), "rad"))
		return v * (180.0 / 3.14159265358979323846);
	if (ieq(u, (int)strlen(u), "turn")) return v * 360.0;
	return v;			/* deg, or a bare number */
}

/* Read ONE component of a colour function. Every kind the grammar can contain
 * is recognised here; deciding which of them a given channel ALLOWS is the
 * caller's job, and that split is what makes `rgb(0, 0, 0deg)` invalid while
 * `hwb(0deg 0% 0%)` is fine. */
static int read_comp(lexed *lx, comp *c, int pct_ok, int ang_ok, int fold)
{
	const tok *t = cur(lx);

	c->kind = K_NUM;
	c->num = 0;
	c->text[0] = 0;

	switch (t->kind) {
	case T_NUM:
		c->kind = K_NUM;
		c->num = t->num;
		adv(lx);
		return 0;
	case T_PCT:
		c->kind = K_PCT;
		c->num = t->num;
		adv(lx);
		return 0;
	case T_DIM:
		if (angle_deg(t, &c->num) != 0) return -1;
		c->kind = K_ANG;
		adv(lx);
		return 0;
	case T_IDENT: {
		int i;
		if (ieq(t->s, t->len, "none")) { c->kind = K_NONE; adv(lx); return 0; }
		if (t->len >= CTEXT) return -1;
		for (i = 0; i < t->len; i++) {
			char ch = t->s[i];
			c->text[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
		}
		c->text[t->len] = 0;
		c->kind = K_IDENT;
		adv(lx);
		return 0;
	}
	case T_FUNC: {
		buf tb;
		struct calcres cr;
		tb.p = c->text;
		tb.len = 0;
		tb.cap = CTEXT;
		tb.ovf = 0;
		/* No channel keywords here: an absolute colour has no `r` to
		 * refer to. That NULL is also what refuses `none` inside a
		 * math function -- `rgb(clamp(10, none, 20) 0 0)` and
		 * `abs(none)` and `-none` -- because an identifier that is
		 * neither a numeric constant nor an allowed name has nowhere
		 * to go. */
		if (calc_channel(lx, &tb, pct_ok, ang_ok, NULL, 0, &cr) != 0)
			return -1;
		if (tb.ovf) return -1;
		/* A RESOLVED calc becomes the number, where the caller's
		 * canonical form has no way to hold a calc. */
		if (fold && cr.resolved) {
			int cat = unit_cat(cr.unit);
			if (cat == U_NUM) { c->kind = K_NUM; c->num = cr.num; return 0; }
			if (cat == U_PCT) { c->kind = K_PCT; c->num = cr.num; return 0; }
			if (cat == U_ANG) {
				c->kind = K_ANG;
				c->num = unit_to_deg(cr.num, cr.unit);
				return 0;
			}
		}
		c->kind = K_TEXT;
		return 0;
	}
	default:
		return -1;
	}
}

enum {
	CF_RGB = 0, CF_HSL, CF_HWB, CF_LAB, CF_LCH, CF_OKLAB, CF_OKLCH,
	CF_COLORFN
};

typedef struct {
	int form;
	const char *space;	/* CF_COLORFN only */
	double v[3];
	int none[3];
	char text[3][CTEXT];	/* non-empty: an unevaluated math function */
	double alpha;
	int anone;
	int ahas;
	char atext[CTEXT];
} ccol;

static int ccol_has_text(const ccol *c)
{
	return c->text[0][0] || c->text[1][0] || c->text[2][0] || c->atext[0];
}

static void bchan(buf *b, const ccol *c, int i)
{
	if (c->text[i][0]) { bput(b, c->text[i], -1); return; }
	if (c->none[i]) { bput(b, "none", 4); return; }
	bnum6(b, c->v[i]);
}

/* The `/ <alpha>` tail of a modern-syntax colour. An alpha of exactly 1 is
 * the default and disappears -- but only when it is a plain number: `none`
 * and an unevaluated calc() are values in their own right and stay. */
static void balpha_modern(buf *b, const ccol *c)
{
	if (!c->ahas) return;
	if (c->atext[0]) { bput(b, " / ", 3); bput(b, c->atext, -1); return; }
	if (c->anone) { bput(b, " / none", 7); return; }
	if (c->alpha == 1) return;
	bput(b, " / ", 3);
	bnum6(b, c->alpha);
}

static void emit_legacy_rgb(buf *b, const double *rgb255, const ccol *c)
{
	double a = c->ahas ? (c->anone ? 0.0 : c->alpha) : 1.0;
	int with_alpha = !(a >= 1.0);

	bput(b, with_alpha ? "rgba(" : "rgb(", -1);
	bint(b, chan255(rgb255[0]));
	bcomma(b);
	bint(b, chan255(rgb255[1]));
	bcomma(b);
	bint(b, chan255(rgb255[2]));
	if (with_alpha) { bcomma(b); balpha_legacy(b, a); }
	bputc(b, ')');
}

static const char *const lab_fn_names[] = { "lab", "lch", "oklab", "oklch" };

static void emit_ccol(buf *b, const ccol *c)
{
	double rgb[3];
	int i;

	switch (c->form) {
	case CF_RGB:
		if (!ccol_has_text(c)) {
			rgb[0] = c->v[0]; rgb[1] = c->v[1]; rgb[2] = c->v[2];
			emit_legacy_rgb(b, rgb, c);
			return;
		}
		/* An unevaluated channel blocks the resolve, and the fallback
		 * spelling is the MODERN one -- `rgb(calc(...) 255 0 / 0.5)`,
		 * space separated, channels on the 0..255 scale. */
		bput(b, "rgb(", 4);
		for (i = 0; i < 3; i++) {
			if (i) bputc(b, ' ');
			if (c->text[i][0]) bput(b, c->text[i], -1);
			else bint(b, chan255(c->v[i]));
		}
		balpha_modern(b, c);
		bputc(b, ')');
		return;

	case CF_HSL:
	case CF_HWB:
		if (!ccol_has_text(c) && !c->none[0] && !c->none[1] &&
		    !c->none[2] && !c->anone) {
			if (c->form == CF_HSL)
				hsl_to_rgb(c->v[0],
					   clampd(c->v[1] / 100.0, 0.0, 1.0),
					   clampd(c->v[2] / 100.0, 0.0, 1.0),
					   rgb);
			else
				hwb_to_rgb(c->v[0], c->v[1] / 100.0,
					   c->v[2] / 100.0, rgb);
			for (i = 0; i < 3; i++) rgb[i] *= 255.0;
			emit_legacy_rgb(b, rgb, c);
			return;
		}
		bput(b, c->form == CF_HSL ? "hsl(" : "hwb(", 4);
		for (i = 0; i < 3; i++) { if (i) bputc(b, ' '); bchan(b, c, i); }
		balpha_modern(b, c);
		bputc(b, ')');
		return;

	case CF_COLORFN:
		bput(b, "color(", 6);
		bput(b, c->space, -1);
		for (i = 0; i < 3; i++) { bputc(b, ' '); bchan(b, c, i); }
		balpha_modern(b, c);
		bputc(b, ')');
		return;

	default:
		bput(b, lab_fn_names[c->form - CF_LAB], -1);
		bputc(b, '(');
		for (i = 0; i < 3; i++) { if (i) bputc(b, ' '); bchan(b, c, i); }
		balpha_modern(b, c);
		bputc(b, ')');
		return;
	}
}

/* ====================================================================
 * Parsing the absolute forms
 * ==================================================================== */

static int set_alpha(ccol *c, const comp *a)
{
	switch (a->kind) {
	case K_NUM: c->alpha = a->num; break;
	case K_PCT: c->alpha = a->num / 100.0; break;
	case K_NONE: c->anone = 1; c->alpha = 0; return 0;
	case K_TEXT: memcpy(c->atext, a->text, sizeof c->atext); return 0;
	default: return -1;		/* an ident, an angle */
	}
	if (isnan(c->alpha)) c->alpha = 0;
	if (c->alpha < 0) c->alpha = 0;
	if (c->alpha > 1) c->alpha = 1;
	return 0;
}

static void set_chan(ccol *c, int i, const comp *k, double scale)
{
	switch (k->kind) {
	case K_NUM: c->v[i] = k->num; break;
	case K_PCT: c->v[i] = k->num * scale / 100.0; break;
	case K_ANG: c->v[i] = k->num; break;
	case K_NONE: c->none[i] = 1; c->v[i] = 0; break;
	case K_TEXT: memcpy(c->text[i], k->text, sizeof c->text[i]); break;
	default: break;
	}
}

/* Read the three channels, the optional alpha and the `)`, for a function
 * that uses the MODERN space-separated syntax only. */
static int read_modern_body(lexed *lx, comp k[3], comp *al, int *have_alpha,
		int hue)
{
	int i;
	*have_alpha = 0;
	/* `hue` names the one channel that takes an angle and refuses a
	 * percentage; -1 where the function has none. Nothing here FOLDS a
	 * resolved calc, because every caller of this function serializes in
	 * a form that can hold one. */
	for (i = 0; i < 3; i++)
		if (read_comp(lx, &k[i], i != hue, i == hue, 0) != 0) return -1;
	if (!at_end(lx) && cur(lx)->kind == T_DELIM && cur(lx)->delim == '/') {
		adv(lx);
		if (read_comp(lx, al, 1, 0, 0) != 0) return -1;
		*have_alpha = 1;
	}
	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	return 0;
}

/* rgb() / rgba(). The cursor is ON the function token.
 *
 * Both syntaxes live here because telling them apart needs a token of
 * lookahead past the first component, and because the LEGACY one is where all
 * the refusals are: no `none`, no angles, no keywords, and the three channels
 * must be all numbers or all percentages. color-invalid-rgb.html is 30
 * subtests of nothing but those. */
static int parse_rgb(lexed *lx, ccol *c)
{
	comp k[3], al;
	int i, legacy = 0, npct = 0, nnum = 0, have_alpha = 0;

	memset(c, 0, sizeof *c);
	c->form = CF_RGB;
	adv(lx);

	if (read_comp(lx, &k[0], 1, 0, 1) != 0) return -1;
	if (!at_end(lx) && cur(lx)->kind == T_COMMA) legacy = 1;
	for (i = 1; i < 3; i++) {
		if (legacy) {
			if (at_end(lx) || cur(lx)->kind != T_COMMA) return -1;
			adv(lx);
		}
		if (read_comp(lx, &k[i], 1, 0, 1) != 0) return -1;
	}
	if (legacy) {
		if (!at_end(lx) && cur(lx)->kind == T_COMMA) {
			adv(lx);
			if (read_comp(lx, &al, 1, 0, 1) != 0) return -1;
			have_alpha = 1;
		}
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
	} else {
		if (!at_end(lx) && cur(lx)->kind == T_DELIM &&
		    cur(lx)->delim == '/') {
			adv(lx);
			if (read_comp(lx, &al, 1, 0, 1) != 0) return -1;
			have_alpha = 1;
		}
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
	}

	for (i = 0; i < 3; i++) {
		if (k[i].kind == K_IDENT || k[i].kind == K_ANG) return -1;
		if (legacy && k[i].kind == K_NONE) return -1;
		if (k[i].kind == K_PCT) npct++;
		else if (k[i].kind == K_NUM) nnum++;
	}
	if (legacy && npct && nnum) return -1;
	if (have_alpha) {
		if (al.kind == K_IDENT || al.kind == K_ANG) return -1;
		if (legacy && al.kind == K_NONE) return -1;
	}

	for (i = 0; i < 3; i++) set_chan(c, i, &k[i], 255.0);
	c->ahas = have_alpha;
	if (have_alpha && set_alpha(c, &al) != 0) return -1;
	return 0;
}

/* hsl() / hsla() / hwb().
 *
 * hwb has no legacy comma form at all ("HWB syntax does not have the hwba
 * function" is a sibling refusal in the same file), so `is_hwb` also means
 * "commas are a parse error here". */
static int parse_hsl_hwb(lexed *lx, ccol *c, int is_hwb)
{
	comp k[3], al;
	int i, legacy = 0, have_alpha = 0;

	memset(c, 0, sizeof *c);
	c->form = is_hwb ? CF_HWB : CF_HSL;
	adv(lx);

	if (read_comp(lx, &k[0], 0, 1, 1) != 0) return -1;
	if (!at_end(lx) && cur(lx)->kind == T_COMMA) {
		if (is_hwb) return -1;
		legacy = 1;
	}
	for (i = 1; i < 3; i++) {
		if (legacy) {
			if (at_end(lx) || cur(lx)->kind != T_COMMA) return -1;
			adv(lx);
		}
		if (read_comp(lx, &k[i], 1, 0, 1) != 0) return -1;
	}
	if (legacy) {
		if (!at_end(lx) && cur(lx)->kind == T_COMMA) {
			adv(lx);
			if (read_comp(lx, &al, 1, 0, 1) != 0) return -1;
			have_alpha = 1;
		}
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
	} else {
		if (!at_end(lx) && cur(lx)->kind == T_DELIM &&
		    cur(lx)->delim == '/') {
			adv(lx);
			if (read_comp(lx, &al, 1, 0, 1) != 0) return -1;
			have_alpha = 1;
		}
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
	}

	/* The hue is a number or an angle -- never a percentage, which is the
	 * whole of `hsl(50%, 50%, 0%)` being invalid. */
	if (k[0].kind == K_PCT || k[0].kind == K_IDENT) return -1;
	if (legacy && k[0].kind == K_NONE) return -1;
	for (i = 1; i < 3; i++) {
		if (k[i].kind == K_IDENT || k[i].kind == K_ANG) return -1;
		/* Legacy hsl() demands percentages for saturation and
		 * lightness: `hsl(0, 50, 30%)` is invalid. */
		if (legacy && k[i].kind != K_PCT) return -1;
	}
	if (have_alpha) {
		if (al.kind == K_IDENT || al.kind == K_ANG) return -1;
		if (legacy && al.kind == K_NONE) return -1;
	}

	set_chan(c, 0, &k[0], 1.0);
	if (!c->none[0] && !c->text[0][0]) c->v[0] = norm_hue(c->v[0]);
	for (i = 1; i < 3; i++) {
		set_chan(c, i, &k[i], 100.0);
		if (c->none[i] || c->text[i][0]) continue;
		/* CLAMPED BELOW, NOT ABOVE, and only hsl():
		 * `hsl(calc(...) -100 300 / 0.5)` is `hsl(calc(...) 0 300)`.
		 * The upper clamp happens at CONVERSION instead, where an
		 * out-of-range lightness would otherwise walk out of the hsl
		 * formula -- it only ever reaches the serializer at all when
		 * some other channel blocked the conversion. hwb() keeps its
		 * two-sided clamp: nothing in the corpus asks otherwise, and
		 * that file is at 38 of 38. */
		if (is_hwb) c->v[i] = clampd(c->v[i], 0.0, 100.0);
		else if (!(c->v[i] > 0)) c->v[i] = 0;
	}
	c->ahas = have_alpha;
	if (have_alpha && set_alpha(c, &al) != 0) return -1;
	return 0;
}

/* lab() / lch() / oklab() / oklch().
 *
 * The scales are the reason this is one function with a table rather than
 * four: every channel of every one of them is "a number, or a percentage of
 * THIS much", and the four functions disagree about `this much` on every
 * channel. lab's a/b run to 125 at 100%, oklab's to 0.4; lch's C to 150,
 * oklch's to 0.4; lab/lch lightness to 100, oklab/oklch to 1. */
static int parse_lab_family(lexed *lx, ccol *c, int form)
{
	comp k[3], al;
	int have_alpha = 0;
	int polar = (form == CF_LCH || form == CF_OKLCH);
	int ok = (form == CF_OKLAB || form == CF_OKLCH);
	double lmax = ok ? 1.0 : 100.0;
	double ab = ok ? 0.4 : 125.0;
	double cmax = ok ? 0.4 : 150.0;

	memset(c, 0, sizeof *c);
	c->form = form;
	adv(lx);

	if (read_modern_body(lx, k, &al, &have_alpha, polar ? 2 : -1) != 0)
		return -1;

	if (k[0].kind == K_IDENT || k[0].kind == K_ANG) return -1;
	if (k[1].kind == K_IDENT || k[1].kind == K_ANG) return -1;
	if (k[2].kind == K_IDENT) return -1;
	if (!polar && k[2].kind == K_ANG) return -1;
	if (polar && k[2].kind == K_PCT) return -1;
	if (have_alpha && (al.kind == K_IDENT || al.kind == K_ANG)) return -1;

	set_chan(c, 0, &k[0], lmax);
	if (!c->none[0] && !c->text[0][0])
		c->v[0] = clampd(c->v[0], 0.0, lmax);

	set_chan(c, 1, &k[1], polar ? cmax : ab);
	if (polar && !c->none[1] && !c->text[1][0] && c->v[1] < 0)
		c->v[1] = 0;

	set_chan(c, 2, &k[2], ab);
	if (polar && !c->none[2] && !c->text[2][0])
		c->v[2] = norm_hue(c->v[2]);

	c->ahas = have_alpha;
	if (have_alpha && set_alpha(c, &al) != 0) return -1;
	return 0;
}

/* The colour spaces color() accepts. `xyz` is an alias that serializes as
 * `xyz-d65`, which is why the table is consulted rather than the author's
 * bytes echoed. */
static const char *const color_spaces[] = {
	"srgb", "srgb-linear", "display-p3", "display-p3-linear",
	"a98-rgb", "prophoto-rgb", "rec2020",
	"xyz", "xyz-d50", "xyz-d65", NULL
};

static int parse_color_fn(lexed *lx, ccol *c)
{
	comp k[3], al;
	int i, have_alpha = 0;
	const char *space;

	memset(c, 0, sizeof *c);
	c->form = CF_COLORFN;
	adv(lx);

	space = tab_lookup(cur(lx), color_spaces);
	if (space == NULL) return -1;
	if (strcmp(space, "xyz") == 0) space = "xyz-d65";
	c->space = space;
	adv(lx);

	if (read_modern_body(lx, k, &al, &have_alpha, -1) != 0) return -1;

	for (i = 0; i < 3; i++)
		if (k[i].kind == K_IDENT || k[i].kind == K_ANG) return -1;
	if (have_alpha && (al.kind == K_IDENT || al.kind == K_ANG)) return -1;

	/* Channels are NOT clamped -- `color(srgb 400% 0 10)` is
	 * `color(srgb 4 0 10)` and -200 stays -200. Clamping "into gamut"
	 * here is the obvious thing to do and destroys the value. */
	for (i = 0; i < 3; i++) set_chan(c, i, &k[i], 1.0);
	c->ahas = have_alpha;
	if (have_alpha && set_alpha(c, &al) != 0) return -1;
	return 0;
}

/* #rgb / #rgba / #rrggbb / #rrggbbaa */
static int parse_hex(const tok *t, ccol *c)
{
	int i, v[4] = { 0, 0, 0, 255 }, n;

	if (t->kind != T_HASH) return -1;
	n = t->len;
	if (n != 3 && n != 4 && n != 6 && n != 8) return -1;
	for (i = 0; i < n; i++) if (!is_hex((unsigned char)t->s[i])) return -1;

	memset(c, 0, sizeof *c);
	c->form = CF_RGB;

#define HEXV(ch) ((ch) <= '9' ? (ch) - '0' : (((ch) | 32) - 'a' + 10))
	if (n <= 4) {
		for (i = 0; i < n; i++) {
			int d = HEXV((unsigned char)t->s[i]);
			v[i] = d * 16 + d;
		}
	} else {
		for (i = 0; i < n / 2; i++)
			v[i] = HEXV((unsigned char)t->s[i * 2]) * 16 +
			       HEXV((unsigned char)t->s[i * 2 + 1]);
	}
#undef HEXV
	for (i = 0; i < 3; i++) c->v[i] = v[i];
	if (n == 4 || n == 8) {
		c->ahas = 1;
		c->alpha = v[3] / 255.0;
	}
	return 0;
}

/* ====================================================================
 * The structural forms: relative colour, color-mix(), color-layers(),
 * light-dark(). None of these RESOLVES; each serializes its arguments.
 * ==================================================================== */

#define CANON_COLOR_MAXDEPTH 8
#define MIX_MAXCOLORS 8

static int canon_color_ctx(lexed *lx, buf *b, int depth, int origin);
static int canon_color(lexed *lx, buf *b, int depth);

/* The channel keywords each relative form exposes, plus the index of the
 * channel that may carry an ANGLE. Getting these tables wrong in either
 * direction is visible: `rgb(from rebeccapurple h g b)` must be refused and
 * `hwb(from rebeccapurple 0deg 0% 0%)` must be accepted. */
struct rel_kind {
	const char *fn;		/* canonical function name */
	const char *ch[4];	/* the three channels + "alpha" */
	int hue;		/* index of the angle-bearing channel, or -1 */
};

static const struct rel_kind rel_kinds[] = {
	{ "rgb",   { "r", "g", "b", "alpha" }, -1 },
	{ "hsl",   { "h", "s", "l", "alpha" },  0 },
	{ "hwb",   { "h", "w", "b", "alpha" },  0 },
	{ "lab",   { "l", "a", "b", "alpha" }, -1 },
	{ "lch",   { "l", "c", "h", "alpha" },  2 },
	{ "oklab", { "l", "a", "b", "alpha" }, -1 },
	{ "oklch", { "l", "c", "h", "alpha" },  2 },
	{ "color", { "r", "g", "b", "alpha" }, -1 },
	{ NULL,    { NULL, NULL, NULL, NULL },  -1 }
};

static const struct rel_kind rel_kind_xyz = {
	"color", { "x", "y", "z", "alpha" }, -1
};

/* The relative form of an alias: `rgba(from ...)` is `rgb(from ...)`. */
static const struct rel_kind *rel_lookup(const tok *t)
{
	int i;
	if (t->kind != T_FUNC) return NULL;
	if (ieq(t->s, t->len, "rgba")) return &rel_kinds[0];
	if (ieq(t->s, t->len, "hsla")) return &rel_kinds[1];
	for (i = 0; rel_kinds[i].fn != NULL; i++)
		if (ieq(t->s, t->len, rel_kinds[i].fn)) return &rel_kinds[i];
	return NULL;
}

/* One channel expression of a relative colour: a channel keyword, `none`, a
 * number, a percentage, an angle where the space has one, or a math function.
 * Emitted as written (normalised), because a relative colour is not resolved
 * and its channels are an expression, not a value. */
static int rel_channel(lexed *lx, buf *b, const struct rel_kind *k,
		int is_hue, int is_alpha)
{
	const tok *t = cur(lx);
	int i;

	switch (t->kind) {
	case T_IDENT:
		if (ieq(t->s, t->len, "none")) { bput(b, "none", 4); adv(lx); return 0; }
		for (i = 0; i < 4; i++)
			if (ieq(t->s, t->len, k->ch[i])) {
				bput(b, k->ch[i], -1);
				adv(lx);
				return 0;
			}
		return -1;
	case T_NUM:
	case T_PCT:
		if (is_hue && t->kind == T_PCT) return -1;
		return emit_token(lx, b, 1);
	case T_DIM: {
		double d;
		if (!is_hue || is_alpha) return -1;
		if (angle_deg(t, &d) != 0) return -1;
		(void)d;
		return emit_token(lx, b, 1);
	}
	case T_FUNC:
		/* A channel keyword inside a relative colour's calc() carries
		 * the CHANNEL's type, and every one of them is a <number> --
		 * `h` included, which is the surprising one. So `calc(r + 1%)`
		 * and `calc(h + 1deg)` are sums whose terms do not combine,
		 * and the unit algebra refuses them without needing a rule of
		 * its own. This used to be a heuristic ("a term that names a
		 * channel and a term that carries a unit cannot both be in
		 * here"); it is now the same check every other expression
		 * gets. */
		return calc_channel(lx, b, !is_hue, is_hue && !is_alpha,
				k->ch, 4, NULL);
	default:
		return -1;
	}
}

/* `<fn>( from <color> [<space>]? <c> <c> <c> [/ <a>]? )`. The cursor is on
 * the function token and the token after it is known to be `from`. */
static int canon_relative(lexed *lx, buf *b, int depth)
{
	const struct rel_kind *k = rel_lookup(cur(lx));
	int i;

	if (k == NULL) return -1;
	if (depth >= CANON_COLOR_MAXDEPTH) return -1;

	bput(b, k->fn, -1);
	bput(b, "(from ", 6);
	adv(lx);			/* the function */
	adv(lx);			/* `from` */

	if (canon_color_ctx(lx, b, depth + 1, 1) != 0) return -1;

	if (strcmp(k->fn, "color") == 0) {
		const char *space = tab_lookup(cur(lx), color_spaces);
		if (space == NULL) return -1;
		if (strcmp(space, "xyz") == 0) space = "xyz-d65";
		bputc(b, ' ');
		bput(b, space, -1);
		adv(lx);
		/* The channel keywords are the SPACE's, not color()'s: an xyz
		 * space exposes x/y/z and an rgb one exposes r/g/b, and
		 * neither accepts the other's. `color(from ... srgb x g b)`
		 * is invalid, which is 21 subtests of
		 * color-invalid-relative-color.html on its own. */
		if (strncmp(space, "xyz", 3) == 0) k = &rel_kind_xyz;
	}

	for (i = 0; i < 3; i++) {
		bputc(b, ' ');
		if (rel_channel(lx, b, k, i == k->hue, 0) != 0) return -1;
	}
	if (!at_end(lx) && cur(lx)->kind == T_DELIM && cur(lx)->delim == '/') {
		adv(lx);
		bput(b, " / ", 3);
		if (rel_channel(lx, b, k, 0, 1) != 0) return -1;
	}
	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	bputc(b, ')');
	return 0;
}

/* Step the cursor past one <color> without serializing it. Used by the
 * color-mix() percentage pass, which has to see EVERY percentage before it
 * can spell ANY of them. */
static int skip_color(lexed *lx)
{
	const tok *t = cur(lx);
	int depth;

	if (t->kind == T_IDENT || t->kind == T_HASH) { adv(lx); return 0; }
	if (t->kind != T_FUNC) return -1;
	depth = 0;
	while (!at_end(lx)) {
		t = cur(lx);
		if (t->kind == T_FUNC) depth++;
		else if (t->kind == T_RPAREN) {
			depth--;
			if (depth == 0) { adv(lx); return 0; }
		}
		adv(lx);
	}
	return -1;
}

/* A colour-mix percentage: a literal, or a math function kept as text. */
struct mixpct {
	int kind;		/* 0 absent, 1 literal, 2 text */
	double val;
	char text[CTEXT];
};

static int read_mixpct(lexed *lx, struct mixpct *p)
{
	const tok *t = cur(lx);

	p->kind = 0;
	p->val = 0;
	p->text[0] = 0;
	if (t->kind == T_PCT) {
		p->kind = 1;
		p->val = t->num;
		adv(lx);
		/* A weight outside [0,100] is a parse error, not a clamp:
		 * color-invalid-color-mix-function.html is 141 subtests and
		 * more than half of them are exactly this. */
		if (p->val < 0 || p->val > 100) return -1;
		return 0;
	}
	if (t->kind == T_FUNC) {
		buf tb;
		tb.p = p->text;
		tb.len = 0;
		tb.cap = CTEXT;
		tb.ovf = 0;
		if (emit_function(lx, &tb, 1) != 0) return -1;
		if (tb.ovf) return -1;
		p->kind = 2;
		return 0;
	}
	return -1;
}

/* Is this token the start of a percentage rather than of a colour? A bare
 * percentage obviously is; a math function is one too, and nothing else can
 * be, because no colour function shares a name with a math function. */
static int starts_pct(lexed *lx)
{
	const tok *t = cur(lx);
	int i;
	if (t->kind == T_PCT) return 1;
	if (t->kind != T_FUNC) return 0;
	for (i = 0; math_fns[i] != NULL; i++)
		if (ieq(t->s, t->len, math_fns[i])) return 1;
	return 0;
}

static const char *const mix_spaces[] = {
	"srgb", "srgb-linear", "display-p3", "display-p3-linear",
	"a98-rgb", "prophoto-rgb", "rec2020", "lab", "oklab",
	"xyz", "xyz-d50", "xyz-d65",
	"hsl", "hwb", "lch", "oklch", NULL
};

static const char *const hue_methods[] = {
	"shorter", "longer", "increasing", "decreasing", NULL
};

static int space_is_polar(const char *s)
{
	return strcmp(s, "hsl") == 0 || strcmp(s, "hwb") == 0 ||
	       strcmp(s, "lch") == 0 || strcmp(s, "oklch") == 0;
}

/*
 * color-mix( [ in <space> [<hue-method> hue]? , ]? <color> <pct>?
 *            [ , <color> <pct>? ]* )
 *
 * THE PERCENTAGE NORMALISATION IS THE WHOLE TEST, and it is not a clamp:
 *
 *   - a missing weight is filled in so the weights sum to 100, shared equally
 *     among however many are missing. `red 50%, green, blue` is
 *     `red 50%, green 25%, blue 25%`.
 *   - if, after that, every weight is exactly 100/n, they ALL disappear.
 *     `red 50%, blue 50%` is `red, blue`; `red 100%` is `red`.
 *   - if any weight is a calc(), NOTHING is normalised: the ones that were
 *     written are kept and the ones that were not stay missing.
 *   - `shorter hue` is the default and disappears; the other three stay.
 *
 * Two passes over the same tokens, because rule two cannot be decided until
 * every weight has been seen and the colours must be emitted in order.
 */
static int canon_color_mix(lexed *lx, buf *b, int depth)
{
	struct mixpct pct[MIX_MAXCOLORS];
	int start[MIX_MAXCOLORS];
	int n = 0, i, after = 0, any_text = 0, any_given = 0, missing = 0;
	int have_space = 0, polar = 0;
	const char *space = NULL, *method = NULL;
	double sum = 0, share;

	if (depth >= CANON_COLOR_MAXDEPTH) return -1;
	adv(lx);			/* color-mix( */

	if (tok_is_ident(cur(lx), "in")) {
		adv(lx);
		space = tab_lookup(cur(lx), mix_spaces);
		if (space == NULL) return -1;
		if (strcmp(space, "xyz") == 0) space = "xyz-d65";
		polar = space_is_polar(space);
		adv(lx);
		have_space = 1;
		if (!at_end(lx) && cur(lx)->kind == T_IDENT) {
			method = tab_lookup(cur(lx), hue_methods);
			if (method == NULL) return -1;
			if (!polar) return -1;	/* no hue on a rectangular space */
			adv(lx);
			if (!tok_is_ident(cur(lx), "hue")) return -1;
			adv(lx);
		}
		if (at_end(lx) || cur(lx)->kind != T_COMMA) return -1;
		adv(lx);
	}

	/* Pass one: weights only. */
	for (;;) {
		struct mixpct lead;
		if (n >= MIX_MAXCOLORS) return -1;
		start[n] = lx->i;
		lead.kind = 0;
		if (starts_pct(lx) && read_mixpct(lx, &lead) != 0) return -1;
		if (skip_color(lx) != 0) return -1;
		pct[n] = lead;
		if (!at_end(lx) && cur(lx)->kind != T_COMMA &&
		    cur(lx)->kind != T_RPAREN) {
			if (lead.kind != 0) return -1;	/* a weight on both sides */
			if (read_mixpct(lx, &pct[n]) != 0) return -1;
		}
		n++;
		if (at_end(lx)) return -1;
		if (cur(lx)->kind == T_RPAREN) { adv(lx); after = lx->i; break; }
		if (cur(lx)->kind != T_COMMA) return -1;
		adv(lx);
	}
	if (n == 0) return -1;

	for (i = 0; i < n; i++) {
		if (pct[i].kind == 2) any_text = 1;
		else if (pct[i].kind == 1) { any_given = 1; sum += pct[i].val; }
		else missing++;
	}

	if (!any_text && any_given) {
		if (missing > 0) {
			share = (100.0 - sum) / missing;
			for (i = 0; i < n; i++)
				if (pct[i].kind == 0) {
					pct[i].kind = 1;
					pct[i].val = share;
				}
		}
		/* Every weight equal to its fair share means no weight at
		 * all: that is what turns `red 50%, blue 50%` into
		 * `red, blue` and `red 100%` into `red`. */
		share = 100.0 / n;
		for (i = 0; i < n; i++)
			if (fabs(pct[i].val - share) > 1e-9) break;
		if (i == n) for (i = 0; i < n; i++) pct[i].kind = 0;
	}

	/* Pass two. */
	bput(b, "color-mix(", 10);
	if (have_space) {
		bput(b, "in ", 3);
		bput(b, space, -1);
#ifdef CANON_NEGCTL
		if (method != NULL) {
#else
		if (method != NULL && strcmp(method, "shorter") != 0) {
#endif
			bputc(b, ' ');
			bput(b, method, -1);
			bput(b, " hue", 4);
		}
		bcomma(b);
	}
	for (i = 0; i < n; i++) {
		if (i) bcomma(b);
		lx->i = start[i];
		if (starts_pct(lx)) {
			struct mixpct skip;
			if (read_mixpct(lx, &skip) != 0) return -1;
		}
		if (canon_color(lx, b, depth + 1) != 0) return -1;
		if (pct[i].kind == 1) {
			bputc(b, ' ');
			bnum6(b, pct[i].val);
			bputc(b, '%');
		} else if (pct[i].kind == 2) {
			bputc(b, ' ');
			bput(b, pct[i].text, -1);
		}
	}
	bputc(b, ')');
	/* Pass two rewound the cursor once per colour; pass one already knew
	 * where the function ends, so that is where it goes back to. Getting
	 * this wrong is invisible at the top level and breaks the moment a
	 * color-mix() is nested inside anything. */
	lx->i = after;
	return 0;
}

/* color-layers( [<blend-mode> ,]? <color># ). `normal` is the initial value
 * and disappears; every other mode stays. */
static const char *const blend_modes[] = {
	"normal", "multiply", "screen", "overlay", "darken", "lighten",
	"color-dodge", "color-burn", "hard-light", "soft-light",
	"difference", "exclusion", "hue", "saturation", "color",
	"luminosity", NULL
};

static int canon_color_layers(lexed *lx, buf *b, int depth)
{
	const char *mode = NULL;
	int n = 0;

	if (depth >= CANON_COLOR_MAXDEPTH) return -1;
	adv(lx);

	/* The mode is an identifier followed by a comma. No blend mode is
	 * also a colour keyword, so there is nothing to disambiguate. */
	if (cur(lx)->kind == T_IDENT && pk(lx, 1)->kind == T_COMMA) {
		mode = tab_lookup(cur(lx), blend_modes);
		if (mode == NULL) return -1;
		adv(lx);
		adv(lx);
	}

	bput(b, "color-layers(", 13);
	if (mode != NULL && strcmp(mode, "normal") != 0) {
		bput(b, mode, -1);
		bcomma(b);
	}
	for (;;) {
		if (n) bcomma(b);
		if (canon_color(lx, b, depth + 1) != 0) return -1;
		n++;
		if (at_end(lx)) return -1;
		if (cur(lx)->kind == T_RPAREN) { adv(lx); break; }
		if (cur(lx)->kind != T_COMMA) return -1;
		adv(lx);
	}
	if (n == 0) return -1;
	bputc(b, ')');
	return 0;
}

/* light-dark(<color>, <color>) */
static int canon_light_dark(lexed *lx, buf *b, int depth)
{
	if (depth >= CANON_COLOR_MAXDEPTH) return -1;
	adv(lx);
	bput(b, "light-dark(", 11);
	if (canon_color(lx, b, depth + 1) != 0) return -1;
	if (at_end(lx) || cur(lx)->kind != T_COMMA) return -1;
	adv(lx);
	bcomma(b);
	if (canon_color(lx, b, depth + 1) != 0) return -1;
	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	bputc(b, ')');
	return 0;
}

/* The Color 4/5 functions this file recognises but does not spell: their
 * serialization needs colour-space conversion or gamut mapping that a
 * specified value does not otherwise require, and doing it wrong is worse
 * than not doing it. They are still parsed structurally and emitted with
 * their whitespace normalised, because DROPPING them is the worse bug -- see
 * the note on the caller. */
static const char *const raw_color_fns[] = {
	"contrast-color", "device-cmyk", "alpha", NULL
};

/* A structural check, not a grammar: the argument list must be non-empty and
 * made only of the token kinds a colour function can contain. */
static int color_fn_body_ok(lexed *lx, int start)
{
	int i, depth = 0, ntok = 0;

	for (i = start; i < lx->n; i++) {
		const tok *t = &lx->t[i];
		switch (t->kind) {
		case T_FUNC:
			depth++;
			ntok++;
			break;
		case T_RPAREN:
			if (depth == 0) return ntok > 0 && i == lx->n - 1;
			depth--;
			break;
		case T_IDENT: case T_NUM: case T_PCT: case T_DIM:
		case T_HASH: case T_COMMA:
			ntok++;
			break;
		case T_DELIM:
			if (t->delim != '/' && t->delim != '+' &&
			    t->delim != '-' && t->delim != '*' &&
			    t->delim != '.')
				return 0;
			ntok++;
			break;
		default:
			return 0;
		}
	}
	return 0;
}

/* Emit the function with its whitespace normalized and nothing else changed. */
static int emit_color_fn_raw(lexed *lx, buf *b, const char *name)
{
	int first = 1, depth = 0;

	bput(b, name, -1);
	bputc(b, '(');
	adv(lx);
	while (!at_end(lx)) {
		const tok *t = cur(lx);

		if (t->kind == T_RPAREN && depth == 0) { adv(lx); break; }
		if (t->kind == T_COMMA) {
			bcomma(b);
			adv(lx);
			first = 1;
			continue;
		}
		if (!first && t->wsbefore) bputc(b, ' ');
		if (t->kind == T_FUNC) {
			bident(b, t->s, t->len);
			bputc(b, '(');
			depth++;
			adv(lx);
			first = 1;
			continue;
		}
		if (t->kind == T_RPAREN) {
			bputc(b, ')');
			depth--;
			adv(lx);
			first = 0;
			continue;
		}
		if (emit_token(lx, b, CANON_MAXDEPTH - 1) != 0) return -1;
		first = 0;
	}
	bputc(b, ')');
	return 0;
}

/* ====================================================================
 * canon_color -- ONE <color>, parsed and spelled. The recursion point.
 * ==================================================================== */

/* `origin` marks the ORIGIN slot of a relative colour, and it changes exactly
 * one thing: a `none` in an hsl()/hwb() there RESOLVES TO ZERO instead of
 * blocking the conversion to sRGB. `hsl(from hsl(none none none) h s l)` is
 * `hsl(from rgb(0, 0, 0) h s l)`, not `hsl(from hsl(none none none) h s l)`.
 *
 * That is not a rule anyone would derive -- everywhere else in Color 4 a
 * `none` is a value that survives, and the corpus itself carries a "FIXME:
 * Clarify with spec editors if 'none' should pass through to the constants"
 * next to these rows. It is what browsers do, so it is what is transcribed;
 * 30 subtests of color-valid-relative-color.html turn on it. */
static int canon_color_ctx(lexed *lx, buf *b, int depth, int origin)
{
	const tok *t;
	ccol c;

	if (depth >= CANON_COLOR_MAXDEPTH) return -1;
	if (at_end(lx)) return -1;
	t = cur(lx);

	if (t->kind == T_HASH) {
		if (parse_hex(t, &c) != 0) return -1;
		adv(lx);
		emit_ccol(b, &c);
		return 0;
	}
	if (t->kind == T_IDENT) {
		const char *kw = color_keyword(t);
		if (kw == NULL) return -1;
		bput(b, kw, -1);
		adv(lx);
		return 0;
	}
	if (t->kind != T_FUNC) return -1;

	/* The functions this file recognises but does not spell come FIRST,
	 * because one of them -- alpha() -- takes a `from` of its own and
	 * would otherwise be mistaken for relative colour syntax. */
	{
		int i;
		for (i = 0; raw_color_fns[i] != NULL; i++)
			if (ieq(t->s, t->len, raw_color_fns[i]))
				return emit_color_fn_raw(lx, b,
						raw_color_fns[i]);
	}

	/* An ORIGIN may be a var(). It is not a colour yet -- the value is a
	 * pending substitution and nobody knows what colour it names until
	 * the cascade runs -- but it is a legal origin and the corpus uses it
	 * eight times over. `var()` is accepted here and NOWHERE else in this
	 * function, because a var() standing alone as the whole value is
	 * LibCSS's pre-pass to resolve, not this file's to spell. */
	if (ieq(t->s, t->len, "var") || ieq(t->s, t->len, "env"))
		return emit_function(lx, b, depth + 1);

	/* Relative colour syntax is signalled by `from` and nothing else. */
	if (tok_is_ident(pk(lx, 1), "from")) return canon_relative(lx, b, depth);

	if (ieq(t->s, t->len, "color-mix")) return canon_color_mix(lx, b, depth);
	if (ieq(t->s, t->len, "color-layers"))
		return canon_color_layers(lx, b, depth);
	if (ieq(t->s, t->len, "light-dark"))
		return canon_light_dark(lx, b, depth);

	if (ieq(t->s, t->len, "rgb") || ieq(t->s, t->len, "rgba")) {
		if (parse_rgb(lx, &c) != 0) return -1;
	} else if (ieq(t->s, t->len, "hsl") || ieq(t->s, t->len, "hsla")) {
		if (parse_hsl_hwb(lx, &c, 0) != 0) return -1;
	} else if (ieq(t->s, t->len, "hwb")) {
		if (parse_hsl_hwb(lx, &c, 1) != 0) return -1;
	} else if (ieq(t->s, t->len, "lab")) {
		if (parse_lab_family(lx, &c, CF_LAB) != 0) return -1;
	} else if (ieq(t->s, t->len, "lch")) {
		if (parse_lab_family(lx, &c, CF_LCH) != 0) return -1;
	} else if (ieq(t->s, t->len, "oklab")) {
		if (parse_lab_family(lx, &c, CF_OKLAB) != 0) return -1;
	} else if (ieq(t->s, t->len, "oklch")) {
		if (parse_lab_family(lx, &c, CF_OKLCH) != 0) return -1;
	} else if (ieq(t->s, t->len, "color")) {
		if (parse_color_fn(lx, &c) != 0) return -1;
	} else {
		return -1;
	}
	if (origin && (c.form == CF_HSL || c.form == CF_HWB)) {
		int i;
		for (i = 0; i < 3; i++) c.none[i] = 0;
		c.anone = 0;
	}
	emit_ccol(b, &c);
	return 0;
}

static int canon_color(lexed *lx, buf *b, int depth)
{
	return canon_color_ctx(lx, b, depth, 0);
}

/* Does the value contain a substitution function anywhere?
 *
 * A declaration whose value contains var() is a PENDING-SUBSTITUTION VALUE,
 * and the CSSOM serializes one as the ORIGINAL token sequence -- there is
 * nothing to canonicalise, because nobody knows what the value is until the
 * cascade substitutes. `lab(from var(--c) l a b / calc(alpha * 0.8))` reads
 * back with its `alpha * 0.8` in the order it was written, NOT reordered to
 * `0.8 * alpha`, and `lch(from var(--c) calc(l / 2) c h)` keeps its division
 * rather than folding it into `0.5 * l`.
 *
 * This is the one place the calculation tree has to stand down, and it is not
 * an exception to the serialization rule -- it IS the serialization rule for
 * this class of value. Found by the tree: three subtests that had been
 * passing on the old verbatim path started failing the moment calc() began
 * spelling things properly. */
static int value_has_var(lexed *lx)
{
	int i;
	for (i = 0; i < lx->n; i++)
		if (lx->t[i].kind == T_FUNC &&
		    (ieq(lx->t[i].s, lx->t[i].len, "var") ||
		     ieq(lx->t[i].s, lx->t[i].len, "env")))
			return 1;
	return 0;
}

/* Does this file claim the VALUE in front of it? A colour property whose
 * value is a bare keyword, a hex literal or anything else LibCSS already
 * spells correctly is left alone -- claiming a property is not claiming every
 * value of it, and rerouting a value that works today through a second
 * serializer buys nothing and risks a regression. */
static const char *const claimed_color_fns[] = {
	"rgb", "rgba", "hsl", "hsla", "hwb", "lab", "lch", "oklab", "oklch",
	"color", "color-mix", "color-layers", "light-dark", NULL
};

/* The properties whose value is a <color>. Claiming `color()` needs a
 * property that actually takes one: `width: color(srgb 0 0 0)` is invalid, not
 * a colour. */
static const char *const color_props[] = {
	"color", "background-color", "border-color",
	"border-top-color", "border-right-color",
	"border-bottom-color", "border-left-color",
	"border-block-color", "border-inline-color",
	"border-block-start-color", "border-block-end-color",
	"border-inline-start-color", "border-inline-end-color",
	"outline-color", "text-decoration-color", "text-emphasis-color",
	"caret-color", "column-rule-color", "accent-color",
	"fill", "stroke", "stop-color", "flood-color", "lighting-color",
	NULL
};

/* ====================================================================
 * font-family
 *
 *   font-family: <family-name>#
 *   <family-name> = <string> | <custom-ident>+
 *
 * The corner that makes this worth a parser rather than a pass-through is
 * that ONE family name may be SEVERAL identifiers -- `quite simple` is a
 * single family, not two -- while a quoted name is exactly one token. Mixing
 * them in one slot is invalid, which is what makes
 * `arial, helvetica, 'times' new roman, sans-serif` invalid even though every
 * piece of it is individually fine. LibCSS accepts that today.
 *
 * The other corner is that an identifier is not the same thing as a word.
 * `0simple` is a DIMENSION (the number 0 with the unit `simple`), which is why
 * it cannot be a family name and why the check has to happen at the token
 * level rather than by scanning characters. Same for `#simple` (a hash),
 * `simple()` (a function) and `simple!` (a delimiter).
 *
 * Escapes are the third: `\073 imple` is the single identifier `simple` (a hex
 * escape eats one following space), while `\s imple` is the TWO identifiers
 * `s` and `imple` -- the same-looking input differing only in whether the
 * escape was hex. Both are valid; they are different values. The scanner
 * already decodes both, so this only has to spell the result back correctly,
 * which is what bident() is for.
 * ==================================================================== */

/* Reserved in <family-name>. The two halves are NOT the same rule and the
 * difference shows up on a one-word value:
 *
 *   - a CSS-wide keyword is a legal whole value (`font-family: inherit` means
 *     inherit) and only a parse error INSIDE a list, where it would have to
 *     be a family name;
 *   - `default` is reserved in <family-name> and is not a CSS-wide keyword,
 *     so it is a parse error in BOTH positions.
 */
static int ff_css_wide(const tok *t)
{
	return tok_is_ident(t, "initial") || tok_is_ident(t, "inherit") ||
	       tok_is_ident(t, "unset") || tok_is_ident(t, "revert") ||
	       tok_is_ident(t, "revert-layer");
}

static int ff_reserved(const tok *t)
{
	return ff_css_wide(t) || tok_is_ident(t, "default");
}

static int canon_font_family(lexed *lx, buf *b)
{
	int slot = 0;

	for (;;) {
		const tok *t = cur(lx);
		int nid = 0;

		if (slot > 0) bcomma(b);

		if (t->kind == T_STR) {
			/* A quoted name is the whole slot. `'times' new roman`
			 * is invalid, not a name of three words. */
			adv(lx);
			if (!at_end(lx) && cur(lx)->kind != T_COMMA) return -1;
#ifdef CANON_NEGCTL
			/* The control: single quotes. Still a string, still
			 * re-parses to itself, still the wrong bytes -- the
			 * CSSOM says a string serializes with double quotes. */
			bputc(b, '\'');
			bput(b, t->s, t->len);
			bputc(b, '\'');
#else
			bstring(b, t->s, t->len);
#endif
		} else if (t->kind == T_IDENT) {
			const tok *first = t;

			while (!at_end(lx) && cur(lx)->kind == T_IDENT) {
				if (nid > 0) bputc(b, ' ');
				bident(b, cur(lx)->s, cur(lx)->len);
				adv(lx);
				nid++;
			}
			/* A slot of exactly one identifier may not be a
			 * reserved word; two or more may (`default bongo` is a
			 * perfectly good family name). */
			if (nid == 1 && ff_reserved(first)) return -1;
			if (!at_end(lx) && cur(lx)->kind != T_COMMA) return -1;
		} else {
			/* A dimension (`0simple`), hash (`#simple`), function
			 * (`simple()`), number or delimiter (`simple!`,
			 * `quite@simple`). None of these is an identifier. */
			return -1;
		}

		slot++;
		if (at_end(lx)) break;
		if (cur(lx)->kind != T_COMMA) return -1;
		adv(lx);
		if (at_end(lx)) return -1;	/* trailing comma */
	}
	return slot > 0 ? 0 : -1;
}

/* ====================================================================
 * position-area
 *
 * A grid cell named by one or two keywords. What makes it a real parse rather
 * than a keyword lookup is that the two keywords are an UNORDERED pair drawn
 * from paired axes, and the serialization is ordered -- so `top left` and
 * `left top` are the same value and only one spelling is correct.
 *
 * Three rules, all read off position-area-parsing.html:
 *
 *  1. AXIS ORDER. The pair is emitted in a fixed slot order regardless of how
 *     it was written: horizontal before vertical, block before inline, and
 *     self-block before self-inline. `top left` serializes `left top`.
 *
 *  2. `span-all` IS THE DEFAULT AND DISAPPEARS. `left span-all` is `left`.
 *     But only where the other keyword names an axis -- see rule 3.
 *
 *  3. THE AMBIGUOUS GROUPS DO NEITHER. `start`/`end`/`span-start`/`span-end`
 *     (and their self- forms) do not say which axis they are on, so nothing
 *     can be reordered and nothing can be dropped: `start span-all` keeps its
 *     span-all and `center start` keeps its order, while the otherwise
 *     identical `center left` becomes `left center`. Getting this wrong is
 *     invisible in a spot check and is 200-odd subtests.
 *
 * Pairs must come from the SAME group: `left inline-start` mixes a physical
 * axis with a logical one and is invalid, as is any pair on the same axis
 * (`left right`), except within an ambiguous group where `start end` is fine.
 * ==================================================================== */

enum {
	PA_XY = 0,	/* physical: left/right x top/bottom */
	PA_BI,		/* logical: block x inline */
	PA_SBI,		/* logical, self-relative: self-block x self-inline */
	PA_SE,		/* start/end -- axis-ambiguous */
	PA_SSE,		/* self-start/self-end -- axis-ambiguous */
	PA_UNIV		/* center / span-all -- any axis */
};

struct pa_kw {
	const char *name;
	unsigned char group;
	unsigned char axis;	/* 0 or 1 within the group; 0 for the rest */
};

static const struct pa_kw pa_kws[] = {
	/* horizontal (axis 0 of PA_XY) */
	{ "left", PA_XY, 0 }, { "right", PA_XY, 0 },
	{ "span-left", PA_XY, 0 }, { "span-right", PA_XY, 0 },
	{ "x-start", PA_XY, 0 }, { "x-end", PA_XY, 0 },
	{ "span-x-start", PA_XY, 0 }, { "span-x-end", PA_XY, 0 },
	{ "self-x-start", PA_XY, 0 }, { "self-x-end", PA_XY, 0 },
	{ "span-self-x-start", PA_XY, 0 }, { "span-self-x-end", PA_XY, 0 },
	/* vertical (axis 1 of PA_XY) */
	{ "top", PA_XY, 1 }, { "bottom", PA_XY, 1 },
	{ "span-top", PA_XY, 1 }, { "span-bottom", PA_XY, 1 },
	{ "y-start", PA_XY, 1 }, { "y-end", PA_XY, 1 },
	{ "span-y-start", PA_XY, 1 }, { "span-y-end", PA_XY, 1 },
	{ "self-y-start", PA_XY, 1 }, { "self-y-end", PA_XY, 1 },
	{ "span-self-y-start", PA_XY, 1 }, { "span-self-y-end", PA_XY, 1 },
	/* block (axis 0 of PA_BI) -- block sorts BEFORE inline */
	{ "block-start", PA_BI, 0 }, { "block-end", PA_BI, 0 },
	{ "span-block-start", PA_BI, 0 }, { "span-block-end", PA_BI, 0 },
	/* inline (axis 1 of PA_BI) */
	{ "inline-start", PA_BI, 1 }, { "inline-end", PA_BI, 1 },
	{ "span-inline-start", PA_BI, 1 }, { "span-inline-end", PA_BI, 1 },
	/* self-block (axis 0 of PA_SBI) */
	{ "self-block-start", PA_SBI, 0 }, { "self-block-end", PA_SBI, 0 },
	{ "span-self-block-start", PA_SBI, 0 },
	{ "span-self-block-end", PA_SBI, 0 },
	/* self-inline (axis 1 of PA_SBI) */
	{ "self-inline-start", PA_SBI, 1 }, { "self-inline-end", PA_SBI, 1 },
	{ "span-self-inline-start", PA_SBI, 1 },
	{ "span-self-inline-end", PA_SBI, 1 },
	/* axis-ambiguous */
	{ "start", PA_SE, 0 }, { "end", PA_SE, 0 },
	{ "span-start", PA_SE, 0 }, { "span-end", PA_SE, 0 },
	{ "self-start", PA_SSE, 0 }, { "self-end", PA_SSE, 0 },
	{ "span-self-start", PA_SSE, 0 }, { "span-self-end", PA_SSE, 0 },
	/* any axis */
	{ "center", PA_UNIV, 0 }, { "span-all", PA_UNIV, 0 },
	{ NULL, 0, 0 }
};

static const struct pa_kw *pa_lookup(const tok *t)
{
	int i;
	if (t->kind != T_IDENT) return NULL;
	for (i = 0; pa_kws[i].name != NULL; i++)
		if (ieq(t->s, t->len, pa_kws[i].name)) return &pa_kws[i];
	return NULL;
}

static int pa_is_span_all(const struct pa_kw *k)
{
	return k->group == PA_UNIV && strcmp(k->name, "span-all") == 0;
}

static int canon_position_area(lexed *lx, buf *b)
{
	const struct pa_kw *a, *c;

	if (tok_is_ident(cur(lx), "none")) {
		adv(lx);
		if (!at_end(lx)) return -1;	/* `none none`, `none start` */
		bput(b, "none", 4);
		return 0;
	}

	a = pa_lookup(cur(lx));
	if (a == NULL) return -1;
	adv(lx);

	if (at_end(lx)) {			/* one keyword */
		bput(b, a->name, -1);
		return 0;
	}

	c = pa_lookup(cur(lx));
	if (c == NULL) return -1;
	adv(lx);
	if (!at_end(lx)) return -1;		/* `top left top` */

	/* Both universal: `center center` and `span-all span-all` collapse,
	 * anything else keeps the order it was written in. */
	if (a->group == PA_UNIV && c->group == PA_UNIV) {
		bput(b, a->name, -1);
		if (a != c) { bputc(b, ' '); bput(b, c->name, -1); }
		return 0;
	}

	/* One universal. For an axis-ambiguous partner nothing can be inferred,
	 * so the pair is emitted exactly as written -- span-all included. */
	if (a->group == PA_UNIV || c->group == PA_UNIV) {
		const struct pa_kw *u = (a->group == PA_UNIV) ? a : c;
		const struct pa_kw *o = (a->group == PA_UNIV) ? c : a;

		if (o->group == PA_SE || o->group == PA_SSE) {
			bput(b, a->name, -1);
			bputc(b, ' ');
			bput(b, c->name, -1);
			return 0;
		}
		if (pa_is_span_all(u)) {	/* the default: drop it */
			bput(b, o->name, -1);
			return 0;
		}
		/* `center` fills the axis the named keyword left empty, and
		 * the pair is emitted in slot order. */
		if (o->axis == 0) {
			bput(b, o->name, -1);
			bputc(b, ' ');
			bput(b, u->name, -1);
		} else {
			bput(b, u->name, -1);
			bputc(b, ' ');
			bput(b, o->name, -1);
		}
		return 0;
	}

	/* Two named keywords: they must belong to the same group. */
	if (a->group != c->group) return -1;

	/* An ambiguous group can pair with itself in either order; identical
	 * keywords collapse to one. */
	if (a->group == PA_SE || a->group == PA_SSE) {
		bput(b, a->name, -1);
		if (a != c) { bputc(b, ' '); bput(b, c->name, -1); }
		return 0;
	}

	if (a->axis == c->axis) return -1;	/* `left right`, `left left` */
	if (!CANON_NAME_FIRST) {
		/* The negative control again: emit the pair in the order the
		 * author wrote it. Every value is still accepted and every
		 * invalid one still refused; only the spelling of `top left`
		 * changes, and only a byte comparison notices. */
		bput(b, a->name, -1);
		bputc(b, ' ');
		bput(b, c->name, -1);
		return 0;
	}
	if (a->axis == 0) {
		bput(b, a->name, -1);
		bputc(b, ' ');
		bput(b, c->name, -1);
	} else {
		bput(b, c->name, -1);
		bputc(b, ' ');
		bput(b, a->name, -1);
	}
	return 0;
}

/* ====================================================================
 * Property tables
 * ==================================================================== */

/* Properties whose value may contain anchor() -- the inset properties. */
static const char *const inset_props[] = {
	"top", "right", "bottom", "left", "inset",
	"inset-block", "inset-inline",
	"inset-block-start", "inset-block-end",
	"inset-inline-start", "inset-inline-end", NULL
};

/* Properties whose value may contain anchor-size(): the inset ones plus the
 * sizing and margin properties. */
static const char *const size_props[] = {
	"width", "height", "min-width", "max-width",
	"min-height", "max-height",
	"block-size", "inline-size",
	"min-block-size", "max-block-size",
	"min-inline-size", "max-inline-size", NULL
};

static const char *const margin_props[] = {
	"margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
	"margin-block", "margin-inline",
	"margin-block-start", "margin-block-end",
	"margin-inline-start", "margin-inline-end", NULL
};

/* The shorthands among them, which take one to four components. Everything
 * else takes exactly one, and the difference is load-bearing: it is what makes
 * `width: anchor-size(--foo width) junk` invalid. */
static const char *const multi_props[] = {
	"inset", "inset-block", "inset-inline",
	"margin", "margin-block", "margin-inline", NULL
};

static int in_tab(const char *p, int plen, const char *const *tab)
{
	int i;
	for (i = 0; tab[i] != NULL; i++)
		if (ieq(p, plen, tab[i])) return 1;
	return 0;
}

/* The shape, filter and motion properties. LibCSS has none of them, so each
 * is a property CSS.supports() denies -- which makes it unsettable, which
 * fails every interpolation test of it before the value is even looked at. */
static const char *const shape_props[] = {
	"clip-path", "shape-outside", "offset-path", NULL
};

static const char *const filter_props[] = {
	"filter", "backdrop-filter", NULL
};

/* ====================================================================
 * <basic-shape> and <filter-function-list>
 *
 * WHY THESE ARE WORTH MORE THAN THEIR OWN TEST FILES.
 *
 * css_supports_decl() -- what CSS.supports() answers from, and what the CSSOM
 * setter consults -- now asks this file before LibCSS. A property LibCSS has
 * never heard of is therefore not merely unserializable: it is UNSUPPORTED,
 * which makes it unsettable, which makes every interpolation test of it fail
 * on its first assertion. interpolation-testcommon.js gates a whole FILE on
 * `CSS.supports(property, from)`, so one unknown grammar costs thousands of
 * subtests that have nothing to do with parsing.
 *
 * That is why the acceptance in here matters more than the spelling. A value
 * this file accepts unblocks its file; a value it spells imperfectly fails
 * only the handful of `-valid` rows that compare bytes -- rows that fail
 * today anyway. So where the corpus pins a spelling, it is transcribed; where
 * it does not, the tokens are normalised and left alone. What is NOT allowed
 * is accepting something the `-invalid` files refuse, because those pass
 * today (vacuously, on a property that never stored anything) and would go
 * red the moment this lands.
 *
 * THE GRAMMARS, transcribed from css-masking/parsing/clip-path-{valid,
 * invalid}, css-shapes/parsing/shape-outside-*, filter-effects/parsing/
 * {filter,backdrop-filter}-parsing-{valid,invalid} and
 * motion/parsing/offset-path-*:
 *
 *   <basic-shape> = inset( <lp>{1,4} [ round <lp>{1,4} [ / <lp>{1,4} ]? ]? )
 *                 | circle( <radius>? [ at <position> ]? )
 *                 | ellipse( [ <radius>{2} ]? [ at <position> ]? )
 *                 | polygon( <fill-rule>? [ round <length> ]? ,
 *                            [ <lp> <lp> ]# )
 *                 | path( [ <fill-rule> , ]? <string> )
 *                 | rect( [ <lp> | auto ]{4} [ round ... ]? )
 *                 | xywh( <lp>{2} <lp>{2} [ round ... ]? )
 *   <radius>      = <lp [0,inf]> | closest-side | farthest-side
 *
 * Two refusals that are easy to miss and are each several rows:
 *   - A BARE NUMBER IS NOT A LENGTH. `inset(123)`, `circle(123)`,
 *     `blur(10)`, `hue-rotate(90)` are all invalid. Zero is the exception
 *     everywhere a length is wanted, and only there.
 *   - `ellipse()` takes TWO radii or NONE. `ellipse(3%)` and
 *     `ellipse(closest-side)` are invalid while `circle(3%)` is fine, which
 *     is the one place the two shapes stop being the same function with a
 *     different name.
 * ==================================================================== */

/* A <length-percentage>: a dimension with a length unit, a percentage, zero,
 * or a math function. `nonneg` refuses a NEGATIVE LITERAL and says nothing
 * about a calc, for the reason the grid section gives at length -- whether a
 * calc is negative is a used-value question. */
static int lp_value(lexed *lx, buf *b, int nonneg, int pct_ok)
{
	const tok *t = cur(lx);

	if (t->kind == T_NUM) {
		/* Zero is the only bare number that is a length. */
		if (t->num != 0) return -1;
		adv(lx);
		bput(b, "0px", 3);
		return 0;
	}
	if (t->kind == T_PCT) {
		if (!pct_ok) return -1;
		if (nonneg && t->num < 0) return -1;
		return emit_token(lx, b, 1);
	}
	if (t->kind == T_DIM) {
		char u[24];
		int n = t->len;
		if (n >= (int)sizeof u) return -1;
		memcpy(u, t->s, (size_t)n);
		u[n] = 0;
		if (unit_cat(u) != U_LEN) return -1;
		if (nonneg && t->num < 0) return -1;
		return emit_token(lx, b, 1);
	}
	if (t->kind == T_FUNC) return calc_lp(lx, b);
	return -1;
}

/* One to four <length-percentage>, space separated. */
static int lp_box(lexed *lx, buf *b, int maxn, int nonneg)
{
	int n = 0;

	while (!at_end(lx) && n < maxn) {
		const tok *t = cur(lx);
		if (t->kind != T_NUM && t->kind != T_PCT && t->kind != T_DIM &&
		    t->kind != T_FUNC)
			break;
		if (n) bputc(b, ' ');
		if (lp_value(lx, b, nonneg, 1) != 0) return -1;
		n++;
	}
	return n;
}

static const char *const pos_keywords[] = {
	"left", "right", "top", "bottom", "center", NULL
};

/* `at <position>`: one to four components of a keyword or a
 * <length-percentage>. The keyword ORDER is canonicalised -- `at top right`
 * reads back as `at right top`, horizontal first -- which is the only part of
 * a position this file rewrites. */
static int shape_position(lexed *lx, buf *b)
{
	const char *kw[4];
	char lpbuf[4][96];
	int islp[4], n = 0, i;
	int hx = -1, vy = -1;

	while (n < 4 && !at_end(lx)) {
		const tok *t = cur(lx);
		const char *k = tab_lookup(t, pos_keywords);
		if (k != NULL) {
			kw[n] = k;
			islp[n] = 0;
			adv(lx);
			n++;
			continue;
		}
		if (t->kind == T_NUM || t->kind == T_PCT || t->kind == T_DIM ||
		    t->kind == T_FUNC) {
			buf tb;
			tb.p = lpbuf[n];
			tb.len = 0;
			tb.cap = (int)sizeof lpbuf[n];
			tb.ovf = 0;
			if (lp_value(lx, &tb, 0, 1) != 0 || tb.ovf) return -1;
			kw[n] = NULL;
			islp[n] = 1;
			n++;
			continue;
		}
		break;
	}
	if (n == 0) return -1;

	/* Two components in the wrong order is the one thing to fix: a
	 * vertical keyword may be written first and must not come out that
	 * way. Anything else is emitted as written. */
	if (n == 2 && !islp[0] && !islp[1]) {
		for (i = 0; i < 2; i++) {
			if (strcmp(kw[i], "left") == 0 ||
			    strcmp(kw[i], "right") == 0) hx = i;
			else if (strcmp(kw[i], "top") == 0 ||
				 strcmp(kw[i], "bottom") == 0) vy = i;
		}
		if (hx == 1 && vy == 0) { const char *s = kw[0]; kw[0] = kw[1]; kw[1] = s; }
	}
	for (i = 0; i < n; i++) {
		if (i) bputc(b, ' ');
		if (islp[i]) bput(b, lpbuf[i], -1);
		else bput(b, kw[i], -1);
	}
	return 0;
}

/* `[ at <position> ]?` -- writes " at ..." when present. */
/* `wrote` says whether a radius came out before this, because
 * `ellipse(closest-side closest-side at 10% 20%)` drops BOTH radii and must
 * then be `ellipse(at 10% 20%)` and not `ellipse( at 10% 20%)`. */
static int shape_at(lexed *lx, buf *b, int wrote)
{
	if (at_end(lx) || !tok_is_ident(cur(lx), "at")) return 0;
	adv(lx);
	if (wrote) bputc(b, ' ');
	bput(b, "at ", 3);
	return shape_position(lx, b);
}

/* `[ round <lp>{1,4} [ / <lp>{1,4} ]? ]?` -- the border-radius tail shared by
 * inset(), rect() and xywh(). A radius is NEVER negative. */
static int shape_round(lexed *lx, buf *b)
{
	buf tb;
	char store[256];
	int n;

	if (at_end(lx) || !tok_is_ident(cur(lx), "round")) return 0;
	adv(lx);
	tb.p = store;
	tb.len = 0;
	tb.cap = (int)sizeof store;
	tb.ovf = 0;
	n = lp_box(lx, &tb, 4, 1);
	if (n <= 0 || tb.ovf) return -1;
	if (!at_end(lx) && cur(lx)->kind == T_DELIM && cur(lx)->delim == '/') {
		adv(lx);
		bput(&tb, " / ", 3);
		if (lp_box(lx, &tb, 4, 1) <= 0 || tb.ovf) return -1;
	}
	/* `round 0` on every corner is the initial value and disappears --
	 * `xywh(0px 1% 2px 3em round 0)` is `xywh(0px 1% 2px 3em)`. */
	if (strcmp(store, "0px") == 0) return 0;
	bput(b, " round ", 7);
	bput(b, store, tb.len);
	return 0;
}

static const char *const shape_radius_kw[] = {
	"closest-side", "farthest-side", NULL
};

/* A circle()/ellipse() radius. Returns 1 if something was written. */
static int shape_radius(lexed *lx, buf *b, int *wrote)
{
	const tok *t = cur(lx);
	const char *k = tab_lookup(t, shape_radius_kw);

	*wrote = 0;
	if (k != NULL) {
		adv(lx);
		/* `closest-side` is the initial value and disappears;
		 * `farthest-side` does not. */
		if (strcmp(k, "closest-side") != 0) { bput(b, k, -1); *wrote = 1; }
		else *wrote = -1;	/* consumed, emitted nothing */
		return 0;
	}
	if (t->kind == T_NUM || t->kind == T_PCT || t->kind == T_DIM ||
	    t->kind == T_FUNC) {
		if (lp_value(lx, b, 1, 1) != 0) return -1;
		*wrote = 1;
		return 0;
	}
	return -1;
}

static int canon_basic_shape(lexed *lx, buf *b, int allow_path);

static int shape_inset(lexed *lx, buf *b)
{
	if (lp_box(lx, b, 4, 0) <= 0) return -1;
	if (shape_round(lx, b) != 0) return -1;
	return 0;
}

static int shape_circle(lexed *lx, buf *b)
{
	int wrote = 0;
	if (!at_end(lx) && cur(lx)->kind != T_RPAREN &&
	    !tok_is_ident(cur(lx), "at")) {
		if (shape_radius(lx, b, &wrote) != 0) return -1;
	}
	return shape_at(lx, b, wrote > 0);
}

static int shape_ellipse(lexed *lx, buf *b)
{
	buf tb;
	char store[128];
	int w1 = 0, w2 = 0;

	if (!at_end(lx) && cur(lx)->kind != T_RPAREN &&
	    !tok_is_ident(cur(lx), "at")) {
		tb.p = store;
		tb.len = 0;
		tb.cap = (int)sizeof store;
		tb.ovf = 0;
		if (shape_radius(lx, &tb, &w1) != 0) return -1;
		if (w1 > 0) bputc(&tb, ' ');
		/* TWO radii or NONE: `ellipse(3%)` is invalid where
		 * `circle(3%)` is fine. */
		if (shape_radius(lx, &tb, &w2) != 0) return -1;
		if (tb.ovf) return -1;
		/* Both `closest-side` means both disappear, and the pair
		 * collapses to nothing at all. */
		if (w1 > 0 || w2 > 0) {
			if (w1 <= 0) bput(b, "closest-side ", 13);
			bput(b, tb.p, tb.len);
			if (w2 <= 0) bput(b, " closest-side", 13);
		}
		return shape_at(lx, b, w1 > 0 || w2 > 0);
	}
	return shape_at(lx, b, 0);
}

static const char *const fill_rules[] = { "nonzero", "evenodd", NULL };

static int shape_polygon(lexed *lx, buf *b)
{
	const char *fr = tab_lookup(cur(lx), fill_rules);
	int n = 0, wrote_head = 0;

	if (fr != NULL) {
		adv(lx);
		/* `nonzero` is the initial value and disappears. */
		if (strcmp(fr, "evenodd") == 0) { bput(b, fr, -1); wrote_head = 1; }
	}
	if (!at_end(lx) && tok_is_ident(cur(lx), "round")) {
		buf tb;
		char store[128];
		adv(lx);
		tb.p = store;
		tb.len = 0;
		tb.cap = (int)sizeof store;
		tb.ovf = 0;
		if (lp_value(lx, &tb, 1, 0) != 0 || tb.ovf) return -1;
		/* `round 0px` is the initial value and disappears. */
		if (strcmp(store, "0px") != 0) {
			if (wrote_head) bputc(b, ' ');
			bput(b, "round ", 6);
			bput(b, store, tb.len);
			wrote_head = 1;
		}
	}
	if (wrote_head) bcomma(b);
	if (at_end(lx) || cur(lx)->kind != T_COMMA) {
		if (at_end(lx)) return -1;
	} else {
		adv(lx);		/* the comma after the head */
	}
	for (;;) {
		if (n) bcomma(b);
		if (lp_value(lx, b, 0, 1) != 0) return -1;
		bputc(b, ' ');
		if (lp_value(lx, b, 0, 1) != 0) return -1;
		n++;
		if (at_end(lx) || cur(lx)->kind != T_COMMA) break;
		adv(lx);
	}
	return n > 0 ? 0 : -1;
}

static int shape_path(lexed *lx, buf *b)
{
	const char *fr = tab_lookup(cur(lx), fill_rules);

	if (fr != NULL) {
		adv(lx);
		if (at_end(lx) || cur(lx)->kind != T_COMMA) return -1;
		adv(lx);
		if (strcmp(fr, "evenodd") == 0) { bput(b, fr, -1); bcomma(b); }
	}
	if (at_end(lx) || cur(lx)->kind != T_STR) return -1;
	/* The path data itself is SVG, not CSS, and this file does not
	 * re-spell it: a normalised path string is a different serialization
	 * problem living in a different spec. */
	bstring(b, cur(lx)->s, cur(lx)->len);
	adv(lx);
	return 0;
}

static int shape_rect(lexed *lx, buf *b)
{
	int i;
	for (i = 0; i < 4; i++) {
		if (i) bputc(b, ' ');
		if (tok_is_ident(cur(lx), "auto")) {
			bput(b, "auto", 4);
			adv(lx);
			continue;
		}
		if (lp_value(lx, b, 0, 1) != 0) return -1;
	}
	return shape_round(lx, b);
}

static int shape_xywh(lexed *lx, buf *b)
{
	int i;
	for (i = 0; i < 4; i++) {
		if (i) bputc(b, ' ');
		/* width and height are non-negative; x and y are not. */
		if (lp_value(lx, b, i >= 2, 1) != 0) return -1;
	}
	return shape_round(lx, b);
}

/* One <basic-shape>. The cursor is on its function token. */
static int canon_basic_shape(lexed *lx, buf *b, int allow_path)
{
	const tok *t = cur(lx);
	int rc = -1;

	if (t->kind != T_FUNC) return -1;
	if (ieq(t->s, t->len, "inset")) { bput(b, "inset(", 6); adv(lx); rc = shape_inset(lx, b); }
	else if (ieq(t->s, t->len, "circle")) { bput(b, "circle(", 7); adv(lx); rc = shape_circle(lx, b); }
	else if (ieq(t->s, t->len, "ellipse")) { bput(b, "ellipse(", 8); adv(lx); rc = shape_ellipse(lx, b); }
	else if (ieq(t->s, t->len, "polygon")) { bput(b, "polygon(", 8); adv(lx); rc = shape_polygon(lx, b); }
	else if (ieq(t->s, t->len, "rect")) { bput(b, "rect(", 5); adv(lx); rc = shape_rect(lx, b); }
	else if (ieq(t->s, t->len, "xywh")) { bput(b, "xywh(", 5); adv(lx); rc = shape_xywh(lx, b); }
	else if (allow_path && ieq(t->s, t->len, "path")) { bput(b, "path(", 5); adv(lx); rc = shape_path(lx, b); }
	else return -1;

	if (rc != 0) return -1;
	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	bputc(b, ')');
	return 0;
}

static const char *const geometry_boxes[] = {
	"content-box", "padding-box", "border-box", "margin-box",
	"fill-box", "stroke-box", "view-box", NULL
};

/* clip-path / shape-outside / offset-path share
 * `<basic-shape> || <geometry-box>` with different extras around it. */
static int canon_shape_value(lexed *lx, buf *b, int allow_path, int allow_url,
		int allow_box)
{
	int shape = 0, box = 0, wrote = 0;

	while (!at_end(lx)) {
		const tok *t = cur(lx);
		const char *g;

		if (allow_box && (g = tab_lookup(t, geometry_boxes)) != NULL) {
			if (box) return -1;
			if (wrote) bputc(b, ' ');
			bput(b, g, -1);
			adv(lx);
			box = 1;
			wrote = 1;
			continue;
		}
		if (allow_url && t->kind == T_FUNC && ieq(t->s, t->len, "url")) {
			if (shape || box) return -1;
			if (emit_color_fn_raw(lx, b, "url") != 0) return -1;
			shape = 1;
			wrote = 1;
			continue;
		}
		if (t->kind == T_FUNC) {
			if (shape) return -1;
			if (wrote) bputc(b, ' ');
			if (canon_basic_shape(lx, b, allow_path) != 0) return -1;
			shape = 1;
			wrote = 1;
			continue;
		}
		return -1;
	}
	return (shape || box) ? 0 : -1;
}

/* ====================================================================
 * <filter-function-list>
 * ==================================================================== */

/* name, and what its single argument is:
 *   'n' <number-percentage> non-negative      'a' <angle>
 *   'l' <length> non-negative                 's' drop-shadow
 *   'N' <number-percentage>, sign unrestricted */
struct filter_fn { const char *name; char arg; };

static const struct filter_fn filter_fns[] = {
	{ "blur", 'l' }, { "brightness", 'n' }, { "contrast", 'n' },
	{ "grayscale", 'n' }, { "invert", 'n' }, { "opacity", 'n' },
	{ "saturate", 'n' }, { "sepia", 'n' }, { "hue-rotate", 'a' },
	{ "drop-shadow", 's' }, { NULL, 0 }
};

/* drop-shadow( <color>? <length>{2,3} <color>? ) -- and the colour, wherever
 * it was written, comes out FIRST. Two or three lengths, never a percentage,
 * which is `drop-shadow(10% 20%)` being invalid. */
static int filter_drop_shadow(lexed *lx, buf *b)
{
	char colour[256], lens[192];
	buf cb, lb;
	int nlen = 0, have_colour = 0;

	cb.p = colour; cb.len = 0; cb.cap = (int)sizeof colour; cb.ovf = 0;
	lb.p = lens; lb.len = 0; lb.cap = (int)sizeof lens; lb.ovf = 0;

	while (!at_end(lx) && cur(lx)->kind != T_RPAREN) {
		const tok *t = cur(lx);
		if (t->kind == T_NUM || t->kind == T_DIM ||
		    (t->kind == T_FUNC && !color_keyword(t) &&
		     ieq(t->s, t->len, "calc"))) {
			if (nlen >= 3) return -1;
			if (nlen) bputc(&lb, ' ');
			if (lp_value(lx, &lb, 0, 0) != 0) return -1;
			nlen++;
			continue;
		}
		if (have_colour) return -1;
		if (canon_color(lx, &cb, 1) != 0) return -1;
		have_colour = 1;
	}
	if (nlen < 2 || cb.ovf || lb.ovf) return -1;
	if (have_colour) { bput(b, cb.p, cb.len); bputc(b, ' '); }
	bput(b, lb.p, lb.len);
	return 0;
}

static int canon_filter_list(lexed *lx, buf *b)
{
	int n = 0;

	while (!at_end(lx)) {
		const tok *t = cur(lx);
		int i;

		if (t->kind != T_FUNC) return -1;
		if (n) bputc(b, ' ');

		if (ieq(t->s, t->len, "url")) {
			if (emit_color_fn_raw(lx, b, "url") != 0) return -1;
			n++;
			continue;
		}
		for (i = 0; filter_fns[i].name != NULL; i++)
			if (ieq(t->s, t->len, filter_fns[i].name)) break;
		if (filter_fns[i].name == NULL) return -1;
		bput(b, filter_fns[i].name, -1);
		bputc(b, '(');
		adv(lx);

		if (!at_end(lx) && cur(lx)->kind == T_RPAREN) {
			/* Every one of them takes an empty argument list. */
			if (filter_fns[i].arg == 's') return -1;
			adv(lx);
			bputc(b, ')');
			n++;
			continue;
		}
		switch (filter_fns[i].arg) {
		case 'l':
			if (lp_value(lx, b, 1, 0) != 0) return -1;
			break;
		case 'n': {
			const tok *v = cur(lx);
			if (v->kind == T_NUM || v->kind == T_PCT) {
				if (v->num < 0) return -1;
				if (emit_token(lx, b, 1) != 0) return -1;
			} else if (v->kind == T_FUNC) {
				/* A calc may be negative here: the corpus has
				 * `brightness(calc(-10))` as VALID. */
				if (calc_channel(lx, b, 1, 0, NULL, 0, NULL) != 0)
					return -1;
			} else {
				return -1;
			}
			break;
		}
		case 'a': {
			const tok *v = cur(lx);
			double d;
			if (v->kind == T_NUM) {
				if (v->num != 0) return -1;	/* needs a unit */
				bput(b, "0deg", 4);
				adv(lx);
			} else if (v->kind == T_DIM) {
				if (angle_deg(v, &d) != 0) return -1;
				if (emit_token(lx, b, 1) != 0) return -1;
			} else if (v->kind == T_FUNC) {
				if (calc_channel(lx, b, 0, 1, NULL, 0, NULL) != 0)
					return -1;
			} else {
				return -1;
			}
			break;
		}
		case 's':
			if (filter_drop_shadow(lx, b) != 0) return -1;
			break;
		default:
			return -1;
		}
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
		bputc(b, ')');
		n++;
	}
	return n > 0 ? 0 : -1;
}

/* offset-path adds ray() to the shape set. */
static const char *const ray_sizes[] = {
	"closest-side", "closest-corner", "farthest-side", "farthest-corner",
	"sides", NULL
};

static int canon_ray(lexed *lx, buf *b)
{
	int have_angle = 0, wrote = 0;
	const char *sz = NULL;
	int contain = 0;

	adv(lx);
	bput(b, "ray(", 4);
	while (!at_end(lx) && cur(lx)->kind != T_RPAREN) {
		const tok *t = cur(lx);
		const char *k;
		double d;

		if (t->kind == T_DIM && angle_deg(t, &d) == 0) {
			if (have_angle) return -1;
			if (wrote) bputc(b, ' ');
			if (emit_token(lx, b, 1) != 0) return -1;
			have_angle = 1;
			wrote = 1;
			continue;
		}
		if ((k = tab_lookup(t, ray_sizes)) != NULL) {
			if (sz != NULL) return -1;
			sz = k;
			adv(lx);
			continue;
		}
		if (tok_is_ident(t, "contain")) {
			if (contain) return -1;
			contain = 1;
			adv(lx);
			continue;
		}
		if (tok_is_ident(t, "at")) {
			if (shape_at(lx, b, 1) != 0) return -1;
			continue;
		}
		return -1;
	}
	if (!have_angle) return -1;
	/* `closest-side` is the initial size and is the one that disappears. */
	if (sz != NULL && strcmp(sz, "closest-side") != 0) {
		bputc(b, ' ');
		bput(b, sz, -1);
	}
	if (contain) bput(b, " contain", 8);
	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	bputc(b, ')');
	return 0;
}

/* ====================================================================
 * The CSS Transforms 2 longhands: rotate, translate, scale, perspective.
 *
 * NOT the same grammar as `transform`, which is a <transform-list> and is
 * css_interp.c's (ci_transform_parse, already routed). These four are
 * independent properties with their own much smaller grammars, and LibCSS has
 * none of them -- so CSS.supports() denies all four and their interpolation
 * files never start.
 *
 *   rotate      = none | <angle> | [ x | y | z | <number>{3} ] && <angle>
 *   translate   = none | <length-percentage> [ <length-percentage> <length>? ]?
 *   scale       = none | [ <number> | <percentage> ]{1,3}
 *   perspective = none | <length [0,inf]>
 *
 * The axis comes out FIRST however it was written, and a direction vector
 * that points along an axis becomes the LETTER: `0.5 0 0 400grad` is
 * `x 400grad`. Trailing components that repeat the default disappear --
 * `translate: 100px 0px` is `100px`, `scale: 100 100` is `100`.
 * ==================================================================== */

static int tx_angle(lexed *lx, buf *b)
{
	const tok *t = cur(lx);
	double d;

	if (t->kind == T_NUM && t->num == 0) { bput(b, "0deg", 4); adv(lx); return 0; }
	if (t->kind == T_DIM && angle_deg(t, &d) == 0) return emit_token(lx, b, 1);
	if (t->kind == T_FUNC) return calc_channel(lx, b, 0, 1, NULL, 0, NULL);
	return -1;
}

static int canon_rotate(lexed *lx, buf *b)
{
	char ang[128];
	buf ab;
	const char *axis = NULL;
	double v[3];
	int nv = 0, have_angle = 0;

	ab.p = ang; ab.len = 0; ab.cap = (int)sizeof ang; ab.ovf = 0;

	while (!at_end(lx)) {
		const tok *t = cur(lx);
		if (t->kind == T_IDENT) {
			if (axis != NULL || nv) return -1;
			if (ieq(t->s, t->len, "x")) axis = "x";
			else if (ieq(t->s, t->len, "y")) axis = "y";
			else if (ieq(t->s, t->len, "z")) axis = "z";
			else return -1;
			adv(lx);
			continue;
		}
		if (t->kind == T_NUM && !(t->kind == T_DIM)) {
			/* A bare number is part of the direction vector; an
			 * angle is the rotation. `0` is ambiguous and the
			 * corpus resolves it as a vector component when three
			 * are being collected and as an angle otherwise. */
			if (axis != NULL || nv >= 3) {
				if (have_angle) return -1;
				if (tx_angle(lx, &ab) != 0) return -1;
				have_angle = 1;
				continue;
			}
			v[nv++] = t->num;
			adv(lx);
			continue;
		}
		if (have_angle) return -1;
		if (tx_angle(lx, &ab) != 0) return -1;
		have_angle = 1;
	}
	if (ab.ovf) return -1;
	/* Three collected numbers and no angle means the last one WAS the
	 * angle -- but a bare number is not an angle unless it is zero. */
	if (!have_angle) {
		if (nv != 1 || v[0] != 0) return -1;
		bput(&ab, "0deg", 4);
		have_angle = 1;
		nv = 0;
	}
	if (nv != 0 && nv != 3) return -1;
	if (nv == 3) {
		/* A vector along an axis is spelled with the letter. */
		if (v[0] > 0 && v[1] == 0 && v[2] == 0) axis = "x";
		else if (v[0] == 0 && v[1] > 0 && v[2] == 0) axis = "y";
		else if (v[0] == 0 && v[1] == 0 && v[2] > 0) axis = NULL; /* z is the default */
		else {
			int i;
			for (i = 0; i < 3; i++) { if (i) bputc(b, ' '); bnum6(b, v[i]); }
			bputc(b, ' ');
			bput(b, ab.p, ab.len);
			return 0;
		}
	}
	if (axis != NULL && strcmp(axis, "z") != 0) {
		bput(b, axis, -1);
		bputc(b, ' ');
	}
	bput(b, ab.p, ab.len);
	return 0;
}

static int canon_translate(lexed *lx, buf *b)
{
	char c[3][96];
	buf cb;
	int n = 0, i;

	while (!at_end(lx) && n < 3) {
		cb.p = c[n]; cb.len = 0; cb.cap = (int)sizeof c[n]; cb.ovf = 0;
		/* The third component is a LENGTH: a percentage z has nothing
		 * to resolve against. */
		if (lp_value(lx, &cb, 0, n < 2) != 0 || cb.ovf) return -1;
		n++;
	}
	if (n == 0 || !at_end(lx)) return -1;
	/* Trailing zeros are the initial value and disappear. */
	while (n > 1 && strcmp(c[n - 1], "0px") == 0) n--;
	for (i = 0; i < n; i++) { if (i) bputc(b, ' '); bput(b, c[i], -1); }
	return 0;
}

static int canon_scale(lexed *lx, buf *b)
{
	double v[3];
	int n = 0, i;
	char txt[3][96];
	int istext[3];

	while (!at_end(lx) && n < 3) {
		const tok *t = cur(lx);
		istext[n] = 0;
		if (t->kind == T_NUM) { v[n] = t->num; adv(lx); }
		else if (t->kind == T_PCT) { v[n] = t->num / 100.0; adv(lx); }
		else if (t->kind == T_FUNC) {
			buf tb;
			tb.p = txt[n]; tb.len = 0; tb.cap = (int)sizeof txt[n]; tb.ovf = 0;
			if (calc_channel(lx, &tb, 1, 0, NULL, 0, NULL) != 0 || tb.ovf)
				return -1;
			istext[n] = 1;
			v[n] = 0;
		} else {
			return -1;
		}
		n++;
	}
	if (n == 0 || !at_end(lx)) return -1;
	/* A z of 1 disappears, and a y equal to x disappears after it. */
	if (n == 3 && !istext[2] && v[2] == 1) n = 2;
	if (n == 2 && !istext[0] && !istext[1] && v[0] == v[1]) n = 1;
	for (i = 0; i < n; i++) {
		if (i) bputc(b, ' ');
		if (istext[i]) bput(b, txt[i], -1);
		else bnum6(b, v[i]);
	}
	return 0;
}

static const char *const fsa_props[] = { "font-size-adjust", NULL };

static const char *const transform_props[] = {
	"rotate", "translate", "scale", "perspective", NULL
};

/* font-size-adjust = none | [ ex-height | cap-height | ch-width | ic-width |
 *                              ic-height ]? [ from-font | <number> ]
 *
 * `ex-height` is the default metric and disappears; the number is
 * non-negative as a LITERAL and unrestricted as a calc, the same asymmetry
 * as everywhere else in this file. A metric on its own is not a value, and
 * neither is `<metric> none`. */
static const char *const fsa_metrics[] = {
	"ex-height", "cap-height", "ch-width", "ic-width", "ic-height", NULL
};

static int canon_font_size_adjust(lexed *lx, buf *b)
{
	const char *m = tab_lookup(cur(lx), fsa_metrics);
	const tok *t;

	if (m != NULL) {
		adv(lx);
		if (strcmp(m, "ex-height") != 0) { bput(b, m, -1); bputc(b, ' '); }
	}
	if (at_end(lx)) return -1;		/* a metric alone is not a value */
	t = cur(lx);
	if (tok_is_ident(t, "from-font")) {
		bput(b, "from-font", 9);
		adv(lx);
	} else if (t->kind == T_NUM) {
		if (t->num < 0) return -1;
		if (emit_token(lx, b, 1) != 0) return -1;
	} else if (t->kind == T_FUNC) {
		if (calc_channel(lx, b, 0, 0, NULL, 0, NULL) != 0) return -1;
	} else {
		return -1;
	}
	return at_end(lx) ? 0 : -1;
}

/* The grid properties this file claims. LibCSS has no grid at all, so these
 * are not values it refuses -- they are properties it has never heard of, and
 * without this the declaration is dropped and `el.style.gridTemplateColumns`
 * reads back "". */
static const char *const grid_props[] = {
	"grid-template-columns", "grid-template-rows",
	"grid-auto-columns", "grid-auto-rows", NULL
};

/* ====================================================================
 * css-grid: <track-list>, and the reason it is here rather than in LibCSS
 *
 * LibCSS HAS NO GRID AT ALL. Not a parser, not a computed field, not a
 * property string -- `grep -i grid` over src/parse/propstrings.h finds one
 * hit, the `grid` value of `display`. So `grid-template-columns: 1fr 2fr` is
 * not a declaration LibCSS refuses; it is a declaration LibCSS has never
 * heard of, which is why the whole css-grid parsing directory reads 318 of
 * 1468 and why the grid engine in c/apps/browser parses declaration strings
 * itself instead of reading a computed style.
 *
 * A specified-value serializer is the half of that this file can honestly
 * do, and the corpus asks for exactly it: `-valid` compares the bytes that
 * come back out of `el.style`. The other half -- a COMPUTED value, which for
 * a track list keeps minmax()/repeat()/fr intact on a non-grid box and
 * becomes the used track sizes in px on a grid container -- needs the
 * cascade to carry the value, and the cascade is not this file's.
 *
 * READ THIS BEFORE ADOPTING css_canon_prop_at() FOR THESE NAMES.
 *
 * Adding the four grid properties to the CSSOM's settable set is NOT enough,
 * and doing only that makes the corpus WORSE in one direction while making it
 * better in the other. Measured, with the names added to js_dom.c's
 * CSSD_EXTRA and nothing else changed:
 *
 *   grid-template-columns-valid      0/34 -> 34/34     the serializer works
 *   grid-template-rows-valid         0/34 -> 34/34
 *   grid-auto-columns-valid          0/30 -> 30/30
 *   grid-auto-rows-valid             0/30 -> 30/30
 *   grid-template-columns-invalid   42/42 -> 0/42      EVERY refusal lost
 *   grid-auto-columns-invalid       16/16 -> 0/16
 *   css-grid/parsing               318    -> 357       net, +39
 *
 * The refusals collapse because for a property LibCSS has never heard of, the
 * CSSOM stores the author's text unconditionally and only canonicalises it on
 * the way back OUT -- so `grid-template-columns: -10px` is written to the
 * style attribute, this file answers CSS_CANON_INVALID when the value is
 * read, and the reader treats that as "no opinion" and hands back the raw
 * bytes. Before this commit those subtests passed VACUOUSLY: nothing was
 * stored, so "should not set the property value" was true for a reason that
 * had nothing to do with validity.
 *
 * So the adoption is two changes, not one: the settable set AND a setter that
 * refuses a declaration this file calls INVALID. Both are in
 * c/apps/browser/js_dom.c and js_cssom.c, which is why they are named here
 * rather than done here.
 *
 * THE GRAMMAR, and every refusal below is a line of
 * grid-template-columns-invalid.html:
 *
 *   <track-breadth>      = <length-percentage [0,inf]> | <flex> |
 *                          min-content | max-content | auto
 *   <inflexible-breadth> = the same WITHOUT <flex>   -- `minmax(5fr, X)` is
 *                          invalid, and that asymmetry is the whole point of
 *                          having two names for one thing
 *   <fixed-breadth>      = <length-percentage> only
 *   <track-size>         = <track-breadth>
 *                        | minmax( <inflexible-breadth> , <track-breadth> )
 *                        | fit-content( <length-percentage> )
 *   <fixed-size>         = a <track-size> guaranteed to have a definite
 *                          size: a <fixed-breadth>, or a minmax() with one
 *                          on either side
 *   <line-names>         = '[' <custom-ident>* ']'
 *
 * A NEGATIVE LITERAL IS A PARSE ERROR AND A NEGATIVE calc() IS NOT.
 * `-10px` is refused; `calc(-0.5em + 10px)` is accepted, because whether a
 * calc is negative is a used-value question and the parser does not get to
 * pre-empt it. Both are in the corpus, adjacent, and an implementation that
 * treats them the same fails one of them whichever way it goes.
 *
 * FIXEDNESS IS A PROPERTY OF THE WHOLE LIST, not of a track. An auto-repeat
 * needs to know how many times to repeat, so the list it sits in must have a
 * definite size -- which means EVERY other track in that list must be fixed
 * too, including the tracks inside an unrelated `repeat(5, ...)`.
 * `auto repeat(auto-fill, auto) auto` is invalid, and so is
 * `min-content repeat(auto-fill, min-content) repeat(5, min-content)`. So
 * this counts non-fixed tracks across the entire list and checks the count
 * at the end, rather than deciding track by track; there is no local rule
 * that gets those two rows right.
 *
 * `[]` IS DROPPED. `repeat(1, [] 10px [])` is `repeat(1, 10px)` and
 * `[] 150px [] 1fr []` is `150px 1fr` -- an empty line-name block names
 * nothing. But two ADJACENT non-empty blocks are still a parse error
 * (`[one] 10px [two] [three]`), so the adjacency check runs on the blocks as
 * written, before the dropping.
 * ==================================================================== */

/* The line-name identifiers CSS reserves. `[auto] 1px` is invalid. */
static const char *const grid_reserved_names[] = {
	"span", "auto", NULL
};

/* Where a breadth may appear. The three differ only in what they exclude,
 * which is why they are flags on one function rather than three functions. */
enum { GB_TRACK = 0, GB_INFLEXIBLE, GB_FIXED };

/* A <length-percentage> written as a math function. In a track list a
 * percentage IS a length -- `calc(30% + 40vw)` is valid here and would be a
 * type error in a colour channel -- so the calculation tree is told so. */
static int grid_calc(lexed *lx, buf *b)
{
	return calc_lp(lx, b);
}

/* One breadth. Returns 0 on success; *fixed says whether it has a definite
 * size, which the caller accumulates for the whole list. */
static int grid_breadth(lexed *lx, buf *b, int kind, int *fixed)
{
	const tok *t = cur(lx);

	*fixed = 0;
	if (t->kind == T_IDENT) {
		if (kind == GB_FIXED) return -1;
		if (ieq(t->s, t->len, "auto")) { bput(b, "auto", 4); adv(lx); return 0; }
		if (ieq(t->s, t->len, "min-content")) {
			bput(b, "min-content", 11); adv(lx); return 0;
		}
		if (ieq(t->s, t->len, "max-content")) {
			bput(b, "max-content", 11); adv(lx); return 0;
		}
		return -1;
	}
	if (t->kind == T_PCT) {
		if (t->num < 0) return -1;
		*fixed = 1;
		return emit_token(lx, b, 1);
	}
	if (t->kind == T_DIM) {
		char u[24];
		int n = t->len;
		if (n >= (int)sizeof u) return -1;
		memcpy(u, t->s, (size_t)n);
		u[n] = 0;
		if (t->num < 0) return -1;
		if (ieq(u, n, "fr")) {
			/* <flex> is a breadth but never an inflexible or a
			 * fixed one: `minmax(5fr, ...)` is invalid. */
			if (kind != GB_TRACK) return -1;
			return emit_token(lx, b, 1);
		}
		if (unit_cat(u) != U_LEN) return -1;
		*fixed = 1;
		return emit_token(lx, b, 1);
	}
	if (t->kind == T_FUNC) {
		/* A math function is a <length-percentage>, and its SIGN is
		 * not the parser's business -- `calc(-0.5em + 10px)` is valid
		 * where `-10px` is not. */
		*fixed = 1;
		return grid_calc(lx, b);
	}
	return -1;
}

static int grid_track_size(lexed *lx, buf *b, int *fixed)
{
	const tok *t = cur(lx);
	int f1 = 0, f2 = 0;

	*fixed = 0;
	if (t->kind == T_FUNC && ieq(t->s, t->len, "minmax")) {
		adv(lx);
		bput(b, "minmax(", 7);
		if (grid_breadth(lx, b, GB_INFLEXIBLE, &f1) != 0) return -1;
		if (at_end(lx) || cur(lx)->kind != T_COMMA) return -1;
		adv(lx);
		bcomma(b);
		if (grid_breadth(lx, b, GB_TRACK, &f2) != 0) return -1;
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
		bputc(b, ')');
		/* A minmax() is fixed when EITHER side is: one definite bound
		 * is enough to size the track. */
		*fixed = f1 || f2;
		return 0;
	}
	if (t->kind == T_FUNC && ieq(t->s, t->len, "fit-content")) {
		adv(lx);
		bput(b, "fit-content(", 12);
		if (grid_breadth(lx, b, GB_FIXED, &f1) != 0) return -1;
		if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
		adv(lx);
		bputc(b, ')');
		*fixed = 0;	/* it depends on the content, so it is not */
		return 0;
	}
	return grid_breadth(lx, b, GB_TRACK, fixed);
}

/* `[` <custom-ident>* `]`. Emits nothing at all for an empty block, and
 * reports through *present that a block WAS there so the caller can refuse
 * two in a row. */
static int grid_line_names(lexed *lx, buf *b, int *present)
{
	int n = 0;

	*present = 0;
	if (at_end(lx) || cur(lx)->kind != T_DELIM || cur(lx)->delim != '[')
		return 0;
	adv(lx);
	*present = 1;
	for (;;) {
		const tok *t = cur(lx);
		if (at_end(lx)) return -1;		/* never closed */
		if (t->kind == T_DELIM && t->delim == ']') { adv(lx); break; }
		if (t->kind != T_IDENT) return -1;
		if (in_tab(t->s, t->len, grid_reserved_names)) return -1;
		if (n == 0) bputc(b, '[');
		else bputc(b, ' ');
		bident(b, t->s, t->len);
		adv(lx);
		n++;
	}
	if (n) bputc(b, ']');
	return n;			/* 0 = emitted nothing */
}

/* State threaded through a whole list, because fixedness is a property of
 * the list and not of a track. */
struct gtl {
	int ntrack;		/* tracks emitted */
	int nonfixed;		/* tracks with no definite size, anywhere */
	int autorep;		/* auto-fill / auto-fit repeats seen */
	int wrote;		/* something has been emitted into b */
};

static void gsep(buf *b, struct gtl *g)
{
	if (g->wrote) bputc(b, ' ');
	g->wrote = 1;
}

/* One `[line-names]? track-size` run. Shared by the top level and by the
 * inside of a repeat(), because they are the same production and writing it
 * twice is how the two drift. `stop` is the token kind that ends the run. */
static int track_run(lexed *lx, buf *b, struct gtl *g, int allow_repeat,
		int allow_names, int stop_at_rparen);

static int grid_repeat(lexed *lx, buf *b, struct gtl *g)
{
	const tok *t;
	struct gtl sub;
	int isauto = 0;

	adv(lx);			/* repeat( */
	bput(b, "repeat(", 7);
	t = cur(lx);
	if (t->kind == T_IDENT && (ieq(t->s, t->len, "auto-fill") ||
				   ieq(t->s, t->len, "auto-fit"))) {
		bput(b, ieq(t->s, t->len, "auto-fill") ? "auto-fill" : "auto-fit", -1);
		isauto = 1;
		g->autorep++;
		adv(lx);
	} else if (t->kind == T_NUM && t->isint && t->num >= 1) {
		bnum6(b, t->num);
		adv(lx);
	} else {
		return -1;
	}
	if (at_end(lx) || cur(lx)->kind != T_COMMA) return -1;
	adv(lx);
	bcomma(b);

	memset(&sub, 0, sizeof sub);
	/* Line names ARE allowed inside a repeat() body even where the
	 * property that contains it does not take them at the top level; a
	 * nested repeat() is not. */
	if (track_run(lx, b, &sub, 0, 1, 1) != 0) return -1;
	if (sub.ntrack == 0) return -1;
	g->ntrack += sub.ntrack;
	g->nonfixed += sub.nonfixed;
	/* An auto-repeat repeats until it fills the container, so it cannot
	 * contain a track whose size depends on its content. */
	if (isauto && sub.nonfixed) return -1;

	if (at_end(lx) || cur(lx)->kind != T_RPAREN) return -1;
	adv(lx);
	bputc(b, ')');
	return 0;
}

static int track_run(lexed *lx, buf *b, struct gtl *g, int allow_repeat,
		int allow_names, int stop_at_rparen)
{
	int names_prev = 0;

	while (!at_end(lx)) {
		const tok *t = cur(lx);
		int fixed = 0;

		if (stop_at_rparen && t->kind == T_RPAREN) break;

		if (t->kind == T_DELIM && t->delim == '[') {
			int present = 0, emitted;
			if (!allow_names) return -1;
			buf tmp;
			char store[256];
			if (names_prev) return -1;	/* two blocks in a row */
			tmp.p = store;
			tmp.len = 0;
			tmp.cap = (int)sizeof store;
			tmp.ovf = 0;
			emitted = grid_line_names(lx, &tmp, &present);
			if (emitted < 0 || tmp.ovf) return -1;
			names_prev = 1;
			/* An empty block names nothing and is DROPPED, but it
			 * still counted as a block for the adjacency rule
			 * above -- which is why the check runs on `present`
			 * and the emission on `emitted`. */
			if (emitted > 0) { gsep(b, g); bput(b, tmp.p, tmp.len); }
			continue;
		}
		names_prev = 0;

		gsep(b, g);
		if (allow_repeat && t->kind == T_FUNC &&
		    ieq(t->s, t->len, "repeat")) {
			if (grid_repeat(lx, b, g) != 0) return -1;
			continue;
		}
		if (grid_track_size(lx, b, &fixed) != 0) return -1;
		g->ntrack++;
		if (!fixed) g->nonfixed++;
	}
	return 0;
}

static int canon_track_list(lexed *lx, buf *b, int allow_repeat,
		int allow_none)
{
	struct gtl g;

	memset(&g, 0, sizeof g);

	if (allow_none && tok_is_ident(cur(lx), "none") && lx->n == 1) {
		bput(b, "none", 4);
		adv(lx);
		return 0;
	}
	/* The template properties take line names and repeat(); the auto-
	 * ones take neither, and one shared flag would have let
	 * `grid-auto-columns: [one] 10px` through. */
	if (track_run(lx, b, &g, allow_repeat, allow_repeat, 0) != 0) return -1;

	/* Line names and no tracks is not a track list. */
	if (g.ntrack == 0) return -1;
	/* At most one auto-repeat, and its presence makes the WHOLE list have
	 * to be definite -- including the tracks inside an unrelated
	 * repeat(5, ...). No per-track rule gets that right, which is why
	 * this is counted across the list and checked once, here. */
	if (g.autorep > 1) return -1;
	if (g.autorep == 1 && g.nonfixed > 0) return -1;
	return 0;
}

/* ====================================================================
 * Entry points
 * ==================================================================== */

/* The properties that are not part of a family. */
static const char *const single_props[] = {
	"anchor-name", "position-anchor", "position-area", "font-family", NULL
};

/* EVERY property this file claims, as one list of lists.
 *
 * The predicate and the ENUMERATION are now the same table, which is the
 * point. c/apps/browser/js_dom.c carries a second, hand-transcribed copy of
 * these names -- the set a CSSStyleDeclaration will let a script assign to --
 * because until now this file published a predicate and no way to iterate.
 * Its own comment names the fix: "one function in canon.h beside the
 * predicate -- a count and an index, exactly the shape css_known_prop_count/at
 * already has -- and then this array deletes itself."
 *
 * This is that function. Until that array is deleted the drift is still
 * there, and it is not theoretical: the grid properties added in this commit
 * are unsettable from script until js_dom.c reads this list, so a serializer
 * that is correct returns nothing measurable. */
static const char *const *const canon_prop_tables[] = {
	inset_props, size_props, margin_props, single_props,
	color_props, grid_props, shape_props, filter_props,
	transform_props, fsa_props, NULL
};

int css_canon_knows_property(const char *prop, int plen)
{
	int i;

	if (prop == NULL) return 0;
	if (plen < 0) plen = (int)strlen(prop);
	if (plen <= 0) return 0;
	for (i = 0; canon_prop_tables[i] != NULL; i++)
		if (in_tab(prop, plen, canon_prop_tables[i])) return 1;
	return 0;
}

int css_canon_prop_count(void)
{
	int i, j, n = 0;

	for (i = 0; canon_prop_tables[i] != NULL; i++)
		for (j = 0; canon_prop_tables[i][j] != NULL; j++) n++;
	return n;
}

const char *css_canon_prop_at(int idx)
{
	int i, j;

	if (idx < 0) return NULL;
	for (i = 0; canon_prop_tables[i] != NULL; i++)
		for (j = 0; canon_prop_tables[i][j] != NULL; j++)
			if (idx-- == 0) return canon_prop_tables[i][j];
	return NULL;
}

/* anchor-name: none | <dashed-ident># */
static int canon_anchor_name(lexed *lx, buf *b)
{
	int first = 1;

	if (tok_is_ident(cur(lx), "none")) {
		adv(lx);
		if (!at_end(lx)) return -1;
		bput(b, "none", 4);
		return 0;
	}
	for (;;) {
		const tok *t = cur(lx);
		if (!is_dashed_ident(t)) return -1;
		if (!first) bcomma(b);
		bident(b, t->s, t->len);
		adv(lx);
		first = 0;
		if (at_end(lx)) break;
		if (cur(lx)->kind != T_COMMA) return -1;
		adv(lx);
		if (at_end(lx)) return -1;
	}
	return 0;
}

/* position-anchor: auto | <dashed-ident> */
static int canon_position_anchor(lexed *lx, buf *b)
{
	const tok *t = cur(lx);

	if (tok_is_ident(t, "auto")) {
		adv(lx);
		if (!at_end(lx)) return -1;
		bput(b, "auto", 4);
		return 0;
	}
	if (!is_dashed_ident(t)) return -1;
	bident(b, t->s, t->len);
	adv(lx);
	return at_end(lx) ? 0 : -1;
}

/* The TOP level of a value this file has claimed.
 *
 * emit_value() is a token-stream normalizer -- the right tool inside a math
 * function, where the contents are an arithmetic expression this file does not
 * type-check. At the top level that is far too permissive: it emits any
 * identifier it is handed, so `width: anchor-size(--foo width) junk`
 * serializes happily and sticks. Accepting everything while looking correct is
 * precisely the failure test_invalid_value exists to catch, so the top level
 * is TYPED: each component must be a length, a percentage, zero, `auto`, or a
 * function, and there may be at most as many components as the property takes.
 */
static int emit_top_value(lexed *lx, buf *b, const char *prop, int plen)
{
	int n = 0;
	int maxn = in_tab(prop, plen, multi_props) ? 4 : 1;

	while (!at_end(lx)) {
		const tok *t;

		if (n >= maxn) return -1;
		if (n > 0) bputc(b, ' ');
		if (try_collapse_calc(lx, b, 0)) { n++; continue; }

		t = cur(lx);
		switch (t->kind) {
		case T_NUM:
			/* A unitless non-zero number is not a length. */
			if (t->num != 0) return -1;
			if (emit_token(lx, b, 0) != 0) return -1;
			break;
		case T_DIM:
		case T_PCT:
			if (emit_token(lx, b, 0) != 0) return -1;
			break;
		case T_FUNC:
			if (emit_function(lx, b, 0) != 0) return -1;
			break;
		case T_IDENT:
			if (!tok_is_ident(t, "auto")) return -1;
			bput(b, "auto", 4);
			adv(lx);
			break;
		default:
			return -1;
		}
		n++;
	}
	return n > 0 ? 0 : -1;
}

/* Does this value mention a function only this file can parse? If not, we have
 * no business claiming the declaration: `width: 10px` belongs to LibCSS and
 * must keep whatever behaviour it has today. */
static int mentions_anchor(lexed *lx)
{
	int i;
	for (i = 0; i < lx->n; i++)
		if (lx->t[i].kind == T_FUNC &&
		    (ieq(lx->t[i].s, lx->t[i].len, "anchor") ||
		     ieq(lx->t[i].s, lx->t[i].len, "anchor-size")))
			return 1;
	return 0;
}

int css_canon_decl(const char *prop, int plen,
		const char *value, int vlen,
		char *out, int outcap, int *outlen)
{
	lexed lx;
	buf b;
	int rc = CSS_CANON_PASS;

	if (prop == NULL || value == NULL || out == NULL || outcap <= 1)
		return CSS_CANON_PASS;
	if (plen < 0) plen = (int)strlen(prop);
	if (vlen < 0) vlen = (int)strlen(value);

	while (plen > 0 && is_ws((unsigned char)prop[0])) { prop++; plen--; }
	while (plen > 0 && is_ws((unsigned char)prop[plen - 1])) plen--;
	while (vlen > 0 && is_ws((unsigned char)value[0])) { value++; vlen--; }
	while (vlen > 0 && is_ws((unsigned char)value[vlen - 1])) vlen--;
	if (plen <= 0) return CSS_CANON_PASS;

	/* A custom property takes any balanced token stream verbatim. */
	if (plen >= 2 && prop[0] == '-' && prop[1] == '-') return CSS_CANON_PASS;

	if (!css_canon_knows_property(prop, plen)) return CSS_CANON_PASS;

	/* An empty value is a removal, not a declaration; let the caller keep
	 * its existing meaning for that. */
	if (vlen <= 0) return CSS_CANON_PASS;

	if (lex(value, vlen, &lx) != 0) {
		/* Tokenization itself failed (an unterminated string). Nothing
		 * downstream can take this either. */
		return CSS_CANON_INVALID;
	}
	if (lx.n == 0) { lex_free(&lx); return CSS_CANON_PASS; }

	b.p = out;
	b.len = 0;
	b.cap = outcap;
	b.ovf = 0;

	if (ieq(prop, plen, "anchor-name")) {
		rc = canon_anchor_name(&lx, &b) == 0 && at_end(&lx)
			? CSS_CANON_OK : CSS_CANON_INVALID;
	} else if (ieq(prop, plen, "position-anchor")) {
		rc = canon_position_anchor(&lx, &b) == 0
			? CSS_CANON_OK : CSS_CANON_INVALID;
	} else if (ieq(prop, plen, "position-area")) {
		rc = canon_position_area(&lx, &b) == 0
			? CSS_CANON_OK : CSS_CANON_INVALID;
	} else if (ieq(prop, plen, "font-family")) {
		/* A CSS-wide keyword as the ENTIRE value is legal and is
		 * LibCSS's to interpret; the same word inside a list is a
		 * parse error. Splitting them here keeps `font-family:
		 * inherit` behaving exactly as it does today. */
		if (lx.n == 1 && ff_css_wide(&lx.t[0]))
			rc = CSS_CANON_PASS;
		else
			rc = canon_font_family(&lx, &b) == 0 && at_end(&lx)
				? CSS_CANON_OK : CSS_CANON_INVALID;
	} else if (in_tab(prop, plen, fsa_props)) {
		if (lx.n == 1 && tok_is_ident(&lx.t[0], "none")) {
			bput(&b, "none", 4);
			rc = CSS_CANON_OK;
		} else {
			rc = canon_font_size_adjust(&lx, &b) == 0
				? CSS_CANON_OK : CSS_CANON_INVALID;
		}
	} else if (in_tab(prop, plen, transform_props)) {
		if (lx.n == 1 && tok_is_ident(&lx.t[0], "none")) {
			bput(&b, "none", 4);
			rc = CSS_CANON_OK;
		} else if (ieq(prop, plen, "rotate")) {
			rc = (canon_rotate(&lx, &b) == 0 && at_end(&lx))
				? CSS_CANON_OK : CSS_CANON_INVALID;
		} else if (ieq(prop, plen, "translate")) {
			rc = canon_translate(&lx, &b) == 0
				? CSS_CANON_OK : CSS_CANON_INVALID;
		} else if (ieq(prop, plen, "scale")) {
			rc = canon_scale(&lx, &b) == 0
				? CSS_CANON_OK : CSS_CANON_INVALID;
		} else {
			/* perspective: a non-negative length, and `none`. */
			rc = (lp_value(&lx, &b, 1, 0) == 0 && at_end(&lx))
				? CSS_CANON_OK : CSS_CANON_INVALID;
		}
	} else if (in_tab(prop, plen, filter_props)) {
		/* `none` is the initial value and is a keyword, not a list. */
		if (lx.n == 1 && tok_is_ident(&lx.t[0], "none")) {
			bput(&b, "none", 4);
			rc = CSS_CANON_OK;
		} else {
			rc = (canon_filter_list(&lx, &b) == 0 && at_end(&lx))
				? CSS_CANON_OK : CSS_CANON_INVALID;
		}
	} else if (in_tab(prop, plen, shape_props)) {
		int off = ieq(prop, plen, "offset-path");
		if (lx.n == 1 && tok_is_ident(&lx.t[0], "none")) {
			bput(&b, "none", 4);
			rc = CSS_CANON_OK;
		} else if (off && lx.t[0].kind == T_FUNC &&
			   ieq(lx.t[0].s, lx.t[0].len, "ray")) {
			rc = (canon_ray(&lx, &b) == 0 && at_end(&lx))
				? CSS_CANON_OK : CSS_CANON_INVALID;
		} else {
			/* clip-path and shape-outside take a geometry box on
			 * its own or beside a shape; offset-path does not,
			 * and takes path() and url() which the other two
			 * spell differently enough to be worth the flags. */
			rc = (canon_shape_value(&lx, &b, 1, 1,
					!ieq(prop, plen, "offset-path")) == 0 &&
			      at_end(&lx))
				? CSS_CANON_OK : CSS_CANON_INVALID;
		}
	} else if (in_tab(prop, plen, grid_props)) {
		/* The template properties take line names and repeat() and
		 * accept `none`; the auto- ones are a bare <track-size>+ and
		 * accept neither. */
		int tmpl = ieq(prop, plen, "grid-template-columns") ||
			   ieq(prop, plen, "grid-template-rows");
		rc = (canon_track_list(&lx, &b, tmpl, tmpl) == 0 && at_end(&lx))
			? CSS_CANON_OK : CSS_CANON_INVALID;
	} else if (in_tab(prop, plen, color_props)) {
		/* WHICH VALUES OF A COLOUR PROPERTY ARE OURS.
		 *
		 * The FUNCTIONS are, because their specified spelling is
		 * defined and LibCSS has none for any of them. A bare keyword
		 * (`red`, `#fff`, `inherit`) is NOT: LibCSS already reads it
		 * back correctly and rerouting a value that works today
		 * through a second serializer buys nothing and risks a
		 * regression -- claiming a property is not claiming every
		 * value of it. The one keyword exception is a SYSTEM colour,
		 * which LibCSS predates entirely and therefore drops.
		 *
		 * border-color and its logical siblings take ONE TO FOUR
		 * colours. Parsing exactly one there and calling the rest a
		 * syntax error would delete `border-color: rgb(1,2,3) red`
		 * from every page that has one -- a value that sticks today. */
		int maxc = (ieq(prop, plen, "border-color") ||
			    ieq(prop, plen, "border-block-color") ||
			    ieq(prop, plen, "border-inline-color")) ? 4 : 1;
		const char *sysk = (lx.n == 1)
			? tab_lookup(&lx.t[0], system_colors) : NULL;

		if (sysk != NULL) {
			bput(&b, sysk, -1);
			rc = CSS_CANON_OK;
		} else if (value_has_var(&lx) && lx.t[0].kind == T_FUNC &&
			   (in_tab(lx.t[0].s, lx.t[0].len, claimed_color_fns) ||
			    in_tab(lx.t[0].s, lx.t[0].len, raw_color_fns))) {
			/* Only when the value is a COLOUR FUNCTION that
			 * happens to contain a var(). A bare `color: var(--x)`
			 * is nobody's colour yet and stays LibCSS's, exactly
			 * as it is today. */
			/* Pending substitution: the original tokens, with
			 * their whitespace normalised and nothing else
			 * touched. On any doubt this PASSES rather than
			 * refusing -- a value nobody can evaluate yet is the
			 * last thing to declare invalid. */
			char nm[64];
			int i, n = lx.t[0].len;
			if (n >= (int)sizeof nm) n = (int)sizeof nm - 1;
			for (i = 0; i < n; i++) {
				char ch = lx.t[0].s[i];
				nm[i] = (ch >= 'A' && ch <= 'Z')
					? (char)(ch + 32) : ch;
			}
			nm[n] = 0;
			rc = (color_fn_body_ok(&lx, 1) &&
			      emit_color_fn_raw(&lx, &b, nm) == 0 &&
			      at_end(&lx))
				? CSS_CANON_OK : CSS_CANON_PASS;
		} else if (lx.t[0].kind == T_FUNC &&
			   (in_tab(lx.t[0].s, lx.t[0].len, claimed_color_fns) ||
			    (rel_lookup(&lx.t[0]) != NULL &&
			     tok_is_ident(&lx.t[1], "from")))) {
			int nc = 0;
			rc = CSS_CANON_OK;
			while (!at_end(&lx)) {
				if (nc >= maxc) { rc = CSS_CANON_INVALID; break; }
				if (nc) bputc(&b, ' ');
				if (canon_color(&lx, &b, 0) != 0) {
					rc = CSS_CANON_INVALID;
					break;
				}
				nc++;
			}
			if (rc == CSS_CANON_OK && nc == 0) rc = CSS_CANON_INVALID;
		} else if (lx.t[0].kind == T_FUNC &&
			   in_tab(lx.t[0].s, lx.t[0].len, raw_color_fns)) {
			/* A Color 4/5 function this file recognises but does
			 * not spell canonically. Emitted with its whitespace
			 * normalised, because DROPPING it is the worse bug:
			 * the value is valid CSS, a script can set it, and
			 * refusing to store it costs more than storing it
			 * unprettified. Measured, when this file first
			 * consumed CSS_CANON_PASS: 2,687 subtests. */
			const char *nm = NULL;
			int i;
			for (i = 0; raw_color_fns[i] != NULL; i++)
				if (ieq(lx.t[0].s, lx.t[0].len, raw_color_fns[i]))
					nm = raw_color_fns[i];
			rc = (nm != NULL && color_fn_body_ok(&lx, 1) &&
			      emit_color_fn_raw(&lx, &b, nm) == 0 &&
			      at_end(&lx))
				? CSS_CANON_OK : CSS_CANON_INVALID;
		} else {
			rc = CSS_CANON_PASS;
		}
	} else if (mentions_anchor(&lx)) {
		int anchor_ok = in_tab(prop, plen, inset_props);
		int size_ok = anchor_ok || in_tab(prop, plen, size_props) ||
			      in_tab(prop, plen, margin_props);
		/* anchor() is only valid in an inset property; anchor-size()
		 * in inset, sizing and margin properties. A misuse is a parse
		 * error, not a fallback -- `width: anchor(--a left)` is
		 * INVALID and must not stick. */
		int i, ok = 1;
		for (i = 0; i < lx.n; i++) {
			if (lx.t[i].kind != T_FUNC) continue;
			if (ieq(lx.t[i].s, lx.t[i].len, "anchor") && !anchor_ok)
				ok = 0;
			if (ieq(lx.t[i].s, lx.t[i].len, "anchor-size") && !size_ok)
				ok = 0;
		}
		if (!ok) rc = CSS_CANON_INVALID;
		else rc = (emit_top_value(&lx, &b, prop, plen) == 0 &&
				at_end(&lx))
			? CSS_CANON_OK : CSS_CANON_INVALID;
	} else {
		/* A sizing/inset/margin property with no anchor function in it
		 * is LibCSS's, unchanged. */
		rc = CSS_CANON_PASS;
	}

	if (rc == CSS_CANON_OK && b.ovf) rc = CSS_CANON_PASS;
	if (rc == CSS_CANON_OK && b.len == 0) rc = CSS_CANON_INVALID;
	if (rc == CSS_CANON_OK && outlen != NULL) *outlen = b.len;

	lex_free(&lx);
	return rc;
}
