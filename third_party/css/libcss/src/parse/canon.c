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

static int emit_value(lexed *lx, buf *b, int depth, int stop_at_comma);
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

/* Emit a whole value: a sequence of component values, space separated. When
 * `stop_at_comma`, a top-level comma ends it (used for argument lists). */
static int emit_value(lexed *lx, buf *b, int depth, int stop_at_comma)
{
	int first = 1;

	if (depth >= CANON_MAXDEPTH) return -1;
	while (!at_end(lx)) {
		const tok *t = cur(lx);
		if (t->kind == T_RPAREN) break;
		if (t->kind == T_COMMA && stop_at_comma) break;
		if (t->kind == T_COMMA) { bcomma(b); adv(lx); first = 1;
			continue; }
		if (!first) bputc(b, ' ');
		if (emit_token(lx, b, depth + 1) != 0) return -1;
		first = 0;
	}
	return first ? -1 : 0;	/* an empty value is not a value */
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

/* ====================================================================
 * Entry points
 * ==================================================================== */

int css_canon_knows_property(const char *prop, int plen)
{
	if (prop == NULL) return 0;
	if (plen < 0) plen = (int)strlen(prop);
	if (plen <= 0) return 0;
	if (in_tab(prop, plen, inset_props)) return 1;
	if (in_tab(prop, plen, size_props)) return 1;
	if (in_tab(prop, plen, margin_props)) return 1;
	if (ieq(prop, plen, "anchor-name")) return 1;
	if (ieq(prop, plen, "position-anchor")) return 1;
	return 0;
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
