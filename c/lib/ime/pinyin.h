#ifndef C_LIB_IME_PINYIN_H_
#define C_LIB_IME_PINYIN_H_

#include <stdint.h>
#include <stddef.h>

/* Pinyin input method engine -- segmentation, prefix, abbreviation and
 * candidate ranking over the dictionary tools/mkpinyin.py builds
 * (c/lib/ime/pinyin_fmt.h, loaded from fsroot/ime/pinyin.dat).
 *
 * FREESTANDING, LIKE c/lib/gfx: no libc, no allocator, integer-only. It is
 * called from the window manager in ring 0 on every keystroke while a
 * composition is active. What "cost per key" actually means, measured (see
 * tests/unit/ime_test.c and the report that shipped this file): a SINGLE
 * dictionary probe is a genuine binary search -- O(log key_count), ~15 string
 * comparisons against a 25,945-key table, built once at ime_open() into a
 * byte-offset index so the variable-length key records support random access
 * at all. A full ime_feed() is NOT "a binary search and nothing else" and this
 * header says so rather than pretend otherwise. It is now FOUR bounded walks,
 * one per candidate class (see IME_TIER_* below), and the honest description
 * of the cost is "a bounded number of binary searches plus one linear sweep of
 * a key range whose candidates are pruned against a running threshold".
 *
 * REENTRANT PER WINDOW: everything that changes while composing lives in a
 * caller-owned struct ime_state, so two windows can each hold an open
 * composition. The ONE exception, matching the brief exactly ("no global
 * state except the dictionary pointer"), is the validated dictionary index
 * itself: one process loads pinyin.dat once, and every window's ime_state
 * just points at the SAME struct ime_dict ime_open() returns. That index is
 * READ-ONLY after ime_open() succeeds, which is what makes sharing it across
 * states safe with no lock.
 *
 * *** THAT PROPERTY IS THE REASON THE USER-WEIGHT HOOK IS A CALLBACK. ***
 * A learned weight table is MUTABLE, and a mutable field inside struct
 * ime_dict would make two windows in ring 0 race over it with nothing here to
 * serialise them -- a corrupted count, not a crash, and no host gate could
 * ever see it. So the table does not live here at all: ime_set_user_weight()
 * installs a function pointer plus an opaque ctx on the STATE, the store owns
 * its own memory and its own locking discipline, and this file only ever adds
 * the number it is handed. See ime_user_weight_fn.
 */

/* ---- the dictionary -------------------------------------------------- */

/* IME_MAX_KEYS bounds the byte-offset index built at load time -- the fixed
 * budget this file's "no allocator" runs on (c/lib/text/glyphras.h's point
 * budget is the same pattern). Measured against the shipped dictionary:
 * 25,945 keys. 32768 is comfortable headroom for the dictionary to grow
 * without a source change; ime_open() REFUSES (returns NULL) rather than
 * silently truncating the table if a future dictionary exceeds it, because a
 * truncated key index would silently make some keys unreachable by lookup
 * with no signal that anything is wrong.
 *
 * The v2 initials table needs NO equivalent bound, and that is a property of
 * the format rather than luck: it is fixed-stride, so it is binary-searched in
 * place with no .bss array to size (pinyin_fmt.h note 1). */
#define IME_MAX_KEYS 32768

struct ime_dict {
	const uint8_t *base;   /* the mapped/loaded pinyin.dat bytes -- NOT copied, NOT owned */
	uint32_t len;
	uint32_t key_count;

	/* v2 section geometry, validated by ime_open(). All are byte offsets
	 * from `base` except the counts and the stride. */
	uint32_t cand_count;
	uint32_t ini_off, ini_count, ini_stride, ini_maxlen;
	uint32_t ref_off, ref_count;

	/* FNV-1a over the header and key section. NOT checked here -- ime_open
	 * has nothing to compare it against. It is published so a user-weight
	 * store can record WHICH dictionary it learned against; see
	 * pinyin_fmt.h note 3 for why a store that keys on byte offsets instead
	 * corrupts silently across a regeneration. */
	uint32_t build_id;

	/* key_off[i] = byte offset (from base) of the i-th key's entry (its
	 * NUL-terminated key string), in the SAME sorted order the file already
	 * stores keys in. Built once by ime_open() with one sequential pass;
	 * every lookup after that is binary search over this array, which is
	 * the only reason a variable-length, NUL-delimited record format can
	 * support O(log n) lookup at all. */
	uint32_t key_off[IME_MAX_KEYS];
};

