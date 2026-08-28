/* Pinyin IME engine (c/lib/ime/pinyin.c) -- host gate.
 *
 * Links pinyin.c directly (no kernel, no vfs): loads the real shipped
 * fsroot/ime/pinyin.dat off disk exactly as tests/unit/ttf_test.c loads the
 * real shipped font, so every assertion below is against the dictionary
 * that actually ships, not a synthetic fixture.
 *
 * Build: make test-ime (tests/ime.mk)
 *
 * ---------------------------------------------------------------------------
 * SIX CONTROLS LIVE IN THIS FILE, and five of the six are compiled from these
 * same sources with one -D -- only IME_NO_BACKTRACK edits the code under test.
 * tests/ime.mk names them, runs each one, and pins the exact number of
 * assertions each must redden (9 / 10 / 5 / 12 / 3 / 1 of 108, measured twice
 * each on 2026-08-28). This block said FIVE while listing six defines for as
 * long as it stood, because IME_CTL_HONEST_CEILING arrived after the sentence
 * was written and reads like a sibling of IME_CTL_PROMOTE. It is not one: the
 * two differ in what they change (a store's POLICY against a store's
 * DECLARATION) and they redden disjoint assertions, which is exactly why they
 * are separate targets rather than one. The mechanism differs per control and
 * the reason is worth stating once here:
 *
 *   IME_NO_BACKTRACK        an ENGINE #ifdef (pinyin.c's seg_dfs) -- the only
 *                           one of the six that edits the code under test.  [9]
 *   IME_CTL_FLAT_FREQ       a DICTIONARY mutation applied to this process's own
 *   IME_CTL_BUCKET_KEYORDER copy of the loaded bytes, before ime_open(). The
 *                           shipped file is never touched. A mutation control
 *                           can ask "which stored FACT is this assertion
 *                           actually reading?" in a way a code #ifdef cannot.
 *                                                                     [10 / 5]
 *   IME_CTL_PROMOTE         a POLICY swap in the user-weight store below -- the
 *   IME_CTL_HONEST_CEILING  store is the test's, not the engine's, so a rival
 *                           policy needs no engine change at all. PROMOTE
 *                           changes what the store RETURNS; HONEST_CEILING
 *                           changes only what it DECLARES, and the engine
 *                           clamps to the declaration rather than to the
 *                           return -- so the two are one mechanism and two
 *                           controls.                                  [3 / 1]
 *   IME_CTL_TIER_BLIND      a READER change: cand_snapshot() re-sorts the
 *                           engine's own emitted list by score alone, which is
 *                           what "the candidate classes were merged" would look
 *                           like from outside.                            [12]
 *
 * A SEVENTH GATE IS NOT IN THIS FILE AND IS NAMED HERE ANYWAY, because it is
 * the one that makes the 108 pins below mean anything: test-ime-dat runs
 * `tools/mkpinyin.py --check` and demands fsroot/ime/pinyin.dat be byte-
 * identical to what the generator emits. Without it every number below is
 * pinned to a committed binary no tool reproduces, i.e. to a fixture.
 *
 * *** ime_open() RETURNS A SINGLETON (pinyin.c's `static struct ime_dict
 * g_dict`), SO TWO DICTIONARIES CANNOT BE HELD OPEN AT ONCE. *** A second
 * ime_open() silently repoints the first caller's pointer at the second file.
 * That is not a bug -- pinyin.h states it ("the ONE static dictionary object
 * this file keeps") -- but it cost a full afternoon during this gate's
 * construction: a scratch harness compared a mutated dictionary against the
 * shipped one, held both pointers, and reported the control as a NO-OP on four
 * of six buffers. It was comparing the shipped file with itself. Section 16
 * asserts the singleton by pointer so the next reader meets it as a check
 * rather than as a wrong number. Every control here mutates a copy, opens it
 * ALONE, and this file holds exactly one dictionary for its whole run.
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
#include "pinyin_fmt.h"
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

/* "FAIL:" WITH THE COLON, not "FAIL" alone: tests/ime.mk's negative controls
 * grep '^FAIL:' for an EXACT count, and the trailing summary line below
 * prints "FAILED\n" -- which starts with the four bytes "FAIL" too. A grep
 * pattern of bare '^FAIL' would count that summary line as an extra
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

/* ---------------------------------------------------------------------------
 * THE READER. Every assertion about candidate ORDER goes through this and not
 * through st->cand directly, for one reason: IME_CTL_TIER_BLIND has to be able
 * to substitute "the same candidates ranked by score alone" for "the candidates
 * ranked by class then score", and a control that only reaches half the
 * assertions is not a control.
 *
 * The paging group (section 5) deliberately does NOT use this -- it is testing
 * ime_candidates()/ime_feed(PGUP/PGDN) themselves, and routing those through a
 * test-side re-sort would be measuring the harness.
 */
struct clist {
	int n;
	char text[IME_MAX_CAND][IME_CAND_MAXCP * 4 + 1];
	int tier[IME_MAX_CAND];
	uint32_t score[IME_MAX_CAND];
	int ncp[IME_MAX_CAND];
};

static void cand_snapshot(const struct ime_state *st, struct clist *out) {
	struct ime_candidate c[IME_MAX_CAND];
	out->n = st->ncand;
	for (int i = 0; i < st->ncand; i++) c[i] = st->cand[i];
#ifdef IME_CTL_TIER_BLIND
	/* THE CONTROL: one list ranked by score, classes ignored. Insertion sort,
	 * strict `<`, so equal scores keep the engine's own order -- the control
	 * must differ from the engine ONLY in the class boundary, or the redden
	 * count stops meaning "the class contract is load-bearing". */
	for (int i = 1; i < out->n; i++) {
		struct ime_candidate k = c[i];
		int j = i - 1;
		while (j >= 0 && c[j].score < k.score) { c[j + 1] = c[j]; j--; }
		c[j + 1] = k;
	}
#endif
	for (int i = 0; i < out->n; i++) {
		utf8_encode(c[i].cp, c[i].ncp, out->text[i], sizeof out->text[i]);
		out->tier[i] = c[i].tier;
		out->score[i] = c[i].score;
		out->ncp[i] = c[i].ncp;
	}
}

static void compose(const struct ime_dict *d, const char *raw, struct ime_state *st,
                    struct clist *cl) {
	ime_reset(st, d);
	feed_str(st, raw);
	cand_snapshot(st, cl);
}

/* Rank of a UTF-8 candidate in the snapshot, or -1. */
static int rank_of(const struct clist *cl, const char *want) {
	for (int i = 0; i < cl->n; i++)
		if (strcmp(cl->text[i], want) == 0) return i;
	return -1;
}

/* Compare the first `n` entries of the snapshot against a NULL-terminated
 * vector, reporting the FIRST disagreement by index and both strings -- a
 * bare "the list differs" on a 32-entry pin is a finding nobody can act on. */
