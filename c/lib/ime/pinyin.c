/* c/lib/ime/pinyin.c -- the pinyin input method engine. See pinyin.h for the
 * public API and the reentrancy/global-state contract.
 *
 * FREESTANDING: no libc, no allocator, integer-only, like c/lib/gfx. Every
 * buffer here is either a fixed-size field of struct ime_dict/struct
 * ime_state (caller-owned) or a local on a bounded-depth stack frame -- see
 * the segmentation section below for the one place that matters (recursion
 * depth <= IME_MAX_RAW).
 *
 * THE THREE LOOKUP TABLES, and what "binary search" means for each:
 *   1. g_dict.key_off[]     -- the DICTIONARY's 25,945 keys, built once by
 *      ime_open() into a byte-offset index (pinyin.dat's records are
 *      variable-length and NUL-delimited, so nothing shorter than a full
 *      index supports random access at all -- see pinyin.h's header note).
 *   2. g_pinyin_syllables[] -- the 414-entry LEGAL SYLLABLE table
 *      (pinyin_syllables.inc), fixed at compile time, used ONLY to decide
 *      where the segmenter is allowed to cut -- never to fetch candidates.
 * Both are sorted, both are searched the same way (halve the range, compare
 * a NUL-terminated string against a length-bounded target), and neither
 * lookup is "the" per-key cost alone -- see recompute()'s comment for what a
 * full ime_feed() actually walks.
 */

#include "pinyin.h"
#include "pinyin_fmt.h"
#include "pinyin_syllables.inc"

/* ---- tiny freestanding helpers: no libc, so these are written here ---- */

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

/* ---- dictionary key lookup: binary search over the byte-offset index --- */

/* <0 if the dict key at `off` sorts before the target, 0 if equal, >0 if after. */
static int key_cmp(const struct ime_dict *d, uint32_t off, const char *key, int keylen) {
	const uint8_t *p = d->base + off;
	for (int i = 0; i < keylen; i++) {
		uint8_t pc = p[i];
		if (pc == 0) return -1; /* dict key is a strict prefix of target */
		unsigned char kc = (unsigned char)key[i];
		if (pc != kc) return pc < kc ? -1 : 1;
	}
	return p[keylen] == 0 ? 0 : 1; /* target is a strict prefix of dict key */
}

/* Returns the index into d->key_off[] of an EXACT match, or -1. This is the
 * one true "binary search" the brief asks for: O(log key_count) calls to
 * key_cmp, each O(keylen) (keylen <= IME_MAX_RAW, and in practice a handful
 * of bytes -- a syllable or two). */
static int32_t dict_find_key(const struct ime_dict *d, const char *key, int keylen) {
	if (!d || keylen <= 0) return -1;
	int32_t lo = 0, hi = (int32_t)d->key_count - 1;
	while (lo <= hi) {
		int32_t mid = lo + (hi - lo) / 2;
		int c = key_cmp(d, d->key_off[mid], key, keylen);
		if (c == 0) return mid;
		if (c < 0) lo = mid + 1;
		else hi = mid - 1;
	}
	return -1;
}

struct cand_cursor {
	const uint8_t *p;
	uint16_t remaining;
};

static void cand_iter_init(const struct ime_dict *d, uint32_t key_index, struct cand_cursor *cur) {
	const uint8_t *p = d->base + d->key_off[key_index];
	while (*p) p++;
	p++; /* skip the key string's NUL */
	uint16_t ncand = (uint16_t)(p[0] | (p[1] << 8));
	cur->p = p + 2;
	cur->remaining = ncand;
}