/* Validate a pinyin.dat blob already resident at `dat`, build the byte-offset
 * index, and return a pointer to the ONE static dictionary object this file
 * keeps (see the "no global state" note above). Returns NULL on any refusal.
 *
 * WHAT IT REFUSES, and the rule every one of these follows: a structure that
 * is short, mis-sized or unsorted makes entries silently UNREACHABLE, and
 * "fewer candidates" looks exactly like a correct answer.
 *   - bad magic, version != 2, key_count of 0 or > IME_MAX_KEYS
 *   - a key section whose records do not all fit below ini_off, or whose
 *     candidate total disagrees with the header's cand_count, or that does
 *     not end EXACTLY at ini_off
 *   - an initials stride outside [PINYIN_INI_STRIDE_MIN, _MAX], or one that
 *     disagrees with ini_maxlen
 *   - an initials or ref table that does not fit in `len`
 *   - an initials row with no NUL inside its string field, or fewer than two
 *     letters, or out of ascending order (binary search over an unsorted
 *     table misses rows with no other symptom)
 *   - buckets that do not tile the ref array contiguously from 0 to ref_count
 *   - a ref that does not point at a candidate record wholly inside the key
 *     section
 *
 * The last one is a BOUND, not a proof that a ref lands on a record boundary:
 * proving that needs the set of all 35,374 record offsets, and ring 0 has no
 * allocator to hold one. Stated rather than implied -- the generator
 * guarantees boundaries, this check catches truncation and garbage, and
 * ime_ui.c's existing U+4E00..U+9FFF codepoint refusal is the backstop that
 * turns a mid-record ref into a refused character rather than a wrong one.
 *
 * Call once, e.g. from wm_run() after the dictionary file is read -- ime_open
 * never touches vfs/vmm itself, so how `dat` got resident is entirely the
 * caller's business. A v1 file is refused here, loudly and by version, and
 * ime_ui.c already prints a named refusal and degrades to ASCII passthrough:
 * there is deliberately no dual-path reader, because a compatible reader
 * would let a v1 dictionary ship with no frequencies and no initials, i.e.
 * with abbreviation and prefix silently absent while every gate stayed green. */
const struct ime_dict *ime_open(const void *dat, size_t len);

/* ---- per-composition state --------------------------------------------- */

/* Longest key observed in the shipped dictionary is 48 ASCII bytes (a whole
 * multi-word idiom's concatenated pinyin, no separators); IME_MAX_RAW gives
 * headroom above that for apostrophe separators a user might add. A
 * composition that reaches the cap simply stops accepting letters (ime_feed
 * returns IME_FEED_IGNORED) rather than overflowing -- nothing here mallocs. */
#define IME_MAX_RAW 64

/* Longest candidate in the shipped dictionary is 15 codepoints (a phrase,
 * "中华人民共和国全国人民代表大会"); 20 is headroom, not a guess -- measured
 * against fsroot/ime/pinyin.dat by the dictionary build and re-checked by
 * tests/unit/ime_test.c. */
#define IME_CAND_MAXCP 20

/* Candidates kept per composition, across all pages.
 *
 * THE DENSEST KEY IS "ji" WITH 66, NOT "zhong" -- this constant's comment
 * named the wrong key for as long as it stood (zhong has 13), and a reader
 * who used "zhong" to re-derive the number got a different one.
 *
 * 96 was chosen when the only classes were the whole-key lookup and a handful
 * of segmentation compositions. With prefix and abbreviation it is now a REAL
 * bound rather than headroom: the "ni" prefix range holds 216 candidates, "s"
 * holds 3,189, and the largest initials bucket ("zz") holds 248. So the
 * selection is a TOP-K, not a truncation of whatever arrived first -- see
 * recompute() in pinyin.c. Truncating by arrival order would drop a user's
 * best candidate while looking exactly like a correct short list. */
#define IME_MAX_CAND 96

/* Candidates per page. 9 on purpose: it is the conventional "press a digit
 * 1-9 to pick a candidate" IME page size, and it is also EXACTLY where the
 * shipped dictionary's own frequency order places the alternate reading in
 * the "xian" gate case (先 at candidate 0, 西安 at candidate 8) -- and that
 * pin is 13 frequency counts wide, 西安 at 2576 directly above 鲜 at 2563.
 * pinyin_fmt.h explains why v2 stores raw counts rather than a quantised
 * score; this is the pair that decided it. */
