/* Pinyin IME engine (c/lib/ime/pinyin.c) -- host gate.
 *
 * Links pinyin.c directly (no kernel, no vfs): loads the real shipped
 * fsroot/ime/pinyin.dat off disk exactly as tests/unit/ttf_test.c loads the
 * real shipped font, so every assertion below is against the dictionary
 * that actually ships, not a synthetic fixture.
 *
 * Build: make test-ime (tests/ime.mk)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#include "pinyin.h"
#include "logit_abi.h" /* -Iinclude/abi, see tests/ime.mk */

/* pinyin.h pins IME_KEY_PGUP/PGDN by VALUE rather than including the ABI
 * header (see pinyin.h's comment) -- this is the cross-check that the pin
 * has not drifted from the real numbers wm.c delivers. */
#if IME_KEY_PGUP != KEY_PGUP
#error "IME_KEY_PGUP has drifted from KEY_PGUP in include/abi/logit_abi.h"
#endif
#if IME_KEY_PGDN != KEY_PGDN
#error "IME_KEY_PGDN has drifted from KEY_PGDN in include/abi/logit_abi.h"
#endif

static int fails, checks;

/* "FAIL:" WITH THE COLON, not "FAIL" alone: tests/ime.mk's negative control
 * greps '^FAIL:' for an EXACT count, and the trailing summary line below
 * prints "FAILED\n" -- which starts with the four bytes "FAIL" too. A grep
 * pattern of bare '^FAIL' would count that summary line as a fifth
 * assertion every single run (caught by hand-checking the negctl's output
 * against its own grep before trusting the count -- see the engine's
 * report). The colon is the fix: it appears after every per-check verdict
 * and never after the word "FAILED". */
static void ck(int cond, const char *what, const char *detail) {
	checks++;
	printf("%s %s%s%s\n", cond ? "ok:  " : "FAIL:", what,
	       detail && *detail ? "  " : "", detail ? detail : "");
	if (!cond) fails++;
}

/* ---- test-only helpers (host libc is fine here; pinyin.c itself is not) */