static int cand_iter_next(struct cand_cursor *cur, const uint8_t **bytes, int *blen) {
	if (cur->remaining == 0) return 0;
	uint16_t len = (uint16_t)(cur->p[0] | (cur->p[1] << 8));
	*bytes = cur->p + 2;
	*blen = len;
	cur->p += 2 + len;
	cur->remaining--;
	return 1;
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

/* ---- candidate list assembly: dedupe + rank (tier 0 before tier 1) ----- */

static int cand_equal(const struct ime_candidate *a, const struct ime_candidate *b) {
	if (a->ncp != b->ncp) return 0;
	for (int i = 0; i < a->ncp; i++)
		if (a->cp[i] != b->cp[i]) return 0;
	return 1;
}

static void cand_append(struct ime_state *st, const struct ime_candidate *c) {
	if (st->ncand >= IME_MAX_CAND || c->ncp == 0) return;
	for (int i = 0; i < st->ncand; i++)
		if (cand_equal(&st->cand[i], c)) return; /* dedupe: a tier-1 composition can equal an existing tier-0 candidate (e.g. "ni"+"hao" -> 你好, already present) */
	st->cand[st->ncand++] = *c;
}

/* Tier 0: the whole buffer as ONE dictionary key. Appended first, so its
 * candidates always outrank tier 1's -- "rank whole-word matches above
 * per-syllable concatenations" from the segmentation brief. */
static void append_whole_key_candidates(struct ime_state *st, const char *text, int len) {
	int32_t ki = dict_find_key(st->dict, text, len);
	if (ki < 0) return;
	struct cand_cursor cur;
	cand_iter_init(st->dict, (uint32_t)ki, &cur);
	const uint8_t *bytes;
	int blen;
	while (cand_iter_next(&cur, &bytes, &blen)) {
		struct ime_candidate c;
		c.ncp = utf8_decode(bytes, blen, c.cp, IME_CAND_MAXCP);
		cand_append(st, &c);
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
 * the SAME 32-candidate list. Tier 0 alone therefore already answers the
 * "xian" gate case with no segmentation involved at all, which is a fact
 * about THIS DICTIONARY's key construction, not a property of this
 * algorithm -- see the engine's shipping report for the case that actually
 * requires backtracking (a buffer where the longest first choice dead-ends
 * and no direct key rescues it, e.g. "angong": greedy commits "ang" then
 * cannot parse the "ong"/"ng" remainder at all -- 'ng' is deliberately not a
 * legal segment, see pinyin_syllables.inc -- while backtracking to "an" +
 * "gong" succeeds, and "angong" itself is not a dictionary key).
 *
 * APOSTROPHE ("xi'an") is an explicit forced cut: it never becomes part of
 * `letters`, and it marks forced[i] = 1 at the letter position it follows,
 * which the search treats as "no segment may have this position strictly
 * inside it". A forced cut ALSO disables tier 0 (see recompute()) --
 * otherwise "xi'an" would silently resurrect the single-syllable "xian"
 * reading the apostrophe exists to rule out.
 *
 * UNSEGMENTABLE BUFFERS ("nhao"): if no legal syllable starts at position 0
 * at all -- true for "nhao", since no syllable begins with the two-letter
 * onset "nh" and the bare fallback "n" is deliberately excluded from this
 * table (it is a real dictionary key, the interjection 嗯, but not a
 * composable segment; see pinyin_syllables.inc) -- the search returns zero
 * parses. Tier 0 also finds nothing ("nhao" is not a key). recompute()
 * leaves st->cand empty: NOT garbage, an explicit empty list, and
 * ime_commit(IME_COMMIT_RAW, ...) remains available to commit "nhao"
 * literally.
 *
 * IME_DFS_BUDGET bounds total recursive calls per recompute(), and
 * IME_MAX_PARSES bounds how many complete segmentations are kept. Both
 * exist for the same reason: recursion depth is naturally bounded by
 * IME_MAX_RAW (each call consumes >=1 letter), but the NUMBER of branches a
 * pathological buffer explores is not -- a buffer of many one-letter legal
 * syllables (e.g. repeated "a"/"e"/"o") backtracks combinatorially without a
 * cap. This runs on every keystroke in ring 0, so a hard budget rather than
 * an unbounded search is the point, not an afterthought; see the engine's
 * report for the measured worst-case cost against a buffer built exactly to
 * exercise this cap. */

/* Every segment is >= 1 letter, so IME_MAX_RAW all-1-letter segments is the
 * true worst case (measured: a first cut at 32 here silently zeroed the
 * candidate list for a full-length buffer of legal 1-letter syllables --
 * seg_dfs's `cur_n >= IME_MAX_SEG` cap hit at position 32 and could not
 * complete the remaining 32 letters, so no parse ever finished. Caught by
 * tests/unit/ime_test.c's worst-case cost measurement, not by inspection --
 * see the engine's report). Matching it to IME_MAX_RAW removes the cap from
 * the domain the buffer size itself already permits, rather than leaving a
 * second, smaller limit for a caller to discover independently. */
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

/* Recompute st->cand[]/st->ncand from st->raw[0..raw_len) from scratch.
 * Called on every letter/apostrophe/backspace -- see pinyin.h's cost note:
 * ONE dictionary probe (tier 0) is a true binary search; this additionally
 * runs the segmentation search above, bounded by IME_DFS_BUDGET, and for
 * every segment of every kept parse does ANOTHER binary-search probe (to
 * fetch that segment's own top candidate) -- so the honest description is
 * "a bounded number of binary searches", not "a single one". */
static void recompute(struct ime_state *st) {
	st->ncand = 0;
	st->page = 0;
	if (st->raw_len == 0) return;

	char letters[IME_MAX_RAW];
	uint8_t forced[IME_MAX_RAW];
	int has_apos = 0;
	int nletters = build_letters(st->raw, st->raw_len, letters, forced, IME_MAX_RAW, &has_apos);
	if (nletters == 0) return;

	if (!has_apos) {
		append_whole_key_candidates(st, letters, nletters);
	}

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
		if (p->nseg <= 1) continue; /* a single-segment parse duplicates tier 0's own lookup */

		struct ime_candidate composed;
		composed.ncp = 0;
		int ok = 1;
		for (int i = 0; i < p->nseg && ok; i++) {
			int32_t ki = dict_find_key(st->dict, letters + p->start[i], p->len[i]);
			if (ki < 0) {
				ok = 0; /* a legal syllable with no dictionary entry at all -- rare, see report */
				break;
			}
			struct cand_cursor cur;
			cand_iter_init(st->dict, (uint32_t)ki, &cur);
			const uint8_t *bytes;
			int blen;
			if (!cand_iter_next(&cur, &bytes, &blen)) {
				ok = 0;
				break;
			}
			int room = IME_CAND_MAXCP - composed.ncp;
			if (room <= 0) break;
			uint32_t tmp[IME_CAND_MAXCP];
			int tn = utf8_decode(bytes, blen, tmp, room);
			for (int j = 0; j < tn; j++) composed.cp[composed.ncp++] = tmp[j];
		}
		if (ok) cand_append(st, &composed);
	}
}

/* ---- public API --------------------------------------------------------- */

/* The ONE global this file keeps -- see pinyin.h's contract. Everything else
 * (struct ime_state, struct seg_search) is caller-owned or on-stack. */
static struct ime_dict g_dict;

const struct ime_dict *ime_open(const void *dat, size_t len) {
	if (!dat || len < 12) return 0;
	const uint8_t *base = (const uint8_t *)dat;
	if (base[0] != 'P' || base[1] != 'Y' || base[2] != 'N' || base[3] != 0) return 0;

	uint32_t version = (uint32_t)base[4] | ((uint32_t)base[5] << 8) |
	                    ((uint32_t)base[6] << 16) | ((uint32_t)base[7] << 24);
	if (version != PINYIN_VERSION) return 0;

	uint32_t key_count = (uint32_t)base[8] | ((uint32_t)base[9] << 8) |
	                      ((uint32_t)base[10] << 16) | ((uint32_t)base[11] << 24);
	if (key_count == 0 || key_count > IME_MAX_KEYS) return 0;

	g_dict.base = base;
	g_dict.len = (uint32_t)len;
	g_dict.key_count = key_count;

	/* One sequential pass over the WHOLE file, at load time only: builds
	 * the byte-offset index (see pinyin.h) and, as a side effect, validates
	 * that every record the header promises actually fits in `len` -- a
	 * truncated or corrupt file is refused here, once, rather than reading
	 * past the end on some later keystroke's binary search. */
	uint32_t off = 12;
	for (uint32_t i = 0; i < key_count; i++) {
		if (off >= len) return 0;
		g_dict.key_off[i] = off;
		while (off < len && base[off] != 0) off++;
		if (off >= len) return 0;
		off++; /* the key string's NUL */
		if (off + 2 > len) return 0;
		uint16_t ncand = (uint16_t)(base[off] | (base[off + 1] << 8));
		off += 2;
		for (uint16_t j = 0; j < ncand; j++) {
			if (off + 2 > len) return 0;
			uint16_t clen = (uint16_t)(base[off] | (base[off + 1] << 8));
			off += 2 + clen;
			if (off > len) return 0;
		}
	}
	return &g_dict;
}

void ime_reset(struct ime_state *st, const struct ime_dict *dict) {
	st->dict = dict;
	st->raw_len = 0;
	st->page = 0;
	st->ncand = 0;
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

int ime_commit(struct ime_state *st, int idx, uint32_t *out, int max) {
	if (idx == IME_COMMIT_RAW) {
		int n = 0;
		for (int i = 0; i < st->raw_len && n < max; i++)
			out[n++] = (uint32_t)(unsigned char)st->raw[i];
		return n;
	}
	if (idx < 0 || idx >= IME_PAGE_SIZE) return -1;
	int gi = st->page * IME_PAGE_SIZE + idx;
	if (gi < 0 || gi >= st->ncand) return -1;

	struct ime_candidate *c = &st->cand[gi];
	int n = c->ncp < max ? c->ncp : max;
	for (int i = 0; i < n; i++) out[i] = c->cp[i];
	return n;
}