#define IME_PAGE_SIZE 9

/* THE CANDIDATE CLASSES, AND THEIR ORDER IS A CONTRACT, NOT A PREFERENCE.
 *
 * Candidates are assembled class by class and a later class can never
 * displace an earlier one, however large its frequency. Two live assertions
 * depend on each boundary, which is what makes this a contract:
 *
 *   TIER_KEY  the whole buffer as ONE dictionary key.
 *             Must outrank everything: merging the "xian" prefix range by
 *             raw frequency puts 向 想 现在 像 先 on page 0 and pushes 西安
 *             (index 8) off it, reddening a check that has nothing to do with
 *             prefix input.
 *   TIER_SEG  segmentation-composed, >= 2 segments.
 *             Must outrank prefix: "jini" is not a key, its tier-1 answer is
 *             及你, and it has 10 prefix extensions. Prefix-first sends 及你
 *             to rank 10 -- page 1 -- and the backtracking control's count
 *             goes from 4 to 3, which reads like the control weakening.
 *   TIER_PRE  a key that STRICTLY extends the buffer ("ni" -> "nian", "nihao").
 *   TIER_ABBR an initials-index bucket whose string EQUALS the buffer
 *             ("nh" -> 你好). Never fires below two letters, because
 *             single-codepoint candidates are not in the index at all
 *             (pinyin_fmt.h note 2) -- a structural property of the file, not
 *             a rule this code has to remember.
 *
 * Within a class: descending score, ties broken by encounter order (key index,
 * then position within the key), so the order is total and reproducible.
 *
 * TIER_PRE BEFORE TIER_ABBR, and the cost is stated rather than hidden: 38 of
 * the 5,850 initials strings are also real pinyin keys ("ba", "de", "ge",
 * ...). For exactly those buffers the prefix class can spend the whole budget
 * and the abbreviation reading is not shown. That is the right call -- a user
 * typing "de" wants 的, not a two-word phrase whose initials are d,e -- and it
 * costs the headline requirement nothing, because no pinyin syllable begins
 * with "nh", "bj" or "zg". */
enum {
	IME_TIER_KEY = 0,
	IME_TIER_SEG = 1,
	IME_TIER_PRE = 2,
	IME_TIER_ABBR = 3,
};

struct ime_candidate {
	uint32_t cp[IME_CAND_MAXCP];
	int ncp;

	int tier;        /* IME_TIER_*, the class this candidate came from */
	uint32_t score;  /* dictionary frequency + user weight; 0 for a composed one */

	/* Byte offsets into the dictionary of the key string and the candidate
	 * record this came from, or 0 for a TIER_SEG composition (which has no
	 * single source). ime_commit_source() reads them; a user-weight store
	 * needs them to know WHAT was just committed. */
	uint32_t src_key;
	uint32_t src_cand;
};

/* A learned weight -- a BONUS added to the dictionary's own frequency, in the
 * same units.
 *
 * Called during ranking with the dictionary key, the candidate's UTF-8 bytes,
 * and that candidate's base frequency. TEXT, never a byte offset, for the
 * reason pinyin_fmt.h note 3 gives. Neither string is NUL-terminated; both
 * lengths are exact. Must not block and must not mutate anything the
 * dictionary points at.
 *
 * `base` IS PASSED BECAUSE THE POLICY IS NOT THIS FILE'S TO CHOOSE, and the
 * two reasonable policies need different arithmetic. Measured on the shipped
 * dictionary:
 *   - an ADDITIVE store (bonus = commits * 256) lands 你好 at rank 0 of the
 *     "nh" bucket after 6 commits, with the trajectory 8 6 5 2 1 1 0 0 0 --
 *     the plateau at 1,1 is the shape that tells a real accumulator apart
 *     from a promote-to-front bug, which would read 8 0 0 0 0 0 0 0 0.
 *   - the same store cannot promote 泥 (4,354) over 你 (234,587) under the
 *     key "ni" in any plausible number of commits: raw frequencies span four
 *     orders of magnitude, so a fixed increment is a near-tie breaker and
 *     nothing else. A store that wants that needs bonus proportional to
 *     `base`, and without `base` it cannot compute one.
 *
 * WHY THE CEILING IS TWO NUMBERS AND NOT A COMMENT. The ranking prunes: once
 * a class's slice is full, a candidate whose score cannot beat the slice's
 * current minimum is skipped without the hook ever being called, and the
 * REMAINING candidates of that key are skipped with it -- sound only because
 * a key's records are stored in descending frequency. That prune needs a
 * monotone upper bound on the final score given the base, so the store
 * declares one: ime_set_user_weight()'s (max_mul_q8, max_add) mean
 *
 *     bound(base) = ((base * max_mul_q8) >> 8) + max_add
 *
 * which covers an additive store (max_mul_q8 = 256, max_add = the largest
 * bonus) and a proportional one (max_mul_q8 = 256 * the largest multiplier,
 * max_add = 0) with the same two integers. The engine CLAMPS every returned
 * bonus to that bound rather than trusting it, so a store that under-declares
 * gets visibly weak learning instead of an engine that silently drops the
 * words the user chose most. A generous bound costs only prune strength --
 * the sweep is bounded by the key range either way, never unbounded. */
