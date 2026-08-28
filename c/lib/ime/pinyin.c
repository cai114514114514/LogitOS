/* c/lib/ime/pinyin.c -- the pinyin input method engine. See pinyin.h for the
 * public API, the class-order contract and the reentrancy/global-state rules,
 * and pinyin_fmt.h for the on-disk format this file reads in place.
 *
 * FREESTANDING: no libc, no allocator, integer-only, like c/lib/gfx. Every
 * buffer here is either a fixed-size field of struct ime_dict/struct
 * ime_state (caller-owned) or a local on a bounded-depth stack frame -- see
 * the segmentation section (recursion depth <= IME_MAX_RAW) and the selection
 * section (struct sel, 1,164 bytes) for the two that matter.
 *
 * MEASURED, not estimated (clang -fstack-usage, the kernel's own flags):
 * recompute() is 5,144 bytes against v1's 5,080 -- SIXTY-FOUR bytes for three
 * candidate classes, because the compiler unions the three struct sel scopes
 * with the segmentation search that has already returned. seg_dfs is 72 bytes
 * a frame at a depth bounded by IME_MAX_RAW, so the worst case is 5,144 +
 * 64*72 = 9,752 against a 32,768-byte kernel stack (sched.c:27), and that
 * bound is v1's too. Re-run it before adding a buffer here; the number that
 * matters is not this file's, it is what sits below it on wm.c's stack.
 *
 * THE FOUR LOOKUP STRUCTURES, and what "binary search" means for each:
 *   1. g_dict.key_off[]     -- the DICTIONARY's 25,945 keys, built once by
 *      ime_open() into a byte-offset index (the key records are
 *      variable-length and NUL-delimited, so nothing shorter than a full
 *      index supports random access at all).
 *   2. g_pinyin_syllables[] -- the 414-entry LEGAL SYLLABLE table
 *      (pinyin_syllables.inc), fixed at compile time, used ONLY to decide
 *      where the segmenter is allowed to cut -- never to fetch candidates.
 *   3. the v2 INITIALS TABLE -- 5,850 fixed-stride rows, binary-searched IN
 *      PLACE. It needs no index array of its own and therefore no second
 *      IME_MAX_* bound; see pinyin_fmt.h note 1.
 *   4. the v2 REF ARRAY -- not searched at all. Buckets are contiguous and
 *      pre-sorted by descending frequency at build time, so an abbreviation
 *      lookup is one binary search over (3) and then a straight read.
 *
 * ONE THING THAT LOOKS LIKE AN OPTIMISATION AND IS THE CORRECTNESS ARGUMENT:
 * the prefix class sweeps a key range that reaches 2,272 keys / 3,189
 * candidates for a one-letter buffer, and IME_MAX_CAND is 96. The bound has to
 * be spent on the BEST candidates, not the first ones found -- an insertion
 * top-K, not a truncation. Truncating by arrival order is not a smaller
 * feature, it is a wrong one that looks identical from outside: measured on
 * the shipped dictionary, 你好 is at score-rank 49 of the 216 candidates under
 * prefix "ni" (on page 6, reachable) and at arrival-rank 121 (past the cap,
 * gone). The prune that makes the sweep cheap falls out of the same fact the
 * ranking rests on -- a key's candidates are stored in descending frequency,
 * so the first one that cannot beat the current cut-off ends that key.
 */

#include "pinyin.h"
#include "pinyin_fmt.h"
#include "pinyin_syllables.inc"

#ifdef IME_STATS
unsigned long ime_stat_keys, ime_stat_cands, ime_stat_userfn;
void ime_stat_reset(void) { ime_stat_keys = ime_stat_cands = ime_stat_userfn = 0; }
#define STAT(x) ((x)++)
#else
#define STAT(x) ((void)0)
#endif

/* ---- tiny freestanding helpers: no libc, so these are written here ---- */

static uint32_t ld16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t ld32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Saturating add. A user weight is caller-supplied; a wrap would turn the
 * best candidate into the worst, silently and only for the words somebody
 * actually uses. Saturation is wrong by at most one ordering among values
 * that are already four thousand times the dictionary's maximum. */
static uint32_t add_sat(uint32_t a, uint32_t b) {
	uint32_t s = a + b;
	return s < a ? 0xFFFFFFFFu : s;
}

static int utf8_decode(const uint8_t *s, int len, uint32_t *out, int outmax) {
	int n = 0, i = 0;
	while (i < len && n < outmax) {
		uint8_t b0 = s[i];
		uint32_t cp;
		int extra;
		if (b0 < 0x80) {
			cp = b0;
			extra = 0;
		} else if ((b0 & 0xE0) == 0xC0) {
			cp = b0 & 0x1F;
			extra = 1;
		} else if ((b0 & 0xF0) == 0xE0) {
			cp = b0 & 0x0F;
			extra = 2;
		} else if ((b0 & 0xF8) == 0xF0) {
			cp = b0 & 0x07;
			extra = 3;
		} else {
			i++; /* stray continuation/invalid lead byte: skip one byte and resync */
			continue;
		}
		if (i + extra >= len) {
			/* truncated sequence -- pinyin.dat is a trusted build artefact
			 * (tools/mkpinyin.py), so this should never fire; refuse the
			 * partial byte rather than read past `len`. */
			break;
		}
		uint32_t v = cp;
		int ok = 1;
		for (int k = 1; k <= extra; k++) {
			uint8_t bk = s[i + k];
			if ((bk & 0xC0) != 0x80) {
				ok = 0;
				break;
			}
			v = (v << 6) | (uint32_t)(bk & 0x3F);
		}
		if (!ok) {
			i++;
			continue;
		}
		out[n++] = v;
		i += extra + 1;
	}
	return n;
}

