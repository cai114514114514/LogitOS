/* The gate for c/lib/wasm: hand-built structural checks, then the official
 * WebAssembly spec test suite.
 *
 * THE NUMBER THIS GATE EXISTS TO PRODUCE IS "WRONGLY ACCEPTED".  Everything
 * downstream of a validator trusts it, so a malformed or invalid module that
 * decodes clean is the only failure here that is not recoverable by the next
 * layer.  It is printed on its own line, it is never folded into a
 * percentage, and any value above zero fails the build.
 *
 * AND THE ACCOUNTING SEPARATES THREE THINGS THAT A PASS/FAIL COLUMN WOULD
 * BLUR, because CLAUDE.md rule 5 is exactly about a control that passes for
 * the wrong reason:
 *
 *   rejected            we refused it, for a reason about the module.
 *   refused-out-of-scope  we refused it because it uses a post-MVP feature
 *                       (WASM_E_UNSUPPORTED).  For a MALFORMED case this is
 *                       NOT a pass -- the suite wanted the module refused for
 *                       an encoding defect and we never got that far.  It is
 *                       counted and printed separately so it can never quietly
 *                       inflate the rejection count.
 *   APPARATUS           WASM_E_NOMEM / WASM_E_LIMIT: our arena or our own
 *                       bound gave up.  Any nonzero count here fails the gate
 *                       outright, because on an arena of zero bytes every
 *                       malformed case would otherwise "pass".
 *
 * The accept side runs against a BASELINE RATCHET rather than a rate, the way
 * tests/wpt.mk does: the current suite carries modules from a dozen post-MVP
 * proposals, so "we reject some valid modules" is expected and the question is
 * whether the SET grew.  A new wrongly-rejected origin fails; the baseline
 * file names each entry by `file.wast:line` so it can be read.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm.h"

#define ARENA_BYTES (64u * 1024u * 1024u)

static unsigned char *g_arena;
static int g_fail;

static void ck(int cond, const char *what)
{
	if (!cond) { printf("  FAIL %s\n", what); g_fail++; }
}

/* run one module image; returns the WASM_E_* code */
static int run(const unsigned char *b, unsigned n)
{
	struct wasm_arena a;
	struct wasm_module m;
	wasm_arena_init(&a, g_arena, ARENA_BYTES);
	return wasm_load(b, n, &m, &a);
}

/* ---------------------------------------------------------------- part 1
 * Hand-built checks.  Deliberately few: the spec suite is the real corpus and
 * a hand-written expectation only records what its author already believed.
 * What is here is the arithmetic that has no representation in the suite (the
 * arena, the utf8 predicate as a function) plus a floor so that this gate says
 * something on a machine with no corpus at all. */

static const unsigned char EMPTY[8] = { 0,'a','s','m', 1,0,0,0 };