typedef uint32_t (*ime_user_weight_fn)(void *ctx,
                                       const char *key, int keylen,
                                       const uint8_t *cand_utf8, int cand_len,
                                       uint32_t base);

struct ime_state {
	const struct ime_dict *dict;

	/* The literal keys typed so far: lowercase a-z and explicit apostrophe
	 * separators, in order. This is the ENTIRE state ime_feed mutates, and
	 * ime_candidates/ime_commit are pure functions of it (recomputed fresh
	 * on every feed) -- which is what makes backspace exact: raw_len-- and
	 * recompute gives byte-identical results to never having typed the
	 * removed key, with no separate undo log to keep in sync. */
	char raw[IME_MAX_RAW];
	int raw_len;

	int page; /* 0-based index into the candidate list, in IME_PAGE_SIZE steps */

	/* The user-weight hook. NULL after ime_reset(): the engine ships with
	 * the hook CALLABLE AND UNUSED, and with it unused every ranking below
	 * is exactly the dictionary's own frequency order -- byte for byte what
	 * v1 produced for the classes v1 had. */
	ime_user_weight_fn user_fn;
	void *user_ctx;
	uint32_t user_mul; /* q8; 256 == 1.0. See ime_user_weight_fn's bound(). */
	uint32_t user_add;

	/* Recomputed by recompute() (pinyin.c, static) on every ime_feed call
	 * that changes raw[]. Ordered by (tier, score desc, encounter). */
	struct ime_candidate cand[IME_MAX_CAND];
	int ncand;
};

/* Begin (or restart) a composition against `dict`. Clears raw/page/cand AND
 * the user-weight hook -- a state is inert until something installs one. */
void ime_reset(struct ime_state *st, const struct ime_dict *dict);

/* Install (or, with fn == 0, remove) the learned-weight hook on one state.
 * (max_mul_q8, max_add) declare the store's own ceiling, as
 * `bound(base) = ((base * max_mul_q8) >> 8) + max_add`; see
 * ime_user_weight_fn for why the engine needs it, what it does with a bonus
 * that exceeds it, and what a store pays for declaring a loose one.
 * max_mul_q8 below 256 is raised to 256 -- a bound below the base itself
 * would prune candidates that have no bonus at all. Call after ime_reset(),
 * before the first ime_feed(). */
void ime_set_user_weight(struct ime_state *st, ime_user_weight_fn fn,
                         void *ctx, uint32_t max_mul_q8, uint32_t max_add);

enum {
	IME_FEED_IGNORED = 0, /* key not consumed -- not a composing key, or a bound (max length/page) was hit */
	IME_FEED_COMPOSING,   /* the raw buffer changed (letter appended, apostrophe added, or backspace); candidates recomputed */
	IME_FEED_PAGED,        /* pageup/pagedown moved the visible page; raw buffer unchanged */
	IME_FEED_CANCELLED,    /* escape: composition cleared back to empty (raw_len == 0, ncand == 0) */
	IME_FEED_EMPTY,         /* backspace emptied the LAST letter: composition is now empty (same end state as CANCELLED, distinct return so a caller can tell "backed all the way out" from "gave up") */
};

/* These match KEY_PGUP/KEY_PGDN in include/abi/logit_abi.h by VALUE, not by
 * #include -- this file stays ABI-agnostic like c/lib/gfx (it has callers
 * outside the kernel: tests/unit/ime_test.c links pinyin.c on the host with
 * no kernel headers at all). tests/unit/ime_test.c statically asserts the
 * two headers agree. Backspace/escape/space are already-standard ASCII
 * control codes (see c/apps/browser/browser.c's own '\b'/'\n' handling for
 * the existing convention this file matches) and need no constant of their
 * own. */