/* ---- record accessors -------------------------------------------------
 *
 * A candidate record is u16 nbytes, u32 freq, then nbytes of UTF-8
 * (PINYIN_CAND_HDR = 6). Every access is byte-wise -- the file is read where
 * it landed, and nothing here assumes an alignment the loader never promised. */

static uint32_t cand_bytes(const struct ime_dict *d, uint32_t coff) { return ld16(d->base + coff); }
static uint32_t cand_freq(const struct ime_dict *d, uint32_t coff) { return ld32(d->base + coff + 2); }
static const uint8_t *cand_text(const struct ime_dict *d, uint32_t coff) {
	return d->base + coff + PINYIN_CAND_HDR;
}

/* Byte offset of a key's FIRST candidate record, and its candidate count. */
static uint32_t key_cands(const struct ime_dict *d, uint32_t koff, uint32_t *ncand) {
	uint32_t p = koff;
	while (d->base[p]) p++;
	p++; /* the key string's NUL */
	*ncand = ld16(d->base + p);
	return p + 2;
}

static int key_len(const struct ime_dict *d, uint32_t koff) {
	int n = 0;
	while (d->base[koff + (uint32_t)n]) n++;
	return n;
}

/* ---- dictionary key lookup: binary search over the byte-offset index --- */

/* Compare the dict key at `off` against `t` treated as a PREFIX:
 *   <0  the key sorts before every string beginning with t
 *    0  the key BEGINS WITH t (it may be longer)
 *   >0  the key sorts after
 * One comparator serves both the exact lookup and the prefix range, which is
 * why a single pair of binary searches yields both: among all keys sharing a
 * prefix, the key EQUAL to it sorts first, so the range's low end is either
 * the exact match or proof there is none. */
static int pfx_cmp(const struct ime_dict *d, uint32_t off, const char *t, int tlen) {
	const uint8_t *p = d->base + off;
	for (int i = 0; i < tlen; i++) {
		uint8_t pc = p[i];
		if (pc == 0) return -1; /* dict key is a strict prefix of the target */
		unsigned char tc = (unsigned char)t[i];
		if (pc != tc) return pc < tc ? -1 : 1;
	}
	return 0;
}

/* [*lo, *hi) = every key index whose key begins with t. Two binary searches
 * over the SAME array v1 already had -- fact 1 of the brief, and the reason
 * incremental prefix needed no new data structure. */
static void prefix_range(const struct ime_dict *d, const char *t, int tlen,
                         int32_t *lo, int32_t *hi) {
	int32_t a = 0, b = (int32_t)d->key_count;
	while (a < b) {
		int32_t mid = a + (b - a) / 2;
		if (pfx_cmp(d, d->key_off[mid], t, tlen) < 0) a = mid + 1;
		else b = mid;
	}
	*lo = a;
	b = (int32_t)d->key_count;
	while (a < b) {
		int32_t mid = a + (b - a) / 2;
		if (pfx_cmp(d, d->key_off[mid], t, tlen) <= 0) a = mid + 1;
		else b = mid;
	}
	*hi = a;
}

/* Index of an EXACT key match, or -1. */
static int32_t dict_find_key(const struct ime_dict *d, const char *key, int keylen) {
	if (!d || keylen <= 0) return -1;
	int32_t lo, hi;
	prefix_range(d, key, keylen, &lo, &hi);
	if (lo < hi && d->base[d->key_off[lo] + (uint32_t)keylen] == 0) return lo;
	return -1;
}

/* Which key owns the candidate record at `coff`: the last key whose entry
 * starts at or before it. Used only by the abbreviation class, whose refs
 * point at records directly, and only for candidates that survive the cut --
 * never once per examined ref. */
static int32_t key_index_of(const struct ime_dict *d, uint32_t coff) {
	int32_t lo = 0, hi = (int32_t)d->key_count - 1, r = 0;
	while (lo <= hi) {
		int32_t mid = lo + (hi - lo) / 2;
		if (d->key_off[mid] <= coff) { r = mid; lo = mid + 1; }
		else hi = mid - 1;
	}
	return r;
}

/* ---- the initials table: binary search over fixed-stride rows, in place -- */

/* Exact bucket match only. A PREFIX search over initials was considered and
 * refused: "nh" as a prefix covers every bucket from "nha" to "nhz", hundreds
 * of them, for a class that is already the widest pool in the engine -- and
 * the requirement is `nh` -> 你好, not `n` -> everything two-syllable. */
static int32_t ini_find(const struct ime_dict *d, const char *s, int slen) {
	uint32_t field = d->ini_stride - PINYIN_INI_TAIL;
	if (slen < 2 || (uint32_t)slen >= field) return -1;
	int32_t lo = 0, hi = (int32_t)d->ini_count - 1;
	while (lo <= hi) {
		int32_t mid = lo + (hi - lo) / 2;
		const uint8_t *row = d->base + d->ini_off + (uint32_t)mid * d->ini_stride;
		int c = 0;
		for (uint32_t i = 0; i < field; i++) {
			uint8_t a = row[i];
			uint8_t b = i < (uint32_t)slen ? (uint8_t)s[i] : 0;
			if (a != b) { c = a < b ? -1 : 1; break; }
		}
		if (c == 0) return mid;
		if (c < 0) lo = mid + 1;
		else hi = mid - 1;
	}
	return -1;
}