static int list_eq(const struct clist *cl, const char *const *want, int n, char *why, size_t whysz) {
	if (cl->n < n) {
		snprintf(why, whysz, "only %d candidates, wanted at least %d", cl->n, n);
		return 0;
	}
	for (int i = 0; i < n; i++)
		if (strcmp(cl->text[i], want[i]) != 0) {
			snprintf(why, whysz, "index %d: got %s, want %s", i, cl->text[i], want[i]);
			return 0;
		}
	snprintf(why, whysz, "%d entries, in order", n);
	return 1;
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

/* ---- byte access to the loaded dictionary, for the mutation controls and
 *      for section 15's refusal cases. Mirrors pinyin.c's own ld16/ld32 rather
 *      than casting a struct over the mapping, for the reason pinyin_fmt.h
 *      gives: the offsets ARE the format. */
static uint32_t d_ld16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t d_ld32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void d_st32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* IME_CTL_FLAT_FREQ: every candidate's u32 frequency becomes 1.
 *
 * This is EXACTLY v1's information content -- the file keeps its order and
 * loses its number -- and it is the control for pinyin_fmt.h's thesis. It is
 * 1 and not 0 on purpose: at 0 every score ties the selector's initial floor
 * of 0, offer_key()'s `score_bound(base) <= s->floor` fires on the first
 * record of every key, and the engine returns an empty list everywhere. That
 * is not "v1's ranking", it is no ranking at all, and a control that empties
 * the machine measures nothing. */
#ifdef IME_CTL_FLAT_FREQ
static void mut_flat_freq(uint8_t *b) {
	uint32_t kc = d_ld32(b + PINYIN_OFF_KEYCOUNT);
	uint32_t ini_off = d_ld32(b + PINYIN_OFF_INIOFF);
	uint32_t off = PINYIN_HDR_SIZE;
	for (uint32_t i = 0; i < kc && off < ini_off; i++) {
		while (off < ini_off && b[off]) off++;
		off++;
		uint32_t nc = d_ld16(b + off);
		off += 2;
		for (uint32_t j = 0; j < nc; j++) {
			d_st32(b + off + 2, 1);
			off += PINYIN_CAND_HDR + d_ld16(b + off);
		}
	}
}
#endif

#ifdef IME_CTL_BUCKET_KEYORDER
static int cmp_u32(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : (x > y);
}

/* IME_CTL_BUCKET_KEYORDER: each initials bucket's refs sorted ascending by
 * byte offset -- which, because the key section is itself sorted by key, is
 * DICTIONARY KEY ORDER. That is what a generator emits if it appends refs as
 * it walks the keys and never sorts, i.e. the natural implementation
 * pinyin_fmt.h's "each bucket is pre-sorted by descending freq at BUILD time"
 * exists to rule out. */
static void mut_bucket_keyorder(uint8_t *b) {
	uint32_t ini_off = d_ld32(b + PINYIN_OFF_INIOFF), ini_n = d_ld32(b + PINYIN_OFF_INICOUNT);
	uint32_t stride = d_ld32(b + PINYIN_OFF_INISTRIDE), ref_off = d_ld32(b + PINYIN_OFF_REFOFF);
	uint32_t field = stride - PINYIN_INI_TAIL;
	for (uint32_t i = 0; i < ini_n; i++) {
		uint8_t *row = b + ini_off + i * stride;
		uint32_t first = d_ld32(row + field), cnt = d_ld16(row + field + 4);
		uint32_t *tmp = (uint32_t *)malloc(cnt * 4);
		for (uint32_t k = 0; k < cnt; k++) tmp[k] = d_ld32(b + ref_off + (first + k) * 4);
		qsort(tmp, cnt, 4, cmp_u32);
		for (uint32_t k = 0; k < cnt; k++) d_st32(b + ref_off + (first + k) * 4, tmp[k]);
		free(tmp);
	}
}
#endif

/* ---- the learned-weight store ------------------------------------------
 *
 * It is the TEST'S store, not the engine's: pinyin.h keeps the table outside
 * struct ime_dict precisely so a mutable thing never lands in the object every
 * window shares unlocked. So a rival ranking policy is a change here and
 * nowhere else, which is what makes IME_CTL_PROMOTE a two-line control.
 *
 * IME_USER_UNIT is 256 because that is the unit pinyin.h documents and
 * predicts a trajectory for. IME_USER_CEIL is the ceiling this store DECLARES
 * to ime_set_user_weight(), and it is the same number in every build below --
 * a control that changed both the policy and the declaration at once could
 * not say which of the two the reddened assertion was about. */
#define IME_USER_UNIT 256u
#define IME_USER_CEIL (8u * IME_USER_UNIT)

struct wstore {
	const char *key;   /* dictionary key, NUL-terminated here */
	const char *text;  /* candidate UTF-8, NUL-terminated here */
	int commits;       /* how many times the user has chosen it */
};

static unsigned long store_calls;

static uint32_t store_bonus(void *ctx, const char *key, int keylen,
                            const uint8_t *cand_utf8, int cand_len, uint32_t base) {
	(void)base;
	struct wstore *s = (struct wstore *)ctx;
	store_calls++;
	if ((int)strlen(s->key) != keylen || memcmp(key, s->key, (size_t)keylen) != 0) return 0;
	if ((int)strlen(s->text) != cand_len || memcmp(cand_utf8, s->text, (size_t)cand_len) != 0) return 0;
#ifdef IME_CTL_PROMOTE
	/* THE CONTROL: promote-to-front. One commit is as good as fifty -- the
	 * store returns its whole budget the moment the count is non-zero. This
	 * is the plausible wrong implementation, not a broken one: it "works" on
	 * every single-commit demo. pinyin.h names the shape that separates it
	 * from an accumulator, and section 13 asserts that shape. */
	return s->commits > 0 ? 0xFFFFFFFFu : 0;
#else
	return (uint32_t)s->commits * IME_USER_UNIT;
#endif
}

/* A store that always returns zero. Its whole job is to make the engine PAY
 * for the hook (no prune, every candidate scored) while changing no answer --
 * section 14 uses it to make the prune observable without an engine #ifdef. */
static uint32_t store_zero(void *ctx, const char *key, int keylen,
                           const uint8_t *cand_utf8, int cand_len, uint32_t base) {
	(void)ctx; (void)key; (void)keylen; (void)cand_utf8; (void)cand_len; (void)base;
	store_calls++;
	return 0;
}

/* ---- pinned vectors, all measured against fsroot/ime/pinyin.dat ---------- */

/* The whole "nh" bucket, in order. 32 entries, every one two characters --
 * pinyin_fmt.h note 2: single-codepoint candidates are not in the initials
 * index at all, so "no abbreviation below two letters" is a property of the
 * FILE and A2 below is what checks the file still has it. */
static const char *const NH_ALL[32] = {
	"南海", "南湖", "女孩", "浓厚", "内河", "内涵", "男孩", "内讧", "你好",
	"呐喊", "怒火", "奈何", "脑海", "农户", "南航", "农行", "年后", "暖和",
	"恼火", "怒吼", "宁海", "内含", "年号", "内核", "年画", "耐寒", "能耗",
	"年华", "恼恨", "怒喝", "内行", "您好",
};
static const char *const N_PAGE0[9]    = {"嗯","年","你","那","能","内","呢","女","南"};
static const char *const NI_PAGE0[9]   = {"你","泥","尼","拟","逆","腻","妮","霓","倪"};
static const char *const XIAN_PAGE0[9] = {"先","县","现","线","显","仙","弦","献","西安"};
/* zz = 248 refs and sj = 165, both past IME_MAX_CAND. They are here because
 * they are the ONLY buffers in this file where the build-time bucket sort is
 * load-bearing; see section 12. */
static const char *const ZZ_PAGE0[9]   = {"这种","组织","政治","增长","战争","作战","之中","真正","坐在"};
static const char *const SJ_PAGE0[9]   = {"世界","时间","世纪","设计","实际","书记","事件","省级","实践"};
static const char *const NIH_ALL[2]    = {"你好","泥灰岩"};
static const char *const NIHA_ALL[2]   = {"你哈","你好"};
static const char *const NIHAO_ALL[2]  = {"你好","你哈哦"};
static const char *const BEIJING_6[6]  = {"北京","背景","被经","北京市","北京大学","北京城"};
static const char *const JINI_3[3]     = {"及你","几年","纪念"};

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "fsroot/ime/pinyin.dat";
	long len = 0;
	char *dat = load_file(path, &len);

	/* The mutation controls act on THIS process's copy. The shipped file is
	 * opened read-only and never written by anything in this file. */
#if defined(IME_CTL_FLAT_FREQ)
	mut_flat_freq((uint8_t *)dat);
	printf("[control IME_CTL_FLAT_FREQ: every candidate frequency := 1]\n");
#elif defined(IME_CTL_BUCKET_KEYORDER)
	mut_bucket_keyorder((uint8_t *)dat);
	printf("[control IME_CTL_BUCKET_KEYORDER: initials buckets re-sorted into key order]\n");
#endif
#ifdef IME_CTL_TIER_BLIND
	printf("[control IME_CTL_TIER_BLIND: the reader ranks by score alone, classes ignored]\n");
#endif
#ifdef IME_CTL_PROMOTE
	printf("[control IME_CTL_PROMOTE: the learned store promotes to front instead of accumulating]\n");
#endif
#ifdef IME_CTL_HONEST_CEILING
	printf("[control IME_CTL_HONEST_CEILING: the 'declares nothing' store declares its real ceiling]\n");
#endif

	const struct ime_dict *dict = ime_open(dat, (size_t)len);
	if (!dict) {
		fprintf(stderr, "ime_open refused %s (%ld bytes) -- bad header?\n", path, len);
		return 1;
	}
	printf("loaded %s: %ld bytes, %u keys, %u candidates, %u initials rows, %u refs, build_id %08x\n",
	       path, len, dict->key_count, dict->cand_count, dict->ini_count,
	       dict->ref_count, dict->build_id);

	struct ime_state st;
	struct clist cl;
	char detail[512];

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

	cand_snapshot(&st, &cl);
	{
		snprintf(detail, sizeof detail, "candidate[0] = %s, ncand=%d",
		         cl.n ? cl.text[0] : "(none)", st.ncand);
		ck(cl.n > 0 && strcmp(cl.text[0], "你好") == 0,
		   "first candidate is 你好 (U+4F60 U+597D)", detail);
	}

	uint32_t commit_out[8];
	int cn = ime_commit(&st, 0, commit_out, 8);
	{
		snprintf(detail, sizeof detail, "got %d cp: %04X %04X", cn, cn > 0 ? commit_out[0] : 0, cn > 1 ? commit_out[1] : 0);
		ck(cn == 2 && commit_out[0] == 0x4F60 && commit_out[1] == 0x597D, "ime_commit(idx=0) yields U+4F60 U+597D", detail);
	}

	/* A second, segmentation-only candidate is expected too: "ni"+"ha"+"o"
	 * (你+哈+哦), since "niha" is not itself a legal syllable and "ha","o"
	 * are -- see pinyin.c's segmentation comment. Not asserted as REQUIRED
	 * by the brief, but its presence is what proves tier 1 ran at all
	 * rather than tier 0 alone; recorded here so a future change to either
	 * tier shows up as a diff instead of silence. */
	ck(rank_of(&cl, "你哈哦") >= 0,
	   "tier-1 segmentation candidate 你哈哦 (ni+ha+o) is also present", "");

	/* ---- 2. backspace restores the previous state EXACTLY -------------- */
	printf("\n-- backspace exactness --\n");
	{
		struct ime_state a, b;
		struct clist ca, cb;
		compose(dict, "niha", &a, &ca); /* stop one letter short of nihao */

		ime_reset(&b, dict);
		feed_str(&b, "nihao");
		int ret = ime_feed(&b, 8); /* backspace */
		ck(ret == IME_FEED_COMPOSING, "backspace after 'nihao' returns COMPOSING (buffer not empty)", "");
		ck(b.raw_len == 4 && memcmp(b.raw, "niha", 4) == 0, "raw buffer after backspace is exactly 'niha'", "");
		cand_snapshot(&b, &cb);

		int same = (ca.n == cb.n) && (a.ncand == b.ncand);
		for (int i = 0; same && i < ca.n; i++)
			if (strcmp(ca.text[i], cb.text[i]) != 0 || ca.score[i] != cb.score[i] ||
			    ca.tier[i] != cb.tier[i]) same = 0;
		snprintf(detail, sizeof detail, "typed-to-'niha' ncand=%d, backspaced-from-'nihao' ncand=%d", a.ncand, b.ncand);
		ck(same, "backspacing to 'niha' matches typing 'niha' directly, text/tier/score for text/tier/score", detail);

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
	compose(dict, "xian", &st, &cl);
	{
		int r_xian = rank_of(&cl, "先"), r_xian2 = rank_of(&cl, "西安");
		snprintf(detail, sizeof detail, "ncand=%d; 先 at %d, 西安 at %d (dictionary key \"xian\" alone has 32 candidates)",
		         st.ncand, r_xian, r_xian2);
		ck(r_xian == 0, "先 (single-syllable reading) is candidate 0", detail);
		ck(r_xian2 == 8, "西安 (xi+an reading) is candidate 8 -- the LAST slot of page 0", detail);
		ck(list_eq(&cl, XIAN_PAGE0, 9, detail, sizeof detail), "xian page 0 is 先 县 现 线 显 仙 弦 献 西安", detail);
	}
	/* THE 13-COUNT MARGIN, pinned as the number and not as an ordering.
	 * pinyin_fmt.h's whole argument for storing a RAW u32 rather than a
	 * quantised score is this pair: any bucketing coarse enough to save bytes
	 * is coarse enough to swap them, and it would do so silently. An assertion
	 * on the order alone cannot tell "still correct" from "correct by 13". */
	{
		int a = rank_of(&cl, "西安"), b = rank_of(&cl, "鲜");
		uint32_t sa = a >= 0 ? cl.score[a] : 0, sb = b >= 0 ? cl.score[b] : 0;
		snprintf(detail, sizeof detail, "西安=%u 鲜=%u, margin %d", sa, sb, (int)sa - (int)sb);
		ck(sa == 2576 && sb == 2563, "the page-0 boundary margin is 13 counts wide (西安 2576 over 鲜 2563)", detail);
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
	compose(dict, "xi'an", &st, &cl);
	{
		snprintf(detail, sizeof detail, "ncand=%d, candidate[0]=%s", st.ncand, cl.n ? cl.text[0] : "(none)");
		ck(rank_of(&cl, "先") < 0, "xi'an does NOT surface the single-syllable 先 reading", detail);
		ck(st.ncand >= 1, "xi'an still produces at least one segmentation-composed candidate", detail);
		/* An apostrophe disables TIER_KEY, TIER_PRE and TIER_ABBR, so every
		 * surviving candidate must be TIER_SEG -- the one class that reads the
		 * buffer as divided. Nothing checked this before; "先 is absent" is
		 * satisfied just as well by an empty list. */
		int all_seg = st.ncand > 0;
		for (int i = 0; i < cl.n; i++) if (cl.tier[i] != IME_TIER_SEG) all_seg = 0;
		ck(all_seg, "every xi'an candidate is TIER_SEG -- the other three classes are off, not merely outranked", detail);
	}

	/* ---- 4. "nhao": zero candidates, raw letters still committable ----- */
	printf("\n-- nhao (unsegmentable) --\n");
	compose(dict, "nhao", &st, &cl);
	ck(st.ncand == 0, "\"nhao\" produces zero candidates", "");
	{
		struct ime_candidate page[IME_PAGE_SIZE];
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
	feed_str(&st, "xian"); /* 96 candidates -- 11 pages at IME_PAGE_SIZE=9 */
	int npages = (st.ncand + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
	{
		snprintf(detail, sizeof detail, "ncand=%d -> %d pages", st.ncand, npages);
		ck(npages >= 2, "xian's candidate list spans more than one page", detail);
	}
	int paged = 0;
	while (ime_feed(&st, IME_KEY_PGDN) == IME_FEED_PAGED) paged++;
	{
		snprintf(detail, sizeof detail, "reached page %d of %d, paged forward %d times", st.page, npages, paged);
		ck(st.page == npages - 1, "paging forward stops exactly at the last page", detail);
		ck(paged == npages - 1, "the number of successful PGDN feeds equals npages-1", detail);
	}
	ck(ime_feed(&st, IME_KEY_PGDN) == IME_FEED_IGNORED, "one more PGDN past the last page is IGNORED, not wrapped or crashed", "");
	ck(st.page == npages - 1, "page index is unchanged by the refused PGDN", "");
	int back = 0;
	while (ime_feed(&st, IME_KEY_PGUP) == IME_FEED_PAGED) back++;
	{
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

	/* ---- 7. the negative-control witnesses (see tests/ime.mk for why
	 *         "xian" itself cannot serve as one) --------------------------- */
	printf("\n-- backtracking witnesses (angong / jini / xier) --\n");
	{
		static const char *const want[3] = {"按共", "及你", "西而"};
		static const char *const bufs[3] = {"angong", "jini", "xier"};
		static const char *const label[3] = {
			"angong -> 按共 (an+gong; greedy commits \"ang\" then dead-ends on \"ong\"/\"ng\")",
			"jini -> 及你 (ji+ni; greedy commits \"jin\" then dead-ends on \"i\")",
			"xier -> 西而 (xi+er; greedy commits \"xie\" then dead-ends on \"r\")",
		};
		for (int i = 0; i < 3; i++) {
			compose(dict, bufs[i], &st, &cl);
			snprintf(detail, sizeof detail, "ncand=%d, rank %d", st.ncand, rank_of(&cl, want[i]));
			ck(rank_of(&cl, want[i]) >= 0, label[i], detail);
			/* none of these three strings is a dictionary key itself --
			 * confirmed offline against fsroot/ime/pinyin.dat -- so this
			 * candidate can ONLY come from tier 1 (segmentation), and ONLY
			 * from the backtracked parse: greedy longest-first alone
			 * dead-ends on every one of them. */
		}
	}

	/* =====================================================================
	 * 8. ABBREVIATION: `nh` -> 你好. Requirement 1 of the brief.
	 *
	 * The pin is the whole 32-entry bucket in order, not "你好 appears". A
	 * containment check passes on a list of 5,000 and on a list ranked by
	 * nothing; the vector is what makes the ranking falsifiable, and it is
	 * what IME_CTL_BUCKET_KEYORDER and IME_CTL_FLAT_FREQ are aimed at.
	 * ===================================================================== */
	printf("\n-- abbreviation: nh --\n");
	compose(dict, "nh", &st, &cl);
	{
		snprintf(detail, sizeof detail, "ncand=%d", st.ncand);
		ck(st.ncand == 32, "\"nh\" produces exactly 32 candidates", detail);
	}
	{
		int two = 1, abbr = 1, bad = -1;
		for (int i = 0; i < cl.n; i++) {
			if (cl.ncp[i] != 2) { two = 0; if (bad < 0) bad = i; }
			if (cl.tier[i] != IME_TIER_ABBR) abbr = 0;
		}
		snprintf(detail, sizeof detail, "%d candidates, first non-2-codepoint at %d",
		         cl.n, bad);
		ck(two, "every \"nh\" candidate is exactly 2 codepoints -- a PHRASE, never a character", detail);
		ck(abbr, "every \"nh\" candidate is IME_TIER_ABBR (no key, no segmentation, no prefix begins with \"nh\")", detail);
	}
	ck(list_eq(&cl, NH_ALL, 32, detail, sizeof detail),
	   "the whole \"nh\" bucket, in frequency order: 南海 南湖 女孩 浓厚 内河 内涵 男孩 内讧 你好 ... 内行 您好", detail);
	ck(list_eq(&cl, NH_ALL, 9, detail, sizeof detail),
	   "\"nh\" page 0 is 南海 南湖 女孩 浓厚 内河 内涵 男孩 内讧 你好", detail);
	{
		int r = rank_of(&cl, "你好");
		snprintf(detail, sizeof detail, "%d/32, page %d slot %d", r, r / IME_PAGE_SIZE, r % IME_PAGE_SIZE + 1);
		ck(r == 8, "你好 is at index 8 -- the LAST slot of page 0, reachable with one keypress", detail);
	}
	{
		int r31 = rank_of(&cl, "您好"), r22 = rank_of(&cl, "年号");
		snprintf(detail, sizeof detail, "您好 at %d, 年号 at %d", r31, r22);
		ck(r31 == 31 && r22 == 22, "您好 is last (31) and 年号 is 22 -- the tail is ordered too, not merely present", detail);
	}
	/* The abbreviation class must not fire where no bucket exists, and the
	 * reason is structural rather than a rule the engine remembers: no pinyin
	 * syllable begins with 'i', so no initials string is ever "ni"; and
	 * "nha" is not a bucket because 你哈 is not a word. */
	{
		struct ime_state s2; struct clist c2;
		compose(dict, "nha", &s2, &c2);
		int nha0 = s2.ncand == 0;
		compose(dict, "ni", &s2, &c2);
		int ni_abbr = 0;
		for (int i = 0; i < c2.n; i++) if (c2.tier[i] == IME_TIER_ABBR) ni_abbr++;
		snprintf(detail, sizeof detail, "nha ncand=%d; ni has %d TIER_ABBR of %d", nha0 ? 0 : -1, ni_abbr, c2.n);
		ck(nha0 && ni_abbr == 0,
		   "\"nha\" yields nothing and \"ni\" yields no abbreviation -- the bucket \"ni\" cannot exist (no syllable begins with 'i')", detail);
	}

	/* The headline abbreviations, one line each. Requirement 3: PHRASES. */
	printf("\n-- abbreviation: the headline set --\n");
	{
		static const struct { const char *buf; const char *want; int ncp; } hs[] = {
			{"bj",      "北京",                  2},
			{"zg",      "中国",                  2},
			{"wm",      "我们",                  2},
			{"sj",      "世界",                  2},
			{"zhrmghg", "中华人民共和国",         7},
		};
		for (unsigned i = 0; i < sizeof hs / sizeof hs[0]; i++) {
			struct ime_state s2; struct clist c2;
			compose(dict, hs[i].buf, &s2, &c2);
			snprintf(detail, sizeof detail, "ncand=%d, candidate[0]=%s (%d cp)",
			         s2.ncand, c2.n ? c2.text[0] : "(none)", c2.n ? c2.ncp[0] : 0);
			char what[128];
			snprintf(what, sizeof what, "\"%s\" -> %s as candidate 0", hs[i].buf, hs[i].want);
			ck(c2.n > 0 && strcmp(c2.text[0], hs[i].want) == 0 && c2.ncp[0] == hs[i].ncp, what, detail);
		}
	}
	/* zz (248 refs) and sj (165) are the only buffers here whose bucket is
	 * larger than IME_MAX_CAND, which is the ONLY regime where the file's
	 * build-time bucket sort can be told from an arbitrary one -- see
	 * section 12 and tests/ime.mk's IME_CTL_BUCKET_KEYORDER note. */
	{
		struct ime_state s2; struct clist c2;
		compose(dict, "zz", &s2, &c2);
		ck(list_eq(&c2, ZZ_PAGE0, 9, detail, sizeof detail),
		   "\"zz\" page 0 is 这种 组织 政治 增长 战争 作战 之中 真正 坐在 (a 248-ref bucket, top-96 selected)", detail);
		compose(dict, "sj", &s2, &c2);
		ck(list_eq(&c2, SJ_PAGE0, 9, detail, sizeof detail),
		   "\"sj\" page 0 is 世界 时间 世纪 设计 实际 书记 事件 省级 实践 (a 165-ref bucket)", detail);
	}

	/* =====================================================================
	 * 9. INCREMENTAL PREFIX: the chain n / ni / nih / niha / nihao.
	 * Requirement 2 of the brief.
	 *
	 * *** THE RANK VECTOR IS NOT MONOTONE AND THE PIN SAYS SO. *** The
	 * design this gate was written against predicted {ABSENT, 49, 0, 0, 0}
	 * and called the vector "non-increasing -- improves monotonically, does
	 * not jump around". Measured, it is {ABSENT, 49, 0, 1, 0}: at "niha" the
	 * segmentation candidate 你哈 (ni+ha, score 2482, the weakest-link score
	 * of two very common characters) is TIER_SEG and 你好 is TIER_PRE, and
	 * the class contract puts SEG first however large the prefix candidate's
	 * frequency. 你好 goes 0 -> 1 -> 0 across the last three keystrokes.
	 *
	 * That is a real property of the shipped engine and dictionary, not a
	 * defect this gate is papering over, and pinning the smooth vector the
	 * design asked for would have made this file assert something false. The
	 * monotone claim survives in the weaker form B4 actually checks: from
	 * "nih" onward 你好 never leaves page 0.
	 * ===================================================================== */
	printf("\n-- incremental prefix: n / ni / nih / niha / nihao --\n");
	{
		static const char *const chain[5] = {"n", "ni", "nih", "niha", "nihao"};
		static const int want_n[5] = {96, 96, 2, 2, 2};
		static const int want_r[5] = {-1, 49, 0, 1, 0};
		int got_n[5], got_r[5];
		char nbuf[128] = "", rbuf[128] = "";
		for (int i = 0; i < 5; i++) {
			struct ime_state s2; struct clist c2;
			compose(dict, chain[i], &s2, &c2);
			got_n[i] = s2.ncand;
			got_r[i] = rank_of(&c2, "你好");
			char t[24];
			snprintf(t, sizeof t, "%d ", got_n[i]);  strcat(nbuf, t);
			snprintf(t, sizeof t, "%d ", got_r[i]);  strcat(rbuf, t);
		}
		int ok_n = 1, ok_r = 1;
		for (int i = 0; i < 5; i++) { if (got_n[i] != want_n[i]) ok_n = 0; if (got_r[i] != want_r[i]) ok_r = 0; }
		snprintf(detail, sizeof detail, "got { %s}, want { 96 96 2 2 2 }", nbuf);
		ck(ok_n, "ncand across n/ni/nih/niha/nihao is exactly { 96 96 2 2 2 } -- the list NARROWS as the buffer grows", detail);
		snprintf(detail, sizeof detail,
		         "got { %s}, want { -1 49 0 1 0 } (-1 = absent: true score-rank is 121 by arrival, past IME_MAX_CAND=96)", rbuf);
		ck(ok_r, "rank(你好) across the chain is exactly { ABSENT 49 0 1 0 }", detail);

		/* B4: the honest form of "it improves". Not monotonicity -- see the
		 * block comment -- but "once it is on page 0 it stays there", which
		 * is the property a person typing actually feels. */
		int stays = got_r[2] >= 0 && got_r[2] < IME_PAGE_SIZE &&
		            got_r[3] >= 0 && got_r[3] < IME_PAGE_SIZE &&
		            got_r[4] >= 0 && got_r[4] < IME_PAGE_SIZE;
		snprintf(detail, sizeof detail, "nih=%d niha=%d nihao=%d, page size %d", got_r[2], got_r[3], got_r[4], IME_PAGE_SIZE);
		ck(stays, "from the 3rd letter on, 你好 never leaves page 0 (it is NOT monotone: 0 -> 1 -> 0)", detail);
	}
	{
		struct ime_state s2; struct clist c2;
		compose(dict, "n", &s2, &c2);
		ck(list_eq(&c2, N_PAGE0, 9, detail, sizeof detail),
		   "one letter \"n\" already ranks usefully: 嗯 年 你 那 能 内 呢 女 南", detail);
		compose(dict, "ni", &s2, &c2);
		ck(list_eq(&c2, NI_PAGE0, 9, detail, sizeof detail),
		   "\"ni\" page 0 is the key's own 12 characters: 你 泥 尼 拟 逆 腻 妮 霓 倪", detail);
		compose(dict, "nih", &s2, &c2);
		ck(list_eq(&c2, NIH_ALL, 2, detail, sizeof detail),
		   "\"nih\" is exactly 你好 泥灰岩 -- two keys, neither of them \"nih\"", detail);
		compose(dict, "niha", &s2, &c2);
		ck(list_eq(&c2, NIHA_ALL, 2, detail, sizeof detail), "\"niha\" is exactly 你哈 你好", detail);
		compose(dict, "nihao", &s2, &c2);
		ck(list_eq(&c2, NIHAO_ALL, 2, detail, sizeof detail), "\"nihao\" is exactly 你好 你哈哦", detail);
		compose(dict, "beijing", &s2, &c2);
		ck(list_eq(&c2, BEIJING_6, 6, detail, sizeof detail),
		   "\"beijing\" is 北京 背景 被经 北京市 北京大学 北京城 -- two keyed readings, one composed, then prefix extensions", detail);
		compose(dict, "jini", &s2, &c2);
		ck(list_eq(&c2, JINI_3, 3, detail, sizeof detail),
		   "\"jini\" leads with the composed 及你, then the prefix extensions 几年 纪念", detail);
	}

	/* NESTING, in the only form that is true. Every candidate of "nih" is
	 * reachable from "ni" OR was cut by IME_MAX_CAND -- and the second half
	 * is not a hedge, it is the case that actually occurs: 泥灰岩 scores 275
	 * and "ni"'s 96th kept candidate scores 321, so it is 46 counts short of
	 * a list it structurally belongs in. Asserting bare containment would
	 * have been asserting something false. */
	{
		struct ime_state sa, sb; struct clist ca, cb;
		compose(dict, "nih", &sb, &cb);
		compose(dict, "ni", &sa, &ca);
		uint32_t floor = ca.n ? ca.score[ca.n - 1] : 0;
		int in = 0, cut = 0, bad = 0;
		char lost[128] = "";
		for (int i = 0; i < cb.n; i++) {
			if (rank_of(&ca, cb.text[i]) >= 0) in++;
			else if (cb.score[i] < floor) { cut++; snprintf(lost, sizeof lost, "%s(%u)", cb.text[i], cb.score[i]); }
			else bad++;
		}
		snprintf(detail, sizeof detail, "%d present, %d below ni's kept floor of %u [%s], %d unexplained",
		         in, cut, floor, lost, bad);
		ck(bad == 0 && cut == 1,
		   "every \"nih\" candidate is in \"ni\"'s list or was cut by IME_MAX_CAND -- exactly one (泥灰岩) was cut", detail);
	}

	/* =====================================================================
	 * 10. THE CLASS CONTRACT. pinyin.h fixes the order KEY < SEG < PRE <
	 * ABBR and calls it a contract rather than a preference. Two things are
	 * checked: that the emitted list actually obeys it, and -- section 11 --
	 * what it costs to break it.
	 * ===================================================================== */
	printf("\n-- the class contract --\n");
	{
		static const char *const bufs[] = {"n", "ni", "nih", "niha", "nihao", "xian", "beijing", "jini", "nh", "zz", 0};
		int tier_ok = 1, score_ok = 1;
		char tb[128] = "", sb2[128] = "";
		for (int i = 0; bufs[i]; i++) {
			struct ime_state s2; struct clist c2;
			compose(dict, bufs[i], &s2, &c2);
			for (int j = 1; j < c2.n; j++) {
				if (c2.tier[j] < c2.tier[j - 1] && !*tb)
					snprintf(tb, sizeof tb, "%s[%d]: tier %d after %d", bufs[i], j, c2.tier[j], c2.tier[j - 1]);
				if (c2.tier[j] < c2.tier[j - 1]) tier_ok = 0;
				if (c2.tier[j] == c2.tier[j - 1] && c2.score[j] > c2.score[j - 1]) {
					if (!*sb2) snprintf(sb2, sizeof sb2, "%s[%d]: %u after %u", bufs[i], j, c2.score[j], c2.score[j - 1]);
					score_ok = 0;
				}
			}
		}
		snprintf(detail, sizeof detail, "%s", *tb ? tb : "10 buffers, no inversion");
		ck(tier_ok, "the class index never decreases down the list (KEY < SEG < PRE < ABBR)", detail);
		snprintf(detail, sizeof detail, "%s", *sb2 ? sb2 : "10 buffers, no inversion");
		ck(score_ok, "within one class the score never increases down the list", detail);
	}

	/* =====================================================================
	 * 11. WHAT THE CLASS CONTRACT BUYS, computed rather than argued.
	 *
	 * This block builds the counterfactual the contract exists to rule out --
	 * the SAME 96 candidates ranked by score alone -- and asserts the two
	 * displacements pinyin.h predicts. It is the reason IME_CTL_TIER_BLIND is
	 * a control and not just a knob: under that build the reader IS this
	 * counterfactual, so the assertions in sections 3, 8 and 9 redden and the
	 * two below still pass. Both halves of the argument are on screen.
	 *
	 * pinyin.h's OTHER prediction -- "jini: prefix-first sends 及你 to rank
	 * 10" -- is NOT reproduced by a score merge, and that is measured here
	 * rather than quietly dropped: a composed candidate's score is the
	 * weakest link of its segments, 及你's is 49988, and that is the largest
	 * score in the whole "jini" list. Under a pure score merge 及你 stays at
	 * 0. Its rank-10 fate needs TIER_PRE to be tried FIRST, which is a
	 * different change from ignoring classes. The distinction matters: a
	 * reader who "fixes" the tier order by re-sorting will not see the jini
	 * regression the comment warns about.
	 * ===================================================================== */
	printf("\n-- the class contract, priced --\n");
	{
		struct ime_state s2;
		struct ime_candidate c[IME_MAX_CAND];
		ime_reset(&s2, dict);
		feed_str(&s2, "xian");
		for (int i = 0; i < s2.ncand; i++) c[i] = s2.cand[i];
		for (int i = 1; i < s2.ncand; i++) {          /* stable, score-descending */
			struct ime_candidate k = c[i]; int j = i - 1;
			while (j >= 0 && c[j].score < k.score) { c[j + 1] = c[j]; j--; }
			c[j + 1] = k;
		}
		int blind_xian = -1;
		char p0[256] = "";
		for (int i = 0; i < s2.ncand; i++) {
			char t[64]; utf8_encode(c[i].cp, c[i].ncp, t, sizeof t);
			if (i < 5) { strcat(p0, t); strcat(p0, " "); }
			if (strcmp(t, "西安") == 0) blind_xian = i;
		}
		snprintf(detail, sizeof detail, "score-only page 0 starts %s; 西安 falls 8 -> %d", p0, blind_xian);
		ck(blind_xian == 59,
		   "ignoring the classes would push 西安 from index 8 to 59 -- off page 0, six pages down", detail);

		struct clist cj; struct ime_state sj;
		compose(dict, "jini", &sj, &cj);
		int jr = rank_of(&cj, "及你");
		uint32_t js = jr >= 0 ? cj.score[jr] : 0, jmax = 0;
		for (int i = 0; i < cj.n; i++) if (cj.score[i] > jmax) jmax = cj.score[i];
		snprintf(detail, sizeof detail, "及你 score %u is the maximum of the 11 (%u), so a score merge leaves it at 0", js, jmax);
		ck(jr == 0 && js == jmax,
		   "jini is NOT a witness for a score merge: 及你 wins on score too (the tier order matters there for a different reason)", detail);
	}

	/* =====================================================================
	 * 12. THE SOURCE OF A COMMIT. ime_commit_source() is how a learned-weight
	 * store finds out what the user chose. TIER_SEG must refuse.
	 * ===================================================================== */
	printf("\n-- commit source --\n");
	{
		struct ime_state s2;
		ime_reset(&s2, dict);
		feed_str(&s2, "nh");
		const char *k = 0; int kl = 0; const uint8_t *t = 0; int tl = 0;
		int r = ime_commit_source(&s2, 8, &k, &kl, &t, &tl);
		snprintf(detail, sizeof detail, "r=%d key=\"%.*s\" (%d) text=\"%.*s\" (%d)",
		         r, kl, k ? k : "", kl, tl, t ? (const char *)t : "", tl);
		ck(r == 1 && kl == 5 && memcmp(k, "nihao", 5) == 0 && tl == 6 && memcmp(t, "你好", 6) == 0,
		   "committing \"nh\" slot 9 reports (key \"nihao\", text 你好) -- the abbreviation knows its own key", detail);

		ime_reset(&s2, dict);
		feed_str(&s2, "nihao");
		int r2 = ime_commit_source(&s2, 1, &k, &kl, &t, &tl);
		ck(r2 == 0, "ime_commit_source refuses the TIER_SEG candidate 你哈哦 -- a composition has no single source to learn from", "");

		ime_reset(&s2, dict);
		feed_str(&s2, "nihao");
		ck(ime_commit_source(&s2, 7, 0, 0, 0, 0) == 0, "ime_commit_source on an out-of-range index returns 0, not a stale pointer", "");
	}

	/* =====================================================================
	 * 13. THE LEARNED WEIGHT. Requirement 5 of the brief.
	 *
	 * The assertion is the TRAJECTORY, not the endpoint, and pinyin.h says
	 * why in one line: an accumulator reads 8 6 5 2 1 1 0 0 0 and a
	 * promote-to-front bug reads 8 0 0 0 0 0 0 0 0. Both end at 0. Only the
	 * shape tells them apart, and the plateau at 1,1 -- two commits that buy
	 * nothing because 南海 is 362 counts away -- is the part a wrong
	 * implementation cannot fake.
	 * ===================================================================== */
	printf("\n-- learned weight: 你好 climbing the nh bucket --\n");
	{
		struct wstore ws = {"nihao", "你好", 0};
		static const int want[9] = {8, 6, 5, 2, 1, 1, 0, 0, 0};
		int got[9];
		char tb[128] = "";
		for (int c = 0; c <= 8; c++) {
			ws.commits = c;
			struct ime_state s2;
			ime_reset(&s2, dict);
			ime_set_user_weight(&s2, store_bonus, &ws, 256, IME_USER_CEIL);
			feed_str(&s2, "nh");
			struct clist c2; cand_snapshot(&s2, &c2);
			got[c] = rank_of(&c2, "你好");
			char t[16]; snprintf(t, sizeof t, "%d ", got[c]); strcat(tb, t);
		}
		int ok = 1;
		for (int c = 0; c <= 8; c++) if (got[c] != want[c]) ok = 0;
		snprintf(detail, sizeof detail, "commits 0..8 -> { %s}, want { 8 6 5 2 1 1 0 0 0 }", tb);
		ck(ok, "an additive store (bonus = commits * 256) walks 你好 up the nh bucket 8 6 5 2 1 1 0 0 0", detail);
		snprintf(detail, sizeof detail, "got[4]=%d got[5]=%d got[6]=%d", got[4], got[5], got[6]);
		ck(got[4] == 1 && got[5] == 1 && got[6] == 0,
		   "the PLATEAU is present: two commits in a row buy no rank, then the third does -- an accumulator, not a promote-to-front", detail);
		snprintf(detail, sizeof detail, "got[1]=%d (a promote-to-front store reads 0 here)", got[1]);
		ck(got[1] == 6, "ONE commit moves 你好 by two places, not to the front", detail);

		/* The documented ceiling of an additive store, asserted so nobody
		 * proposes it as personalisation for the whole dictionary: raw
		 * frequencies span four orders of magnitude, so a fixed increment is
		 * a near-tie breaker and nothing else. */
		struct wstore w2 = {"ni", "泥", 500};
		struct ime_state s3;
		ime_reset(&s3, dict);
		ime_set_user_weight(&s3, store_bonus, &w2, 256, 500u * IME_USER_UNIT);
		feed_str(&s3, "ni");
		struct clist c3; cand_snapshot(&s3, &c3);
		snprintf(detail, sizeof detail, "泥 at %d after 500 commits (你 = 234587, 泥 = 4354 + 128000)", rank_of(&c3, "泥"));
		ck(rank_of(&c3, "泥") == 1,
		   "500 commits of 泥 still do not pass 你 -- an additive store is a tie-breaker, not a personaliser", detail);
	}

	/* THE DECLARED CEILING IS LOAD-BEARING, and the pair below is what shows
	 * it. Two states, the SAME store returning the SAME large bonus; they
	 * differ only in the (max_mul_q8, max_add) each declared. The engine
	 * clamps to the declaration rather than trusting the return, so the
	 * under-declaring one must be byte-identical to no store at all. */
	printf("\n-- the declared ceiling --\n");
	{
		struct wstore ws = {"nihao", "你好", 6};
		struct ime_state base, under, honest;
		struct clist cbase, cunder, chonest;
		compose(dict, "nh", &base, &cbase);

		ime_reset(&under, dict);
#ifdef IME_CTL_HONEST_CEILING
		/* THE CONTROL: the store that is supposed to be able to change
		 * nothing declares its real ceiling instead of zero. */
		ime_set_user_weight(&under, store_bonus, &ws, 256, IME_USER_CEIL);
#else
		ime_set_user_weight(&under, store_bonus, &ws, 256, 0);
#endif
		feed_str(&under, "nh");
		cand_snapshot(&under, &cunder);

		ime_reset(&honest, dict);
		ime_set_user_weight(&honest, store_bonus, &ws, 256, IME_USER_CEIL);
		feed_str(&honest, "nh");
		cand_snapshot(&honest, &chonest);

		int same = cbase.n == cunder.n;
		for (int i = 0; same && i < cbase.n; i++)
			if (strcmp(cbase.text[i], cunder.text[i]) || cbase.score[i] != cunder.score[i]) same = 0;
		snprintf(detail, sizeof detail, "under-declared rank(你好)=%d, no-store rank=%d, honest rank=%d",
		         rank_of(&cunder, "你好"), rank_of(&cbase, "你好"), rank_of(&chonest, "你好"));
		ck(same, "a store declaring (256, 0) changes NOTHING though it returns 1536 -- the engine clamps to the declaration", detail);
		snprintf(detail, sizeof detail, "same ctx, same bonus of 6*256=1536; declared 0 -> rank %d, declared %u -> rank %d",
		         rank_of(&cunder, "你好"), IME_USER_CEIL, rank_of(&chonest, "你好"));
		ck(rank_of(&chonest, "你好") == 0 && rank_of(&cbase, "你好") == 8,
		   "the SAME store declaring (256, 2048) takes 你好 to rank 0 -- the declaration, not the bonus, is what moved it", detail);
	}

	/* =====================================================================
	 * 14. COST, PORTABLY. pinyin.h asks for this by name: the only cost guard
	 * this engine ever had was an rdtsc block that prints "skipped" on
	 * darwin/arm64, the one host anybody develops on (CLAUDE.md's host-reality
	 * table, fifth shape -- a gate that cannot run where people work is a gate
	 * nobody watches). An operation count runs everywhere.
	 *
	 * AND IT MAKES THE PRUNE OBSERVABLE WITHOUT AN ENGINE #ifdef. A store that
	 * returns 0 for everything but declares a huge ceiling defeats
	 * `score_bound(base) <= floor` -- every candidate must now be scored,
	 * because any of them COULD come back big. The answers are identical; only
	 * the work changes. That difference IS the prune, measured.
	 * ===================================================================== */
	printf("\n-- cost: operation counts (portable; see pinyin.h IME_STATS) --\n");
#ifdef IME_STATS
	{
		static const struct { const char *buf; unsigned long budget; unsigned long full; } cases[] = {
			/* buffer  pruned budget   full sweep (= the range/bucket size) */
			{"s",        520,   3189},   /* 3,189 candidates under one letter */
			{"n",        300,    717},
			{"ni",       200,    216},
			{"xian",     220,    446},
			{"zz",       110,    248},   /* the largest initials bucket */
			{"sj",       110,    165},
			{"bj",       110,    108},
			{"nh",        40,     32},   /* under the cap: nothing to prune */
		};
		for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
			const char *b = cases[i].buf;
			int L = (int)strlen(b);
			struct ime_state a;
			ime_reset(&a, dict);
			for (int j = 0; j < L - 1; j++) ime_feed(&a, (unsigned char)b[j]);
			ime_stat_reset();
			ime_feed(&a, (unsigned char)b[L - 1]);
			unsigned long pruned = ime_stat_cands, pkeys = ime_stat_keys;
			unsigned long puser = ime_stat_userfn;

			struct ime_state z;
			ime_reset(&z, dict);
			ime_set_user_weight(&z, store_zero, 0, 256, 1000000000u);
			for (int j = 0; j < L - 1; j++) ime_feed(&z, (unsigned char)b[j]);
			ime_stat_reset();
			ime_feed(&z, (unsigned char)b[L - 1]);
			unsigned long full = ime_stat_cands;

			/* the answers must be identical -- a zero bonus is not a ranking */
			struct clist ca, cz;
			cand_snapshot(&a, &ca);
			cand_snapshot(&z, &cz);
			int same = ca.n == cz.n;
			for (int j = 0; same && j < ca.n; j++)
				if (strcmp(ca.text[j], cz.text[j]) || ca.score[j] != cz.score[j]) same = 0;

			char what[160];
			snprintf(what, sizeof what, "\"%s\": the last keystroke scores <= %lu of %lu candidates, and the hook changes no answer",
			         b, cases[i].budget, cases[i].full);
			snprintf(detail, sizeof detail, "pruned=%lu keys=%lu userfn=%lu, unpruned=%lu (%.0f%%), lists identical: %s",
			         pruned, pkeys, puser, full, full ? 100.0 * (double)pruned / (double)full : 0.0, same ? "yes" : "NO");
			ck(pruned <= cases[i].budget && full == cases[i].full && puser == 0 && same, what, detail);
		}
	}
#else
	printf("(built without -DIME_STATS -- operation counts unavailable; tests/ime.mk always defines it)\n");
	ck(0, "the cost group needs -DIME_STATS", "tests/ime.mk's IME_CF must define it");
#endif

	/* The cycle counter stays, but as a REPORT and never as a gate: it is
	 * silent on the development host by construction. */
	printf("\n-- cost: cycles (x86 only) --\n");
#if defined(__x86_64__) || defined(__i386__)
	{
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
		 * at every one of the 32 boundaries. */
		char adv[IME_MAX_RAW + 1];
		for (int i = 0; i < IME_MAX_RAW; i += 2) { adv[i] = 'a'; adv[i + 1] = 'o'; }
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
	printf("(host is not x86 -- no rdtsc. The operation counts above are the portable cost gate;\n");
	printf(" this block is a report on x86 and silent here, never a pass/fail either way.)\n");
#endif

	/* The adversarial buffer must also be CORRECT, not merely bounded --
	 * measured on every host, unlike the cycle count above. */
	{
		char adv[IME_MAX_RAW + 1];
		for (int i = 0; i < IME_MAX_RAW; i += 2) { adv[i] = 'a'; adv[i + 1] = 'o'; }
		adv[IME_MAX_RAW] = 0;
		ime_reset(&st, dict);
		feed_str(&st, adv);
		snprintf(detail, sizeof detail, "raw_len=%d ncand=%d", st.raw_len, st.ncand);
		ck(st.raw_len == IME_MAX_RAW, "a full 64-letter adversarial buffer fills raw[] exactly and does not overflow", detail);
		ck(ime_feed(&st, 'a') == IME_FEED_IGNORED, "the 65th letter is IGNORED, not written past IME_MAX_RAW", detail);
		ck(st.ncand >= 0 && st.ncand <= IME_MAX_CAND, "the adversarial buffer's candidate count stays inside IME_MAX_CAND", detail);
	}

	/* =====================================================================
	 * 15. WHAT ime_open() REFUSES. pinyin.h lists these refusals and gives
	 * one reason for all of them: a structure that is short, mis-sized or
	 * unsorted makes entries silently UNREACHABLE, and "fewer candidates"
	 * looks exactly like a correct answer. Each case below is its own
	 * control -- the mutation IS the injected fault, and a refusal that
	 * stopped happening reddens here with the byte named.
	 *
	 * A v1 dictionary is in this list on purpose. pinyin.h refuses a
	 * dual-path reader because a compatible one would let a v1 file ship with
	 * no frequencies and no initials -- abbreviation and prefix silently
	 * absent while every gate above stayed green on the classes v1 had.
	 * ===================================================================== */
	printf("\n-- ime_open refusals --\n");
	{
		uint8_t *copy = (uint8_t *)malloc((size_t)len);
		static const struct { const char *name; uint32_t off; int wide; uint32_t val; } m[] = {
			{"magic byte 0 flipped",         0,                    0, 'Q'},
			{"version = 1 (a v1 dictionary)", PINYIN_OFF_VERSION,  1, 1},
			{"version = 3 (from the future)", PINYIN_OFF_VERSION,  1, 3},
			{"key_count = 0",                PINYIN_OFF_KEYCOUNT,  1, 0},
			{"key_count > IME_MAX_KEYS",     PINYIN_OFF_KEYCOUNT,  1, IME_MAX_KEYS + 1},
			{"key_count one too small",      PINYIN_OFF_KEYCOUNT,  1, 25944},
			{"key_count one too large",      PINYIN_OFF_KEYCOUNT,  1, 25946},
			{"cand_count one too large",     PINYIN_OFF_CANDCOUNT, 1, 35375},
			{"ini_count = 0",                PINYIN_OFF_INICOUNT,  1, 0},
			{"ini_stride below the floor",   PINYIN_OFF_INISTRIDE, 1, PINYIN_INI_STRIDE_MIN - 1},
			{"ini_stride disagrees with ini_maxlen", PINYIN_OFF_INISTRIDE, 1, 23},
			{"ref_count = 0",                PINYIN_OFF_REFCOUNT,  1, 0},
			{"ref_count one too small (buckets no longer tile)", PINYIN_OFF_REFCOUNT, 1, 31111},
		};
		for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
			memcpy(copy, dat, (size_t)len);
			if (m[i].wide) d_st32(copy + m[i].off, m[i].val); else copy[m[i].off] = (uint8_t)m[i].val;
			char what[160];
			snprintf(what, sizeof what, "ime_open refuses: %s", m[i].name);
			ck(ime_open(copy, (size_t)len) == 0, what, "");
		}
		/* structural faults that no single header field expresses */
		{
			memcpy(copy, dat, (size_t)len);
			uint8_t tmp[PINYIN_INI_STRIDE_MAX];
			uint32_t io = d_ld32(copy + PINYIN_OFF_INIOFF), s = d_ld32(copy + PINYIN_OFF_INISTRIDE);
			memcpy(tmp, copy + io, s);
			memcpy(copy + io, copy + io + s, s);
			memcpy(copy + io + s, tmp, s);
			ck(ime_open(copy, (size_t)len) == 0,
			   "ime_open refuses: two initials rows swapped (binary search over an unsorted table MISSES, it does not fail)", "");
		}
		{
			memcpy(copy, dat, (size_t)len);
			uint32_t io = d_ld32(copy + PINYIN_OFF_INIOFF), ro = d_ld32(copy + PINYIN_OFF_REFOFF);
			d_st32(copy + ro, io + 4); /* a ref pointing past the key section */
			ck(ime_open(copy, (size_t)len) == 0, "ime_open refuses: a ref pointing outside the key section", "");
		}
		{
			/* One bucket claims one ref too many. Every later bucket's
			 * ref_first now disagrees with the running tile, which is an
			 * OVERLAP: bucket 0 serving one of bucket 1's candidates. The
			 * only symptom in a working engine would be one wrong word. */
			memcpy(copy, dat, (size_t)len);
			uint32_t io = d_ld32(copy + PINYIN_OFF_INIOFF), s = d_ld32(copy + PINYIN_OFF_INISTRIDE);
			uint8_t *row = copy + io + (s - PINYIN_INI_TAIL);
			uint32_t n = d_ld16(row + 4);
			row[4] = (uint8_t)(n + 1); row[5] = (uint8_t)((n + 1) >> 8);
			ck(ime_open(copy, (size_t)len) == 0,
			   "ime_open refuses: one bucket claiming an extra ref (the buckets no longer TILE the array)", "");
		}
		{
			memcpy(copy, dat, (size_t)len);
			ck(ime_open(copy, (size_t)len - 1) == 0, "ime_open refuses: the file one byte short", "");
		}
		ck(ime_open(copy, PINYIN_HDR_SIZE - 1) == 0, "ime_open refuses: a buffer smaller than the header", "");
		ck(ime_open(0, (size_t)len) == 0, "ime_open refuses: a NULL pointer", "");
		free(copy);

		/* EVERY refusal above left g_dict holding whatever the last successful
		 * open put there -- so re-open the real file before anything else runs.
		 * That is the singleton in section 16, met the hard way. */
		dict = ime_open(dat, (size_t)len);
		ck(dict != 0, "the shipped dictionary re-opens after the refusal sweep", "");
	}

	/* =====================================================================
	 * 16. THE SINGLETON. See this file's header comment: two dictionaries
	 * cannot be open at once, and the failure is silent -- the first
	 * pointer keeps working and starts answering about the second file.
	 * ===================================================================== */
	printf("\n-- the dictionary is a singleton --\n");
	{
		const struct ime_dict *a = ime_open(dat, (size_t)len);
		const struct ime_dict *b = ime_open(dat, (size_t)len);
		ck(a == b && a == dict,
		   "ime_open returns the SAME object every time -- a second open repoints the first caller, it does not fail", "");
		ck(a->key_count == 25945 && a->cand_count == 35374 && a->ini_count == 5850 && a->ref_count == 31112,
		   "the shipped dictionary is 25,945 keys / 35,374 candidates / 5,850 initials rows / 31,112 refs", "");
		/* pinyin_fmt.h note 2: single-codepoint candidates are deliberately
		 * NOT indexed, so ref_count counts exactly the multi-character
		 * candidates. The subtraction is the check -- it fails the moment a
		 * generator "helpfully" indexes single characters, which is the change
		 * that turns every first keystroke into an abbreviation avalanche and
		 * is otherwise visible only as a longer list. */
		snprintf(detail, sizeof detail, "%u indexed phrases + %u single characters = %u candidates",
		         a->ref_count, a->cand_count - a->ref_count, a->cand_count);
		ck(a->cand_count - a->ref_count == 4262,
		   "PHRASES are the majority (31,112) and single characters (4,262) are the ONLY unindexed candidates", detail);
	}

	/* ---- summary ----------------------------------------------------- */
	printf("\n%d checks, %d failed\n", checks, fails);
	printf(fails ? "FAILED\n" : "all checks passed\n");
	free(dat);
	return fails ? 1 : 0;
}