#define IME_KEY_PGUP 0x103
#define IME_KEY_PGDN 0x104

/* Feed one key from the window manager's input stream. `ch` is:
 *   'a'..'z'           a syllable letter -- appended, buffer re-ranked
 *   '\''                explicit syllable separator. It forces a cut, and it
 *                       also disables TIER_KEY, TIER_PRE and TIER_ABBR: an
 *                       apostrophe is the user asserting where the syllables
 *                       end, and all three of those classes read the buffer
 *                       as an undivided string. ("xi'an" must not resurrect
 *                       the single-syllable 先 the apostrophe exists to rule
 *                       out, and an initials lookup over a buffer with
 *                       explicit syllable boundaries is meaningless.)
 *   '\b' (8)            backspace -- pops the last raw[] entry (letter or
 *                       apostrophe) and recomputes; see IME_FEED_EMPTY
 *   27 (ESC)            cancel the whole composition
 *   IME_KEY_PGUP/PGDN   move one page; refused (IME_FEED_IGNORED, page
 *                       unchanged) past either end -- see ime_candidates
 *
 * Anything else (space, a digit, a plain letter outside a-z, ...) is
 * IME_FEED_IGNORED: this file does not special-case "space confirms the
 * first candidate" or "digit N selects candidate N" because doing either
 * needs an output buffer for the resulting codepoints, which ime_feed's
 * signature does not have (see ime_commit's own comment) -- assembling
 * "space means idx 0, digit '3' means idx 2" and calling ime_commit with
 * that idx is the caller's five-line job, not this file's guess at a UI
 * convention it cannot fully implement anyway. */
int ime_feed(struct ime_state *st, int ch);

/* Copy up to `max` candidates of the CURRENT PAGE into `out` (candidate 0 of
 * the page is candidate page*IME_PAGE_SIZE of the composition), returns the
 * count copied (0..min(IME_PAGE_SIZE, max)), or -1 if `st` is not composing
 * (raw_len == 0). */
int ime_candidates(const struct ime_state *st, struct ime_candidate *out, int max);

/* Commit one candidate to codepoints. `idx` is PAGE-RELATIVE (0-based, same
 * indexing ime_candidates hands back -- so a caller that mapped a digit key
 * to an index does not have to also track the page offset), OR
 * IME_COMMIT_RAW to commit the raw typed letters verbatim (each ASCII byte
 * is its own codepoint, apostrophes included -- this is the literal-English-
 * fallback path: always available, even at zero candidates, per the "the raw
 * letters must still be committable" requirement).
 *
 * Returns the number of codepoints written (<= max), or -1 if idx names a
 * candidate outside the current page. Does NOT reset `st` -- the composition
 * stays open (so paging/selecting again after a commit is meaningful for a
 * caller that wants it); call ime_reset() when the caller is done with it. */
#define IME_COMMIT_RAW (-1)
int ime_commit(struct ime_state *st, int idx, uint32_t *out, int max);

/* Describe WHICH dictionary entry page-relative `idx` names, so a learned-
 * weight store can record what the user just chose in the same (key, text)
 * terms ime_user_weight_fn is asked about. Both strings point INTO the
 * dictionary and are NOT NUL-terminated.
 *
 * Returns 1 on success, 0 if `idx` is out of range OR names a TIER_SEG
 * composition, which has no single source entry -- a composed candidate is a
 * concatenation of several keys' top candidates and there is nothing honest
 * to attribute a weight to. A store that ignores this and learns nothing from
 * a composed commit is behaving correctly. */
int ime_commit_source(const struct ime_state *st, int idx,
                      const char **key, int *keylen,
                      const uint8_t **cand_utf8, int *cand_len);

/* Per-recompute() work counters, compiled in only under -DIME_STATS.
 *
 * They exist because the only cost guard this engine ever had was an rdtsc
 * block that prints "skipped" on darwin/arm64 -- the one host anybody
 * develops on -- which is CLAUDE.md's host-reality table, fifth shape. An
 * operation count is portable, is a proxy for the real number rather than the
 * number itself (the real one is a guest measurement; the BKL and TCG are not
 * modelled here), and can be asserted against a pinned budget on EVERY host.
 * Zero cost when the macro is off. */
#ifdef IME_STATS
extern unsigned long ime_stat_keys, ime_stat_cands, ime_stat_userfn;
void ime_stat_reset(void);
#endif

#endif /* C_LIB_IME_PINYIN_H_ */