/* ---- syllable legality: binary search over the fixed 414-entry table --- */

static int syl_cmp(const char *dict_syl /* NUL-terminated */, const char *s, int slen) {
	for (int i = 0; i < slen; i++) {
		unsigned char dc = (unsigned char)dict_syl[i];
		if (dc == 0) return -1;
		unsigned char sc = (unsigned char)s[i];
		if (dc != sc) return dc < sc ? -1 : 1;
	}
	return dict_syl[slen] == 0 ? 0 : 1;
}

static int is_legal_syllable(const char *s, int len) {
	if (len <= 0 || len > 6) return 0;
	int lo = 0, hi = PINYIN_SYLLABLE_COUNT - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		int c = syl_cmp(g_pinyin_syllables[mid], s, len);
		if (c == 0) return 1;
		if (c < 0) lo = mid + 1;
		else hi = mid - 1;
	}
	return 0;
}

/* ---- candidate list assembly ------------------------------------------ */

static int cand_equal(const struct ime_candidate *a, const struct ime_candidate *b) {
	if (a->ncp != b->ncp) return 0;
	for (int i = 0; i < a->ncp; i++)
		if (a->cp[i] != b->cp[i]) return 0;
	return 1;
}

/* Dedupe is by TEXT, not by source offset, and it has to be: a segmentation
 * composition can equal a real dictionary word ("ni"+"hao" -> 你好, already
 * present from the whole-key class), and those two have no offset in common
 * to compare. Cross-key duplicates cannot happen -- a word has exactly one
 * key -- so this loop is short in practice. */
static void cand_append(struct ime_state *st, const struct ime_candidate *c) {
	if (st->ncand >= IME_MAX_CAND || c->ncp == 0) return;
	for (int i = 0; i < st->ncand; i++)
		if (cand_equal(&st->cand[i], c)) return;
	st->cand[st->ncand++] = *c;
}

/* ---- the top-K selector ------------------------------------------------
 *
 * A score-descending, encounter-stable array of at most `cap` (byte offset,
 * score, key index) triples. 12 bytes an entry, so a full displacement moves
 * 1,152 bytes -- against 100 bytes per struct ime_candidate, which is why the
 * selection runs over offsets and decodes UTF-8 only for the survivors.
 *
 * `floor` is the current cut-off: the score an incoming candidate must BEAT,
 * or 0 while the array has room. Every class reads it to prune. */
struct sel {
	struct { uint32_t coff; uint32_t score; int32_t ki; } a[IME_MAX_CAND];
	int n;
	int cap;
	uint32_t floor;
};

static void sel_init(struct sel *s, int cap) {
	s->n = 0;
	s->cap = cap < 0 ? 0 : (cap > IME_MAX_CAND ? IME_MAX_CAND : cap);
	s->floor = 0;
}

static void sel_add(struct sel *s, uint32_t coff, uint32_t score, int32_t ki) {
	if (s->cap == 0) return;
	if (s->n == s->cap) {
		if (score <= s->a[s->cap - 1].score) return;
		s->n = s->cap - 1; /* drop the worst; the incoming one takes its place */
	}
	int i = s->n;
	/* strict `<`, so equal scores keep their arrival order and the result is
	 * a total, reproducible order rather than one that depends on the
	 * insertion path */
	while (i > 0 && s->a[i - 1].score < score) { s->a[i] = s->a[i - 1]; i--; }
	s->a[i].coff = coff;
	s->a[i].score = score;
	s->a[i].ki = ki;
	s->n++;
	s->floor = s->n == s->cap ? s->a[s->cap - 1].score : 0;
}

/* The best score a candidate with this base frequency could possibly reach,
 * given what the installed store declared about itself. Monotone in `base`,
 * which is the whole property the prune rests on: within a key the records
 * descend by frequency, so if the bound on THIS one does not beat the cut-off
 * then no later one in that key can either.
 *
 * With no hook installed this is the identity and the prune is exact. */