static void part1_builtin(void)
{
	printf("built-in structural checks\n");

	/* the preamble */
	ck(run(EMPTY, 8) == WASM_OK, "empty module accepted");
	ck(run((const unsigned char *)"", 0) == WASM_E_END, "empty input -> unexpected end");
	ck(run((const unsigned char *)"\0as", 3) == WASM_E_END, "truncated magic -> unexpected end");
	{
		unsigned char b[8]; memcpy(b, EMPTY, 8); b[0] = 'a';
		ck(run(b, 8) == WASM_E_MAGIC, "wrong magic -> magic header not detected");
		memcpy(b, EMPTY, 8); b[4] = 0x0d;
		ck(run(b, 8) == WASM_E_VERSION, "wrong version refused");
	}

	/* LEB128: padding is LEGAL up to ceil(N/7) bytes, longer is not.  This is
	 * the rule the natural guess gets backwards, so it is checked from both
	 * sides.  A type section whose vector count is a padded zero: */
	{
		unsigned char ok5[]  = { 0,'a','s','m',1,0,0,0, 1,5, 0x80,0x80,0x80,0x80,0x00 };
		unsigned char bad6[] = { 0,'a','s','m',1,0,0,0, 1,6, 0x80,0x80,0x80,0x80,0x80,0x00 };
		ck(run(ok5, sizeof ok5) == WASM_OK, "u32 padded to 5 bytes is legal");
		ck(run(bad6, sizeof bad6) == WASM_E_LEB_LONG, "u32 in 6 bytes is too long");
	}
	{   /* last byte carrying bits above 32 */
		unsigned char big[] = { 0,'a','s','m',1,0,0,0, 1,5, 0x80,0x80,0x80,0x80,0x10 };
		ck(run(big, sizeof big) == WASM_E_LEB_LARGE, "u32 with bits above 32 is too large");
	}

	/* section ordering and the function/code pairing */
	{
		unsigned char rev[] = { 0,'a','s','m',1,0,0,0, 5,1,0, 1,1,0 };   /* memory then type */
		ck(run(rev, sizeof rev) == WASM_E_SECTION_ORDER, "sections out of order refused");
	}
	{   /* one entry in `function`, none in `code` */
		unsigned char lens[] = { 0,'a','s','m',1,0,0,0,
		                         1,4, 1, 0x60,0,0,      /* type: [] -> [] */
		                         3,2, 1, 0 };           /* function: one func, type 0 */
		ck(run(lens, sizeof lens) == WASM_E_LENGTHS, "function without code refused");
	}

	/* the smallest complete function, and then the same body with a type error */
	{
		unsigned char good[] = { 0,'a','s','m',1,0,0,0,
		                         1,5, 1, 0x60,0,1,0x7F, /* [] -> [i32] */
		                         3,2, 1, 0,
		                         10,6, 1, 4, 0, 0x41,0x00, 0x0B };
		unsigned char bad[sizeof good];
		ck(run(good, sizeof good) == WASM_OK, "i32.const body validates");
		memcpy(bad, good, sizeof good);
		bad[sizeof good - 3] = 0x42;   /* i64.const: wrong result type */
		ck(run(bad, sizeof bad) == WASM_E_TYPE, "i64 body for an i32 result refused");
	}

	/* the polymorphic stack: `unreachable` must make the block satisfy any
	 * result, and must NOT excuse a wrong type that is actually present */
	{
		unsigned char poly[] = { 0,'a','s','m',1,0,0,0,
		                         1,5, 1, 0x60,0,1,0x7F,
		                         3,2, 1, 0,
		                         10,5, 1, 3, 0, 0x00, 0x0B };  /* unreachable; end */
		ck(run(poly, sizeof poly) == WASM_OK, "unreachable satisfies any result");
	}
	{
		unsigned char poly2[] = { 0,'a','s','m',1,0,0,0,
		                          1,5, 1, 0x60,0,1,0x7F,
		                          3,2, 1, 0,
		                          10,7, 1, 5, 0, 0x00, 0x42,0x00, 0x0B };
		/* unreachable; i64.const 0; end -- the i64 IS on the stack, so the
		 * function's i32 result is a genuine mismatch even in dead code. */
		ck(run(poly2, sizeof poly2) == WASM_E_TYPE,
		   "a wrong type present after unreachable is still a mismatch");
	}

	/* A POLYMORPHIC `select` STILL PUSHES A VALUE, and this case is here
	 * because the corpus caught it and a built-in must pin it: with
	 * WASM_VT_UNKNOWN spelled 0x00 -- the same number wasm_valid.c uses for
	 * "this block yields no result" -- push_val dropped it, the stack came
	 * back one slot short, and the leftover check at `end` did not fire.
	 * `(func (unreachable) (select))` validated CLEAN against an assertion
	 * that says "type mismatch" (unreached-invalid.wast:56).  It is the only
	 * opcode in MVP that can push Unknown, so this is the whole exposure. */
	{
		unsigned char polysel[] = { 0,'a','s','m',1,0,0,0,
		                            1,4, 1, 0x60,0,0,        /* [] -> [] */
		                            3,2, 1, 0,
		                            10,6, 1, 4, 0, 0x00, 0x1B, 0x0B };
		ck(run(polysel, sizeof polysel) == WASM_E_TYPE,
		   "unreachable; select leaves an operand and is a type error");
	}

	/* An operand left on the stack when a block ends is a type error.  This
	 * check exists to give test-wasm-negctl something to bite on with NO
	 * corpus present: WASM_NEGCTL_STACK deletes exactly the line that catches
	 * it, and without this case the negative control would depend on a
	 * downloaded suite and would silently stop controlling anything the day
	 * the download failed. */
	{
		unsigned char leftover[] = { 0,'a','s','m',1,0,0,0,
		                             1,4, 1, 0x60,0,0,        /* [] -> [] */
		                             3,2, 1, 0,
		                             10,6, 1, 4, 0, 0x41,0x00, 0x0B };
		ck(run(leftover, sizeof leftover) == WASM_E_TYPE,
		   "an operand left on the stack at `end` is a type error");
	}

	/* alignment is an INVALID condition, not a malformed one */
	{
		unsigned char al[] = { 0,'a','s','m',1,0,0,0,
		                       1,4, 1, 0x60,0,0,
		                       3,2, 1, 0,
		                       5,3, 1, 0, 1,                 /* one memory */
		                       10,9, 1, 7, 0, 0x41,0x00, 0x28,0x03,0x00, 0x0B };
		/* i32.load align=3 (8 bytes) is larger than natural (4) */
		ck(run(al, sizeof al) == WASM_E_ALIGN, "over-aligned i32.load refused");
	}

	/* export names must be unique, and they are compared as BYTES */
	{
		unsigned char dup[] = { 0,'a','s','m',1,0,0,0,
		                        1,4, 1, 0x60,0,0,
		                        3,2, 1, 0,
		                        7,9, 2, 1,'a',0,0, 1,'a',0,0,
		                        10,4, 1, 2, 0, 0x0B };
		ck(run(dup, sizeof dup) == WASM_E_DUP_EXPORT, "duplicate export name refused");
	}

	/* memory limits: 65537 pages is one too many */
	{
		unsigned char mem[] = { 0,'a','s','m',1,0,0,0, 5,5, 1, 0, 0x81,0x80,0x04 };
		ck(run(mem, sizeof mem) == WASM_E_MEMSIZE, "65537 pages refused");
	}

	/* the utf8 predicate, driven directly */
	ck(wasm_utf8_ok((const uint8_t *)"", 0) == 1, "utf8: empty");
	ck(wasm_utf8_ok((const uint8_t *)"\xC2\x80", 2) == 1, "utf8: U+0080");
	ck(wasm_utf8_ok((const uint8_t *)"\xC0\x80", 2) == 0, "utf8: overlong 2-byte NUL");
	ck(wasm_utf8_ok((const uint8_t *)"\xE0\x80\x80", 3) == 0, "utf8: overlong 3-byte");
	ck(wasm_utf8_ok((const uint8_t *)"\xED\xA0\x80", 3) == 0, "utf8: surrogate U+D800");
	ck(wasm_utf8_ok((const uint8_t *)"\xF4\x90\x80\x80", 4) == 0, "utf8: above U+10FFFF");
	ck(wasm_utf8_ok((const uint8_t *)"\xF4\x8F\xBF\xBF", 4) == 1, "utf8: U+10FFFF");
	ck(wasm_utf8_ok((const uint8_t *)"\xE2\x82", 2) == 0, "utf8: truncated 3-byte");

	/* the arena refuses rather than wraps */
	{
		struct wasm_arena a;
		unsigned char buf[64];
		wasm_arena_init(&a, buf, sizeof buf);
		ck(wasm_arena_alloc(&a, 0x40000000u, 16) == NULL, "arena: count*size overflow refused");
		ck(wasm_arena_alloc(&a, 65, 1) == NULL, "arena: over-capacity refused");
		ck(wasm_arena_alloc(&a, 8, 1) != NULL, "arena: in-capacity granted");
	}
}