static void utf8_encode(const uint32_t *cp, int n, char *out, size_t outsz) {
	size_t o = 0;
	for (int i = 0; i < n; i++) {
		uint32_t c = cp[i];
		if (c < 0x80) {
			if (o + 1 >= outsz) break;
			out[o++] = (char)c;
		} else if (c < 0x800) {
			if (o + 2 >= outsz) break;
			out[o++] = (char)(0xC0 | (c >> 6));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			if (o + 3 >= outsz) break;
			out[o++] = (char)(0xE0 | (c >> 12));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else {
			if (o + 4 >= outsz) break;
			out[o++] = (char)(0xF0 | (c >> 18));
			out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[o] = 0;
}

static void feed_str(struct ime_state *st, const char *s) {
	for (; *s; s++) ime_feed(st, (unsigned char)*s);
}

/* True if `cp` (candidate codepoints) equals the fixed list `want`. */
static int cp_eq(const uint32_t *cp, int n, const uint32_t *want, int wn) {
	if (n != wn) return 0;
	for (int i = 0; i < n; i++)
		if (cp[i] != want[i]) return 0;
	return 1;
}

/* True if some candidate on the CURRENT page equals `want`. */
static int page_has(const struct ime_state *st, const uint32_t *want, int wn) {
	struct ime_candidate page[IME_PAGE_SIZE];
	int n = ime_candidates(st, page, IME_PAGE_SIZE);
	for (int i = 0; i < n; i++)
		if (cp_eq(page[i].cp, page[i].ncp, want, wn)) return 1;
	return 0;
}

static char *load_file(const char *path, long *out_len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = (char *)malloc((size_t)n);
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "short read on %s\n", path);
		exit(1);
	}
	fclose(f);
	*out_len = n;
	return buf;
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "fsroot/ime/pinyin.dat";
	long len = 0;
	char *dat = load_file(path, &len);

	const struct ime_dict *dict = ime_open(dat, (size_t)len);
	if (!dict) {
		fprintf(stderr, "ime_open refused %s (%ld bytes) -- bad header?\n", path, len);
		return 1;
	}
	printf("loaded %s: %ld bytes, %u keys\n", path, len, dict->key_count);

	struct ime_state st;

	/* ---- 1. "nihao" letter by letter: first candidate is 你好 ---------- */
	printf("\n-- nihao --\n");
	ime_reset(&st, dict);
	int last_ret = -1;
	last_ret = ime_feed(&st, 'n');
	ck(last_ret == IME_FEED_COMPOSING, "feeding 'n' composes", "");
	ime_feed(&st, 'i');
	ime_feed(&st, 'h');
	ime_feed(&st, 'a');
	last_ret = ime_feed(&st, 'o');
	ck(last_ret == IME_FEED_COMPOSING, "feeding the 5th letter still composes", "");
	ck(st.raw_len == 5 && memcmp(st.raw, "nihao", 5) == 0, "raw buffer is exactly 'nihao'", "");

	struct ime_candidate page[IME_PAGE_SIZE];
	int npage = ime_candidates(&st, page, IME_PAGE_SIZE);
	char buf[64];
	{
		char detail[128];
		utf8_encode(page[0].cp, page[0].ncp, buf, sizeof buf);
		snprintf(detail, sizeof detail, "candidate[0] = %s (%d cp), ncand=%d", buf, page[0].ncp, st.ncand);
		static const uint32_t nihao_cp[2] = {0x4F60, 0x597D}; /* 你好 */
		ck(npage > 0 && cp_eq(page[0].cp, page[0].ncp, nihao_cp, 2), "first candidate is 你好 (U+4F60 U+597D)", detail);
	}

	uint32_t commit_out[8];
	int cn = ime_commit(&st, 0, commit_out, 8);
	{
		char detail[64];
		snprintf(detail, sizeof detail, "got %d cp: %04X %04X", cn, cn > 0 ? commit_out[0] : 0, cn > 1 ? commit_out[1] : 0);
		ck(cn == 2 && commit_out[0] == 0x4F60 && commit_out[1] == 0x597D, "ime_commit(idx=0) yields U+4F60 U+597D", detail);
	}

	/* A second, segmentation-only candidate is expected too: "ni"+"ha"+"o"
	 * (你+哈+哦), since "niha" is not itself a legal syllable and "ha","o"
	 * are -- see pinyin.c's segmentation comment. Not asserted as REQUIRED
	 * by the brief, but its presence is what proves tier 1 ran at all
	 * rather than tier 0 alone; recorded here so a future change to either
	 * tier shows up as a diff instead of silence. */
	{
		static const uint32_t niha_o[3] = {0x4F60, 0x54C8, 0x54E6}; /* 你哈哦 */
		ck(page_has(&st, niha_o, 3), "tier-1 segmentation candidate 你哈哦 (ni+ha+o) is also present", "");
	}

	/* ---- 2. backspace restores the previous state EXACTLY -------------- */
	printf("\n-- backspace exactness --\n");
	{
		struct ime_state a, b;
		ime_reset(&a, dict);
		feed_str(&a, "niha"); /* stop one letter short of nihao */
		struct ime_candidate pa[IME_PAGE_SIZE];
		int na = ime_candidates(&a, pa, IME_PAGE_SIZE);

		ime_reset(&b, dict);
		feed_str(&b, "nihao");
		int ret = ime_feed(&b, 8); /* backspace */
		ck(ret == IME_FEED_COMPOSING, "backspace after 'nihao' returns COMPOSING (buffer not empty)", "");
		ck(b.raw_len == 4 && memcmp(b.raw, "niha", 4) == 0, "raw buffer after backspace is exactly 'niha'", "");

		struct ime_candidate pb[IME_PAGE_SIZE];
		int nb = ime_candidates(&b, pb, IME_PAGE_SIZE);
		int same = (na == nb) && (a.ncand == b.ncand);
		for (int i = 0; same && i < na; i++)
			if (!cp_eq(pa[i].cp, pa[i].ncp, pb[i].cp, pb[i].ncp)) same = 0;
		{
			char detail[64];
			snprintf(detail, sizeof detail, "typed-to-'niha' ncand=%d, backspaced-from-'nihao' ncand=%d", a.ncand, b.ncand);
			ck(same, "backspacing to 'niha' matches typing 'niha' directly, byte for byte", detail);
		}

		/* backspace all the way out: IME_FEED_EMPTY exactly at raw_len==0 */
		struct ime_state c;
		ime_reset(&c, dict);
		feed_str(&c, "ni");
		ime_feed(&c, 8);
		int ret2 = ime_feed(&c, 8);
		ck(ret2 == IME_FEED_EMPTY, "backspacing the last letter returns IME_FEED_EMPTY", "");
		ck(c.raw_len == 0 && c.ncand == 0, "buffer and candidates are both empty", "");
		ck(ime_feed(&c, 8) == IME_FEED_IGNORED, "backspace on an already-empty buffer is IGNORED, not negative raw_len", "");
	}

	/* ---- 3. "xian": both 西安 and 先 in the first page ------------------ */
	printf("\n-- xian ambiguity --\n");
	ime_reset(&st, dict);
	feed_str(&st, "xian");
	{
		static const uint32_t xian_cp[1] = {0x5148};        /* 先 */
		static const uint32_t xian2_cp[2] = {0x897F, 0x5B89}; /* 西安 */
		char detail[64];
		snprintf(detail, sizeof detail, "ncand=%d (dictionary key \"xian\" alone has 32 candidates)", st.ncand);
		ck(page_has(&st, xian_cp, 1), "先 (single-syllable reading) is on the first page", detail);
		ck(page_has(&st, xian2_cp, 2), "西安 (xi+an reading) is on the first page", detail);
	}
	/* Documented, not asserted as a requirement: BOTH of these come from
	 * TIER 0 alone (the dictionary's own key "xian" already lists both --
	 * see pinyin.c's segmentation comment for why the concatenation-with-no-
	 * separator key format makes this true regardless of segmentation).
	 * Confirmed by checking they are unaffected by IME_NO_BACKTRACK below. */

	/* apostrophe forces the split and suppresses the single-syllable
	 * reading: "xi'an" must NOT surface 先 (which is only reachable via the
	 * whole-buffer key "xian", disabled whenever an apostrophe is present). */
	printf("\n-- xi'an (explicit separator) --\n");
	ime_reset(&st, dict);
	feed_str(&st, "xi'an");
	{
		static const uint32_t xian_cp[1] = {0x5148}; /* 先 */
		char detail[64];
		snprintf(detail, sizeof detail, "ncand=%d", st.ncand);
		ck(!page_has(&st, xian_cp, 1), "xi'an does NOT surface the single-syllable 先 reading", detail);
		ck(st.ncand >= 1, "xi'an still produces at least one segmentation-composed candidate", detail);
		if (st.ncand >= 1) {
			utf8_encode(st.cand[0].cp, st.cand[0].ncp, buf, sizeof buf);
			printf("     xi'an composed candidate[0] = %s\n", buf);
		}
	}

	/* ---- 4. "nhao": zero candidates, raw letters still committable ----- */
	printf("\n-- nhao (unsegmentable) --\n");
	ime_reset(&st, dict);
	feed_str(&st, "nhao");
	ck(st.ncand == 0, "\"nhao\" produces zero candidates", "");
	{
		int n = ime_candidates(&st, page, IME_PAGE_SIZE);
		ck(n == 0, "ime_candidates for \"nhao\" returns 0, not -1 (composing, just empty)", "");
	}
	{
		uint32_t out[8];
		int n = ime_commit(&st, IME_COMMIT_RAW, out, 8);
		int ok = n == 4 && out[0] == 'n' && out[1] == 'h' && out[2] == 'a' && out[3] == 'o';
		ck(ok, "ime_commit(IME_COMMIT_RAW) yields the literal letters 'n','h','a','o'", "");
	}
	ck(ime_commit(&st, 0, (uint32_t[8]){0}, 8) == -1, "ime_commit(idx=0) on zero candidates is refused (-1)", "");

	/* ---- 5. paging past the end refuses --------------------------------- */
	printf("\n-- paging --\n");
	ime_reset(&st, dict);
	feed_str(&st, "xian"); /* 32 (tier0) + >=1 (tier1) candidates -- multiple pages at IME_PAGE_SIZE=9 */
	int npages = (st.ncand + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
	{
		char detail[64];
		snprintf(detail, sizeof detail, "ncand=%d -> %d pages", st.ncand, npages);
		ck(npages >= 2, "xian's candidate list spans more than one page", detail);
	}
	int paged = 0;
	while (ime_feed(&st, IME_KEY_PGDN) == IME_FEED_PAGED) paged++;
	{
		char detail[64];
		snprintf(detail, sizeof detail, "reached page %d of %d, paged forward %d times", st.page, npages, paged);
		ck(st.page == npages - 1, "paging forward stops exactly at the last page", detail);
		ck(paged == npages - 1, "the number of successful PGDN feeds equals npages-1", detail);
	}
	ck(ime_feed(&st, IME_KEY_PGDN) == IME_FEED_IGNORED, "one more PGDN past the last page is IGNORED, not wrapped or crashed", "");
	ck(st.page == npages - 1, "page index is unchanged by the refused PGDN", "");
	int back = 0;
	while (ime_feed(&st, IME_KEY_PGUP) == IME_FEED_PAGED) back++;
	{
		char detail[64];
		snprintf(detail, sizeof detail, "paged backward %d times", back);
		ck(st.page == 0, "paging backward returns exactly to page 0", detail);
	}
	ck(ime_feed(&st, IME_KEY_PGUP) == IME_FEED_IGNORED, "PGUP on page 0 is IGNORED, not negative", "");

	/* ---- 6. escape cancels the whole composition ------------------------ */
	printf("\n-- escape --\n");
	ime_reset(&st, dict);
	feed_str(&st, "nihao");
	ck(ime_feed(&st, 27) == IME_FEED_CANCELLED, "escape returns CANCELLED", "");
	ck(st.raw_len == 0 && st.ncand == 0, "escape clears the buffer and candidates", "");

	/* ---- 7. the negative-control witnesses (see below for why "xian"
	 *         itself cannot serve as one) --------------------------------- */
	printf("\n-- backtracking witnesses (angong / jini / xier) --\n");
	{
		struct {
			const char *buf;
			const uint32_t *want;
			int wn;
			const char *label;
		} cases[3] = {
			{"angong", (const uint32_t[]){0x6309, 0x5171}, 2, "angong -> 按共 (an+gong; greedy commits \"ang\" then dead-ends on \"ong\"/\"ng\")"},
			{"jini", (const uint32_t[]){0x53CA, 0x4F60}, 2, "jini -> 及你 (ji+ni; greedy commits \"jin\" then dead-ends on \"i\")"},
			{"xier", (const uint32_t[]){0x897F, 0x800C}, 2, "xier -> 西而 (xi+er; greedy commits \"xie\" then dead-ends on \"r\")"},
		};
		for (int i = 0; i < 3; i++) {
			ime_reset(&st, dict);
			feed_str(&st, cases[i].buf);
			char detail[80];
			snprintf(detail, sizeof detail, "ncand=%d", st.ncand);
			ck(page_has(&st, cases[i].want, cases[i].wn), cases[i].label, detail);
			/* none of these three strings is a dictionary key itself --
			 * confirmed offline against fsroot/ime/pinyin.dat -- so this
			 * candidate can ONLY come from tier 1 (segmentation), and ONLY
			 * from the backtracked parse: greedy longest-first alone
			 * dead-ends on every one of them. */
		}
	}

	/* ---- 8. cost: cycles per ime_feed(), measured ------------------------ */
	printf("\n-- cost --\n");
#if defined(__x86_64__) || defined(__i386__)
	{
		/* Typical case: a short, realistic composition. */
		ime_reset(&st, dict);
		const char *typical = "zhongguo";
		unsigned long long t0 = __rdtsc();
		const int N = 2000;
		for (int rep = 0; rep < N; rep++) {
			ime_reset(&st, dict);
			for (const char *p = typical; *p; p++) ime_feed(&st, (unsigned char)*p);
		}
		unsigned long long t1 = __rdtsc();
		double per_word = (double)(t1 - t0) / N;
		printf("typical: %.0f cycles for the WHOLE word \"%s\" (%d keys) = %.0f cycles/key\n",
		       per_word, typical, (int)strlen(typical), per_word / (double)strlen(typical));

		/* Adversarial case: "ao" repeated to fill IME_MAX_RAW. "ao", "a" and
		 * "o" are ALL independently legal syllables (袄/阿.../哦...), so at
		 * every "ao" boundary the search can take "ao" whole (advance 2) or
		 * "a" then "o" separately (advance 1, then 1) -- genuine branching
		 * at every one of the 32 boundaries, not just a long deterministic
		 * walk (an all-distinct-1-letter buffer like "aeoaeo..." turned out
		 * to have exactly one legal cut at every position -- no branching at
		 * all -- verified with tools/gen_pinyin_syllables.py's own table
		 * offline before writing this comment, not assumed). Confirmed via
		 * the same offline search: this pattern reaches IME_MAX_PARSES
		 * (capped at 8) almost immediately -- the parse CAP, not the
		 * IME_DFS_BUDGET call cap, is what actually bounds this case; both
		 * are reported below rather than asserted in advance. */
		char adv[IME_MAX_RAW + 1];
		for (int i = 0; i < IME_MAX_RAW; i += 2) {
			adv[i] = 'a';
			adv[i + 1] = 'o';
		}
		adv[IME_MAX_RAW] = 0;
		int an = IME_MAX_RAW;

		ime_reset(&st, dict);
		unsigned long long t2 = __rdtsc();
		const int M = 500;
		for (int rep = 0; rep < M; rep++) {
			ime_reset(&st, dict);
			for (int i = 0; i < an; i++) ime_feed(&st, (unsigned char)adv[i]);
		}
		unsigned long long t3 = __rdtsc();
		double per_worst = (double)(t3 - t2) / M;
		printf("worst-case: %.0f cycles for a full %d-letter buffer of \"ao\" repeated = %.0f cycles/key\n",
		       per_worst, an, per_worst / an);
		printf("(bounded by IME_MAX_PARSES=8 and/or IME_DFS_BUDGET=8192 -- final ncand=%d)\n", st.ncand);
	}
#else
	printf("(host is not x86 -- no rdtsc; cycle measurement skipped, only correctness gates ran)\n");
#endif

	/* ---- summary ----------------------------------------------------- */
	printf("\n%d checks, %d failed\n", checks, fails);
	printf(fails ? "FAILED\n" : "all checks passed\n");
	free(dat);
	return fails ? 1 : 0;
}