static uint32_t score_bound(const struct ime_state *st, uint32_t base) {
	if (!st->user_fn) return base;
	uint64_t v = ((uint64_t)base * st->user_mul) >> 8;
	v += st->user_add;
	return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

/* The final score for one candidate: its frequency plus the store's bonus,
 * CLAMPED to the bound the store published rather than trusted to respect it.
 * Clamping is what keeps the prune sound in the face of a store that
 * under-declares: it then gets visibly weak learning instead of an engine
 * that silently drops the words the user chose most. */
static uint32_t score_of(const struct ime_state *st, uint32_t koff, uint32_t coff,
                         uint32_t base) {
	if (!st->user_fn) return base;
	const struct ime_dict *d = st->dict;
	STAT(ime_stat_userfn);
	uint32_t w = st->user_fn(st->user_ctx,
	                         (const char *)(d->base + koff), key_len(d, koff),
	                         cand_text(d, coff), (int)cand_bytes(d, coff), base);
	uint32_t cap = score_bound(st, base);
	uint32_t s = add_sat(base, w);
	return s > cap ? cap : s;
}

static void sel_emit(struct ime_state *st, const struct sel *s, int tier) {
	const struct ime_dict *d = st->dict;
	for (int i = 0; i < s->n; i++) {
		struct ime_candidate c;
		uint32_t coff = s->a[i].coff;
		c.ncp = utf8_decode(cand_text(d, coff), (int)cand_bytes(d, coff),
		                    c.cp, IME_CAND_MAXCP);
		c.tier = tier;
		c.score = s->a[i].score;
		c.src_key = d->key_off[s->a[i].ki];
		c.src_cand = coff;
		cand_append(st, &c);
	}
}

/* Offer every candidate of key index `ki` to the selector, stopping at the
 * first one that cannot make the cut. The stop is exact, not a heuristic:
 * within a key the records are stored in descending frequency, so if this
 * one's best possible score (base plus the largest weight the store can
 * return) does not beat the floor, no later one can either. */
static void offer_key(struct ime_state *st, struct sel *s, int32_t ki) {
	const struct ime_dict *d = st->dict;
	uint32_t koff = d->key_off[ki], ncand;
	uint32_t p = key_cands(d, koff, &ncand);
	STAT(ime_stat_keys);
	for (uint32_t j = 0; j < ncand; j++) {
		uint32_t base = ld32(d->base + p + 2);
		if (score_bound(st, base) <= s->floor) return;
		STAT(ime_stat_cands);
		sel_add(s, p, score_of(st, koff, p, base), ki);
		p += PINYIN_CAND_HDR + ld16(d->base + p);
	}
}

/* ---- segmentation: longest-syllable-first DFS with backtracking -------
 *
 * "nihao" -> ni+hao; "zhongguo" -> zhong+guo: the segment boundaries are not
 * given, so at each position the search tries the LONGEST legal syllable
 * first (matching how a person reads pinyin -- greedily) and, in the
 * default build, backtracks to a shorter one when the longest choice leaves
 * an unparseable remainder.
 *
 * "xian" -> xi+an OR xian (both legal) is the textbook ambiguity example,
 * and it is worth recording exactly how this dictionary answers it: pypinyin
 * phrase keys are built by CONCATENATING each character's toneless pinyin
 * with NO separator (tools/mkpinyin.py), so the single-syllable reading
 * ("xian", 先/现/县/...) and the two-syllable phrase ("xi"+"an" -> 西安) land
 * under the IDENTICAL dictionary key "xian" -- position 0 and position 8 of
 * the SAME 32-candidate list. The whole-key class therefore already answers
 * the "xian" gate case with no segmentation involved at all, which is a fact
 * about THIS DICTIONARY's key construction, not a property of this
 * algorithm -- see tests/ime.mk for the case that actually requires
 * backtracking (a buffer where the longest first choice dead-ends and no
 * direct key rescues it, e.g. "angong": greedy commits "ang" then cannot
 * parse the "ong"/"ng" remainder at all -- 'ng' is deliberately not a legal
 * segment, see pinyin_syllables.inc -- while backtracking to "an" + "gong"
 * succeeds, and "angong" itself is not a dictionary key).
 *
 * APOSTROPHE ("xi'an") is an explicit forced cut: it never becomes part of
 * `letters`, and it marks forced[i] = 1 at the letter position it follows,
 * which the search treats as "no segment may have this position strictly
 * inside it". A forced cut ALSO disables the whole-key, prefix and
 * abbreviation classes (see recompute()) -- all three read the buffer as one
 * undivided string, which is exactly the reading the apostrophe rules out.
 *
 * UNSEGMENTABLE BUFFERS ("nhao"): if no legal syllable starts at position 0
 * at all -- true for "nhao", since no syllable begins with the two-letter
 * onset "nh" and the bare fallback "n" is deliberately excluded from this
 * table (it is a real dictionary key, the interjection 嗯, but not a
 * composable segment; see pinyin_syllables.inc) -- the search returns zero
 * parses. No key begins with "nh" either, so prefix finds nothing, and no
 * initials bucket is "nhao". recompute() leaves st->cand empty: NOT garbage,
 * an explicit empty list, and ime_commit(IME_COMMIT_RAW, ...) remains
 * available to commit "nhao" literally.
 *
 * IME_DFS_BUDGET bounds total recursive calls per recompute(), and
 * IME_MAX_PARSES bounds how many complete segmentations are kept. Both
 * exist for the same reason: recursion depth is naturally bounded by
 * IME_MAX_RAW (each call consumes >=1 letter), but the NUMBER of branches a
 * pathological buffer explores is not -- a buffer of many one-letter legal
 * syllables (e.g. repeated "a"/"e"/"o") backtracks combinatorially without a
 * cap. This runs on every keystroke in ring 0, so a hard budget rather than
 * an unbounded search is the point, not an afterthought. */

/* Every segment is >= 1 letter, so IME_MAX_RAW all-1-letter segments is the
 * true worst case (measured: a first cut at 32 here silently zeroed the
 * candidate list for a full-length buffer of legal 1-letter syllables --
 * seg_dfs's `cur_n >= IME_MAX_SEG` cap hit at position 32 and could not
 * complete the remaining 32 letters, so no parse ever finished. Caught by
 * tests/unit/ime_test.c's worst-case cost measurement, not by inspection).
 * Matching it to IME_MAX_RAW removes the cap from the domain the buffer size
 * itself already permits, rather than leaving a second, smaller limit for a
 * caller to discover independently. */
#define IME_MAX_SEG IME_MAX_RAW
#define IME_MAX_PARSES 8
#define IME_DFS_BUDGET 8192

struct seg_parse {
	int start[IME_MAX_SEG];
	int len[IME_MAX_SEG];
	int nseg;
};

struct seg_search {
	const char *letters;
	int nletters;
	const uint8_t *forced;
	int budget;

	struct seg_parse parses[IME_MAX_PARSES];
	int nparses;

	int cur_start[IME_MAX_SEG];
	int cur_len[IME_MAX_SEG];
	int cur_n;
};

static void seg_dfs(struct seg_search *s, int pos) {
	if (s->nparses >= IME_MAX_PARSES) return;
	if (s->budget <= 0) return;
	s->budget--;

	if (pos == s->nletters) {
		struct seg_parse *p = &s->parses[s->nparses++];
		p->nseg = s->cur_n;
		for (int i = 0; i < s->cur_n; i++) {
			p->start[i] = s->cur_start[i];
			p->len[i] = s->cur_len[i];
		}
		return;
	}

	int maxlen = s->nletters - pos;
	if (maxlen > 6) maxlen = 6;

	for (int L = maxlen; L >= 1; L--) {
		if (s->cur_n >= IME_MAX_SEG) break;

		int crosses = 0;
		for (int k = pos; k < pos + L - 1; k++) {
			if (s->forced[k]) {
				crosses = 1;
				break;
			}
		}
		if (crosses) continue;
		if (!is_legal_syllable(s->letters + pos, L)) continue;

		s->cur_start[s->cur_n] = pos;
		s->cur_len[s->cur_n] = L;
		s->cur_n++;
		seg_dfs(s, pos + L);
		s->cur_n--;

#ifdef IME_NO_BACKTRACK
		/* THE NEGATIVE CONTROL: commit to the first (longest) syllable that
		 * matched and never try a shorter one at this position, whether or
		 * not the recursive call below it found anything. A dead end below
		 * this point is never revisited -- that is the entire difference
		 * from the default build. */
		return;
#endif
		if (s->nparses >= IME_MAX_PARSES) return;
	}
}

/* letters/forced are separated from struct ime_state so the raw buffer
 * (which stores apostrophes literally, for exact backspace behaviour -- see
 * pinyin.h) never has to be re-parsed inside the search itself. */
static int build_letters(const char *raw, int raw_len, char *letters, uint8_t *forced, int maxlen, int *has_apos) {
	int n = 0;
	*has_apos = 0;
	for (int i = 0; i < raw_len; i++) {
		char c = raw[i];
		if (c == '\'') {
			*has_apos = 1;
			if (n > 0) forced[n - 1] = 1;
			continue;
		}
		if (n >= maxlen) break;
		letters[n] = c;
		forced[n] = 0;
		n++;
	}
	return n;
}

/* IME_TIER_SEG. Its own function so the 4.7 KiB struct seg_search leaves the
 * stack before the selector's frame is built, rather than the two coexisting
 * in recompute()'s. A composed candidate's score is the MINIMUM of its
 * segments' frequencies -- the weakest link, which is the only one of the
 * obvious rules that does not let a common first syllable carry a rare
 * second one to the top. With at most IME_MAX_PARSES=8 compositions it
 * rarely decides anything; it is here so the class has a number like the
 * others rather than an order nothing can merge. */
static void tier_segmentation(struct ime_state *st, const char *letters, int nletters,
                              const uint8_t *forced) {
	struct seg_search s;
	s.letters = letters;
	s.nletters = nletters;
	s.forced = forced;
	s.nparses = 0;
	s.cur_n = 0;
	s.budget = IME_DFS_BUDGET;
	seg_dfs(&s, 0);

	for (int pi = 0; pi < s.nparses; pi++) {
		struct seg_parse *p = &s.parses[pi];
		if (p->nseg <= 1) continue; /* a single-segment parse duplicates the whole-key class */

		struct ime_candidate composed;
		composed.ncp = 0;
		composed.tier = IME_TIER_SEG;
		composed.src_key = 0;
		composed.src_cand = 0;
		uint32_t weakest = 0xFFFFFFFFu;
		int ok = 1;
		for (int i = 0; i < p->nseg && ok; i++) {
			int32_t ki = dict_find_key(st->dict, letters + p->start[i], p->len[i]);
			if (ki < 0) {
				ok = 0; /* a legal syllable with no dictionary entry at all */
				break;
			}
			uint32_t ncand;
			uint32_t coff = key_cands(st->dict, st->dict->key_off[ki], &ncand);
			if (ncand == 0) {
				ok = 0;
				break;
			}
			uint32_t f = cand_freq(st->dict, coff);
			if (f < weakest) weakest = f;
			int room = IME_CAND_MAXCP - composed.ncp;
			if (room <= 0) break;
			uint32_t tmp[IME_CAND_MAXCP];
			int tn = utf8_decode(cand_text(st->dict, coff), (int)cand_bytes(st->dict, coff),
			                     tmp, room);
			for (int j = 0; j < tn; j++) composed.cp[composed.ncp++] = tmp[j];
		}
		if (ok) {
			composed.score = weakest == 0xFFFFFFFFu ? 0 : weakest;
			cand_append(st, &composed);
		}
	}
}

/* Recompute st->cand[]/st->ncand from st->raw[0..raw_len) from scratch, one
 * candidate class at a time and in the order pinyin.h fixes as a contract.
 * Called on every letter/apostrophe/backspace. The honest cost is: ONE pair
 * of binary searches over key_off[] (which yields the whole-key match AND the
 * prefix range together), a bounded segmentation walk each of whose cuts is
 * itself a binary search against the 414-syllable table, a linear sweep of
 * the prefix range in which a key costs one string walk and one frequency
 * read before the cut-off ends it, and one binary search over the initials
 * table. */
static void recompute(struct ime_state *st) {
	st->ncand = 0;
	st->page = 0;
	if (st->raw_len == 0) return;

	char letters[IME_MAX_RAW];
	uint8_t forced[IME_MAX_RAW];
	int has_apos = 0;
	int nletters = build_letters(st->raw, st->raw_len, letters, forced, IME_MAX_RAW, &has_apos);
	if (nletters == 0) return;

	int32_t lo = 0, hi = 0, exact = -1;
	if (!has_apos) {
		prefix_range(st->dict, letters, nletters, &lo, &hi);
		if (lo < hi && st->dict->base[st->dict->key_off[lo] + (uint32_t)nletters] == 0)
			exact = lo;

		/* IME_TIER_KEY */
		if (exact >= 0) {
			struct sel s;
			sel_init(&s, IME_MAX_CAND);
			offer_key(st, &s, exact);
			sel_emit(st, &s, IME_TIER_KEY);
		}
	}

	/* IME_TIER_SEG */
	tier_segmentation(st, letters, nletters, forced);

	if (has_apos) return;
	/* A full budget ends the recompute. Without this an exhausted class still
	 * costs its whole sweep -- the prune never fires, because a selector with
	 * cap 0 has no floor to prune against, so "s" would score all 3,189
	 * candidates in its prefix range and discard every one. */
	if (st->ncand >= IME_MAX_CAND) return;

	/* IME_TIER_PRE */
	if (hi - lo > (exact >= 0 ? 1 : 0)) {
		struct sel s;
		sel_init(&s, IME_MAX_CAND - st->ncand);
		for (int32_t ki = lo; ki < hi; ki++) {
			if (ki == exact) continue;
			offer_key(st, &s, ki);
		}
		sel_emit(st, &s, IME_TIER_PRE);
	}

	/* IME_TIER_ABBR */
	if (st->ncand < IME_MAX_CAND) {
		int32_t b = ini_find(st->dict, letters, nletters);
		if (b < 0) return;
		const struct ime_dict *d = st->dict;
		uint32_t field = d->ini_stride - PINYIN_INI_TAIL;
		const uint8_t *row = d->base + d->ini_off + (uint32_t)b * d->ini_stride;
		uint32_t first = ld32(row + field);
		uint32_t cnt = ld16(row + field + 4);

		struct sel s;
		sel_init(&s, IME_MAX_CAND - st->ncand);
		for (uint32_t r = 0; r < cnt; r++) {
			uint32_t coff = ld32(d->base + d->ref_off + (first + r) * 4);
			uint32_t base = cand_freq(d, coff);
			/* Same exact stop as offer_key, for the same reason: a bucket is
			 * pre-sorted by descending frequency at build time. Note the
			 * key-index lookup happens only AFTER the stop, so it costs one
			 * binary search per candidate that survives the cut -- not one
			 * per ref in a bucket of up to 248. */
			if (score_bound(st, base) <= s.floor) break;
			STAT(ime_stat_cands);
			int32_t ki = key_index_of(d, coff);
			sel_add(&s, coff, score_of(st, d->key_off[ki], coff, base), ki);
		}
		sel_emit(st, &s, IME_TIER_ABBR);
	}
}

/* ---- public API --------------------------------------------------------- */

/* The ONE global this file keeps -- see pinyin.h's contract. Everything else
 * (struct ime_state, struct seg_search, struct sel) is caller-owned or
 * on-stack, and the learned-weight table is deliberately NOT here: it is
 * mutable, and this object is shared unlocked across every window. */
static struct ime_dict g_dict;

const struct ime_dict *ime_open(const void *dat, size_t len) {
	if (!dat || len < PINYIN_HDR_SIZE) return 0;
	if (len > 0xFFFFFFFFu) return 0;
	const uint8_t *b = (const uint8_t *)dat;
	uint32_t L = (uint32_t)len;

	if (b[0] != 'P' || b[1] != 'Y' || b[2] != 'N' || b[3] != 0) return 0;
	if (ld32(b + PINYIN_OFF_VERSION) != PINYIN_VERSION) return 0;

	uint32_t key_count  = ld32(b + PINYIN_OFF_KEYCOUNT);
	uint32_t cand_count = ld32(b + PINYIN_OFF_CANDCOUNT);
	uint32_t ini_count  = ld32(b + PINYIN_OFF_INICOUNT);
	uint32_t ini_off    = ld32(b + PINYIN_OFF_INIOFF);
	uint32_t ini_stride = ld32(b + PINYIN_OFF_INISTRIDE);
	uint32_t ini_maxlen = ld32(b + PINYIN_OFF_INIMAXLEN);
	uint32_t ref_count  = ld32(b + PINYIN_OFF_REFCOUNT);
	uint32_t ref_off    = ld32(b + PINYIN_OFF_REFOFF);

	if (key_count == 0 || key_count > IME_MAX_KEYS) return 0;

	/* Header arithmetic first, before anything indexes off these numbers.
	 * Written as subtractions rather than `a + b > L` throughout -- the two
	 * are the same expression until the sum overflows, and then they are
	 * opposites (c/kernel/module/modelf.c makes the same argument at
	 * length). */
	if (ini_stride < PINYIN_INI_STRIDE_MIN || ini_stride > PINYIN_INI_STRIDE_MAX) return 0;
	if (ini_maxlen + 1 + PINYIN_INI_TAIL != ini_stride) return 0;
	if (ini_off < PINYIN_HDR_SIZE || ini_off > L) return 0;
	if (ini_count == 0 || ini_count > (L - ini_off) / ini_stride) return 0;
	if (ref_off > L || ref_off - ini_off < ini_count * ini_stride) return 0;
	if (ref_count == 0 || ref_count > (L - ref_off) / 4) return 0;

	g_dict.base = b;
	g_dict.len = L;
	g_dict.key_count = key_count;
	g_dict.cand_count = cand_count;
	g_dict.ini_off = ini_off;
	g_dict.ini_count = ini_count;
	g_dict.ini_stride = ini_stride;
	g_dict.ini_maxlen = ini_maxlen;
	g_dict.ref_off = ref_off;
	g_dict.ref_count = ref_count;
	g_dict.build_id = ld32(b + PINYIN_OFF_BUILDID);

	/* (1) One sequential pass over the key section, at load time only. It
	 * builds the byte-offset index and proves that every record the header
	 * promises fits BELOW ini_off -- so a truncated or corrupt file is
	 * refused here, once, rather than reading into the initials table on
	 * some later keystroke's binary search. Two checks v1 did not make:
	 * the walked candidate total must equal cand_count, and the section
	 * must end EXACTLY at ini_off. A header that under-counts is how a ref
	 * array sized from the header ends up shorter than the entries it has
	 * to cover. */
	uint32_t off = PINYIN_HDR_SIZE, ncands = 0;
	for (uint32_t i = 0; i < key_count; i++) {
		if (off >= ini_off) return 0;
		g_dict.key_off[i] = off;
		while (off < ini_off && b[off] != 0) off++;
		if (off >= ini_off) return 0;
		off++; /* the key string's NUL */
		if (ini_off - off < 2) return 0;
		uint32_t ncand = ld16(b + off);
		off += 2;
		for (uint32_t j = 0; j < ncand; j++) {
			if (ini_off - off < PINYIN_CAND_HDR) return 0;
			uint32_t clen = ld16(b + off);
			if (ini_off - off - PINYIN_CAND_HDR < clen) return 0;
			off += PINYIN_CAND_HDR + clen;
			ncands++;
		}
	}
	if (off != ini_off) return 0;
	if (ncands != cand_count) return 0;

	/* (2) Keys must be strictly ascending. v1 never checked this and relied
	 * on the generator; every lookup in this file is a binary search, and a
	 * binary search over an unsorted array does not fail -- it misses, and a
	 * missing key is indistinguishable from a word the dictionary does not
	 * have. key_count-1 comparisons beside a 967 KB scan already happening. */
	for (uint32_t i = 1; i < key_count; i++) {
		const uint8_t *p = b + g_dict.key_off[i - 1];
		const uint8_t *q = b + g_dict.key_off[i];
		uint32_t k = 0;
		while (p[k] && p[k] == q[k]) k++;
		if (p[k] >= q[k]) return 0; /* equal or descending: both refused */
	}

	/* (3) The initials table: NUL-terminated inside its field, NUL-PADDED
	 * after that (padding is what makes a fixed-width compare equal a
	 * lexicographic one -- without it the binary search is over a different
	 * order than the generator sorted by), at least two letters, strictly
	 * ascending, and buckets that TILE the ref array from 0 with no gap and
	 * no overlap. A gap is unreachable refs; an overlap is one bucket
	 * serving another's candidates. Neither has any symptom but a shorter
	 * or wrong candidate list. */
	uint32_t field = ini_stride - PINYIN_INI_TAIL;
	uint32_t tiled = 0;
	for (uint32_t i = 0; i < ini_count; i++) {
		const uint8_t *row = b + ini_off + i * ini_stride;
		uint32_t sl = 0;
		while (sl < field && row[sl]) sl++;
		if (sl >= field || sl < 2) return 0;
		for (uint32_t k = sl; k < field; k++)
			if (row[k]) return 0;
		if (i) {
			const uint8_t *prev = row - ini_stride;
			uint32_t k = 0;
			while (k < field && prev[k] == row[k]) k++;
			if (k == field || prev[k] > row[k]) return 0;
		}
		uint32_t first = ld32(row + field);
		uint32_t cnt = ld16(row + field + 4);
		if (cnt == 0) return 0;
		if (first != tiled) return 0;
		if (ref_count - first < cnt) return 0;
		tiled = first + cnt;
	}
	if (tiled != ref_count) return 0;

	/* (4) Every ref must name a candidate record that lies wholly inside the
	 * key section. A BOUND, not a proof that it lands on a record boundary:
	 * proving that needs the set of all 35,374 record offsets and ring 0 has
	 * no allocator to hold one. The generator guarantees boundaries, this
	 * catches truncation and garbage, and ime_ui.c's U+4E00..U+9FFF refusal
	 * is the backstop that turns a mid-record ref into a refused character
	 * rather than a wrong one. */
	for (uint32_t i = 0; i < ref_count; i++) {
		uint32_t r = ld32(b + ref_off + i * 4);
		if (r < PINYIN_HDR_SIZE || r > ini_off) return 0;
		if (ini_off - r < PINYIN_CAND_HDR) return 0;
		if (ini_off - r - PINYIN_CAND_HDR < ld16(b + r)) return 0;
	}

	return &g_dict;
}

void ime_reset(struct ime_state *st, const struct ime_dict *dict) {
	st->dict = dict;
	st->raw_len = 0;
	st->page = 0;
	st->ncand = 0;
	st->user_fn = 0;
	st->user_ctx = 0;
	st->user_mul = 256;
	st->user_add = 0;
}

void ime_set_user_weight(struct ime_state *st, ime_user_weight_fn fn,
                         void *ctx, uint32_t max_mul_q8, uint32_t max_add) {
	st->user_fn = fn;
	st->user_ctx = fn ? ctx : 0;
	st->user_mul = fn && max_mul_q8 > 256 ? max_mul_q8 : 256;
	st->user_add = fn ? max_add : 0;
}

int ime_feed(struct ime_state *st, int ch) {
	if (!st->dict) return IME_FEED_IGNORED;

	if (ch >= 'a' && ch <= 'z') {
		if (st->raw_len >= IME_MAX_RAW) return IME_FEED_IGNORED;
		st->raw[st->raw_len++] = (char)ch;
		recompute(st);
		return IME_FEED_COMPOSING;
	}

	if (ch == '\'') {
		if (st->raw_len == 0 || st->raw_len >= IME_MAX_RAW) return IME_FEED_IGNORED;
		if (st->raw[st->raw_len - 1] == '\'') return IME_FEED_IGNORED; /* no doubled separator */
		st->raw[st->raw_len++] = '\'';
		recompute(st);
		return IME_FEED_COMPOSING;
	}

	if (ch == 8) { /* backspace */
		if (st->raw_len == 0) return IME_FEED_IGNORED;
		st->raw_len--;
		recompute(st);
		return st->raw_len == 0 ? IME_FEED_EMPTY : IME_FEED_COMPOSING;
	}

	if (ch == 27) { /* escape */
		if (st->raw_len == 0 && st->ncand == 0) return IME_FEED_IGNORED;
		st->raw_len = 0;
		st->page = 0;
		st->ncand = 0;
		return IME_FEED_CANCELLED;
	}

	if (ch == IME_KEY_PGDN) {
		if (st->raw_len == 0) return IME_FEED_IGNORED;
		int npages = (st->ncand + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
		if (npages == 0) npages = 1;
		if (st->page + 1 >= npages) return IME_FEED_IGNORED; /* refuse past the last page */
		st->page++;
		return IME_FEED_PAGED;
	}

	if (ch == IME_KEY_PGUP) {
		if (st->raw_len == 0) return IME_FEED_IGNORED;
		if (st->page == 0) return IME_FEED_IGNORED; /* refuse before the first page */
		st->page--;
		return IME_FEED_PAGED;
	}

	return IME_FEED_IGNORED;
}

int ime_candidates(const struct ime_state *st, struct ime_candidate *out, int max) {
	if (st->raw_len == 0) return -1;
	int start = st->page * IME_PAGE_SIZE;
	int n = 0;
	for (int i = start; i < st->ncand && i < start + IME_PAGE_SIZE && n < max; i++)
		out[n++] = st->cand[i];
	return n;
}

/* Page-relative index -> absolute, or -1. */
static int cand_index(const struct ime_state *st, int idx) {
	if (idx < 0 || idx >= IME_PAGE_SIZE) return -1;
	int gi = st->page * IME_PAGE_SIZE + idx;
	if (gi < 0 || gi >= st->ncand) return -1;
	return gi;
}

int ime_commit(struct ime_state *st, int idx, uint32_t *out, int max) {
	if (idx == IME_COMMIT_RAW) {
		int n = 0;
		for (int i = 0; i < st->raw_len && n < max; i++)
			out[n++] = (uint32_t)(unsigned char)st->raw[i];
		return n;
	}
	int gi = cand_index(st, idx);
	if (gi < 0) return -1;

	struct ime_candidate *c = &st->cand[gi];
	int n = c->ncp < max ? c->ncp : max;
	for (int i = 0; i < n; i++) out[i] = c->cp[i];
	return n;
}

int ime_commit_source(const struct ime_state *st, int idx,
                      const char **key, int *keylen,
                      const uint8_t **cand_utf8, int *cand_len) {
	int gi = cand_index(st, idx);
	if (gi < 0) return 0;
	const struct ime_candidate *c = &st->cand[gi];
	if (c->src_cand == 0) return 0; /* a composed candidate has no single source */
	const struct ime_dict *d = st->dict;
	if (key) *key = (const char *)(d->base + c->src_key);
	if (keylen) *keylen = key_len(d, c->src_key);
	if (cand_utf8) *cand_utf8 = cand_text(d, c->src_cand);
	if (cand_len) *cand_len = (int)cand_bytes(d, c->src_cand);
	return 1;
}