/* ---------------------------------------------------------------- part 2 */

struct tally {
	unsigned n, rejected, accepted, unsupported, apparatus;
};

static int g_verbose;
static const char *g_baseline_path;
#define MAXBASE 4096
static char *g_base[MAXBASE];
static int g_nbase, g_base_hit[MAXBASE];

static void load_baseline(void)
{
	FILE *f;
	char line[512];
	if (!g_baseline_path) return;
	f = fopen(g_baseline_path, "r");
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *p = line, *e;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\n' || !*p) continue;
		e = p; while (*e && *e != '\n' && *e != '\t' && *e != ' ') e++;
		*e = 0;
		if (g_nbase < MAXBASE) g_base[g_nbase++] = strdup(p);
	}
	fclose(f);
}

static int baselined(const char *origin)
{
	int i;
	for (i = 0; i < g_nbase; i++)
		if (!strcmp(g_base[i], origin)) { g_base_hit[i] = 1; return 1; }
	return 0;
}

static unsigned char *slurp(const char *path, unsigned *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return NULL; }
	b = malloc((size_t)n + 1);
	if (!b) { fclose(f); return NULL; }
	if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f);
	*len = (unsigned)n;
	return b;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : NULL;
	char path[1024], line[1024];
	FILE *mf;
	struct tally acc = {0,0,0,0,0}, mal = {0,0,0,0,0}, inv = {0,0,0,0,0};
	unsigned newreject = 0, wrongly_accepted = 0;
	char mode[64] = "unknown";
	int i;

	g_baseline_path = argc > 2 ? argv[2] : NULL;
	/* WASM_VERBOSE=1 prints every wrongly-rejected origin rather than the
	 * first 25.  That listing is how the baseline file is written in the
	 * first place, so the gate can regenerate its own expectations. */
	if (getenv("WASM_VERBOSE")) g_verbose = 1;
	g_arena = malloc(ARENA_BYTES);
	if (!g_arena) { printf("wasm: cannot allocate the %u-byte arena\n", ARENA_BYTES); return 2; }

	part1_builtin();

	if (!dir) {
		printf("\nwasm-spec: SKIPPED -- no corpus directory given.\n");
		printf("           The official spec suite is what makes the malformed and\n");
		printf("           invalid halves of this gate mean anything; without it this\n");
		printf("           binary has checked its own author's expectations and nothing\n");
		printf("           else.  Settle it with:   make wasm-fetch\n");
		printf("\nbuilt-in: %s (%d failure%s)\n", g_fail ? "FAIL" : "ok",
		       g_fail, g_fail == 1 ? "" : "s");
		return g_fail ? 1 : 0;
	}

	snprintf(path, sizeof path, "%s/manifest.tsv", dir);
	mf = fopen(path, "r");
	if (!mf) {
		printf("\nwasm-spec: SKIPPED -- %s is absent.\n", path);
		printf("           Fetch and build the corpus with:   make wasm-fetch\n");
		printf("\nbuilt-in: %s (%d failure%s)\n", g_fail ? "FAIL" : "ok",
		       g_fail, g_fail == 1 ? "" : "s");
		return g_fail ? 1 : 0;
	}

	load_baseline();
	printf("\nspec suite corpus: %s\n", dir);

	while (fgets(line, sizeof line, mf)) {
		char *kind, *file, *origin, *tab;
		unsigned char *bytes;
		unsigned n;
		int e;
		struct tally *t;

		if (line[0] == '#') {
			if (!strncmp(line, "# mode\t", 7)) {
				char *p = line + 7, *q = mode;
				while (*p && *p != '\n' && q < mode + sizeof mode - 1) *q++ = *p++;
				*q = 0;
			}
			continue;
		}
		kind = line;
		tab = strchr(kind, '\t'); if (!tab) continue; *tab = 0; file = tab + 1;
		tab = strchr(file, '\t'); if (!tab) continue; *tab = 0; origin = tab + 1;
		tab = strchr(origin, '\t'); if (tab) *tab = 0;
		tab = strchr(origin, '\n'); if (tab) *tab = 0;

		if      (!strcmp(kind, "accept"))    t = &acc;
		else if (!strcmp(kind, "malformed")) t = &mal;
		else if (!strcmp(kind, "invalid"))   t = &inv;
		else continue;

		snprintf(path, sizeof path, "%s/%s", dir, file);
		bytes = slurp(path, &n);
		if (!bytes) continue;
		e = run(bytes, n);
		free(bytes);
		t->n++;

		if (e == WASM_E_NOMEM || e == WASM_E_LIMIT) {
			t->apparatus++;
			printf("  APPARATUS %-28s %s\n", origin, wasm_errstr(e));
			continue;
		}
		if (e == WASM_E_UNSUPPORTED) { t->unsupported++; continue; }
		if (e == WASM_OK) {
			t->accepted++;
			if (t != &acc) {
				wrongly_accepted++;
				if (g_verbose || wrongly_accepted <= 25)
					printf("  WRONGLY ACCEPTED  [%s] %s\n", kind, origin);
			}
			continue;
		}
		t->rejected++;
		if (t == &acc && !baselined(origin)) {
			newreject++;
			if (g_verbose || newreject <= 25)
				printf("  WRONGLY REJECTED  %-28s %s\n", origin, wasm_errstr(e));
		}
	}
	fclose(mf);

	printf("\ncorpus mode: %s\n", mode);
	printf("%-10s %7s %9s %9s %13s %10s\n",
	       "kind", "cases", "accepted", "rejected", "out-of-scope", "APPARATUS");
	printf("%-10s %7u %9u %9u %13u %10u\n", "accept",
	       acc.n, acc.accepted, acc.rejected, acc.unsupported, acc.apparatus);
	printf("%-10s %7u %9u %9u %13u %10u\n", "malformed",
	       mal.n, mal.accepted, mal.rejected, mal.unsupported, mal.apparatus);
	printf("%-10s %7u %9u %9u %13u %10u\n", "invalid",
	       inv.n, inv.accepted, inv.rejected, inv.unsupported, inv.apparatus);

	printf("\nmalformed: %u of %u REJECTED  (%u refused as post-MVP, which is NOT a pass)\n",
	       mal.rejected, mal.n, mal.unsupported);
	printf("invalid:   %u of %u REJECTED  (%u refused as post-MVP, which is NOT a pass)\n",
	       inv.rejected, inv.n, inv.unsupported);
	printf("WRONGLY ACCEPTED: %u   <- the only number in this gate that must be zero\n",
	       wrongly_accepted);

	/* An unmatched baseline entry is NOT necessarily stale, and saying so
	 * flatly would get live entries deleted.  The two corpus modes cover
	 * different halves of the suite -- binary-only carries the seven
	 * `(ref func)` cases of elem.wast that wast2json never emits, and
	 * wast2json carries the multi-memory imports.wast cases that binary-only
	 * never emits -- so the baseline is the UNION and each mode leaves the
	 * other mode's entries unmatched.  The count is what to watch: it should
	 * fall to zero only when a mode is run that contains every entry. */
	{
		unsigned unmatched = 0;
		for (i = 0; i < g_nbase; i++)
			if (!g_base_hit[i]) {
				unmatched++;
				if (g_verbose)
					printf("  not in THIS corpus mode: %s\n", g_base[i]);
			}
		if (unmatched)
			printf("baseline: %d entries, %u matched, %u not present in corpus mode `%s`\n"
			       "          (the two modes cover different halves; WASM_VERBOSE=1 lists them)\n",
			       g_nbase, (unsigned)g_nbase - unmatched, unmatched, mode);
	}

	if (newreject)
		printf("\n%u valid module(s) rejected that the baseline does not name.\n"
		       "If they are genuinely post-MVP they belong in %s with a reason;\n"
		       "if they are MVP they are a bug in c/lib/wasm.\n",
		       newreject, g_baseline_path ? g_baseline_path : "(no baseline given)");

	if (acc.apparatus + mal.apparatus + inv.apparatus)
		printf("\nAPPARATUS failures are not verdicts about any module.  A gate that\n"
		       "counted them as rejections would report green on a zero-byte arena.\n");

	printf("\nbuilt-in: %s (%d failure%s)\n", g_fail ? "FAIL" : "ok",
	       g_fail, g_fail == 1 ? "" : "s");

	if (g_fail || wrongly_accepted || newreject ||
	    acc.apparatus + mal.apparatus + inv.apparatus)
		return 1;
	return 0;
}
