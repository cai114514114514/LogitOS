#ifndef C_LIB_IME_PINYIN_H_
#define C_LIB_IME_PINYIN_H_

#include <stdint.h>
#include <stddef.h>

/* Pinyin input method engine -- segmentation + candidate lookup over the
 * dictionary tools/mkpinyin.py builds (c/lib/ime/pinyin_fmt.h, loaded from
 * fsroot/ime/pinyin.dat).
 *
 * FREESTANDING, LIKE c/lib/gfx: no libc, no allocator, integer-only. It is
 * called from the window manager in ring 0 on every keystroke while a
 * composition is active. What "cost per key" actually means, measured (see
 * tests/unit/ime_test.c's IME_BENCH block and the report that shipped this
 * file): a SINGLE dictionary probe is a genuine binary search -- O(log
 * key_count), ~15 string comparisons against a 25,945-key table, built once
 * at ime_open() into a byte-offset index so the variable-length key records
 * support random access at all. A full ime_feed() is NOT "a binary search
 * and nothing else" and this header says so rather than pretend otherwise:
 * segmentation additionally walks the current buffer (bounded by
 * IME_MAX_RAW) trying syllable cuts, each cut test itself a binary search
 * against the 414-entry syllable table. The realistic per-key cost is that
 * walk, bounded and measured -- not one comparison.
 *
 * REENTRANT PER WINDOW: everything that changes while composing lives in a
 * caller-owned struct ime_state, so two windows can each hold an open
 * composition. The ONE exception, matching the brief exactly ("no global
 * state except the dictionary pointer"), is the validated dictionary index
 * itself: one process loads pinyin.dat once, and every window's ime_state
 * just points at the SAME struct ime_dict ime_open() returns. That index
 * (the byte-offset table making binary search possible) is read-only after
 * ime_open() succeeds, which is what makes sharing it across states safe
 * with no lock.
 */

/* ---- the dictionary -------------------------------------------------- */

/* IME_MAX_KEYS bounds the byte-offset index built at load time -- the fixed
 * budget this file's "no allocator" runs on (c/lib/text/glyphras.h's point
 * budget is the same pattern). Measured against the shipped dictionary:
 * 25,945 keys (see fsroot/ime/pinyin.dat's header). 32768 is comfortable
 * headroom for the dictionary to grow without a source change; ime_open()
 * REFUSES (returns NULL) rather than silently truncating the table if a
 * future dictionary exceeds it, because a truncated key index would silently
 * make some keys unreachable by lookup with no signal that anything is wrong. */
#define IME_MAX_KEYS 32768

struct ime_dict {
	const uint8_t *base;   /* the mapped/loaded pinyin.dat bytes -- NOT copied, NOT owned */
	uint32_t len;
	uint32_t key_count;
	/* key_off[i] = byte offset (from base) of the i-th key's entry (its
	 * NUL-terminated key string), in the SAME sorted order the file already
	 * stores keys in. Built once by ime_open() with one sequential pass;
	 * every lookup after that is binary search over this array, which is
	 * the only reason a variable-length, NUL-delimited record format can
	 * support O(log n) lookup at all. */
	uint32_t key_off[IME_MAX_KEYS];
};

/* Validate a pinyin.dat blob already resident at `dat` (magic/version, and
 * that the key table the header promises actually fits in `len` bytes),
 * build the byte-offset index, and return a pointer to the ONE static
 * dictionary object this file keeps (see the "no global state" note above).
 * Returns NULL on a bad header, a size that does not fit, or key_count >
 * IME_MAX_KEYS. Call once, e.g. from wm_run() after the dictionary file is
 * read -- ime_open never touches vfs/vmm itself, so how `dat` got resident
 * (read whole, or paged in via vfs_pread -- CLAUDE.md's IME note names both)
 * is entirely the caller's business. */
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

/* Candidates kept per composition, across all pages. The densest key
 * measured in the shipped dictionary has 66 candidates (the syllable
 * "zhong"); 96 leaves room for that PLUS a handful of segmentation-composed
 * (tier-1) candidates layered on top -- see pinyin.c's ranking comment.
 * ime_candidates()/ime_commit() never read past ncand, so a dictionary that
 * somehow exceeds this is a truncation (documented, not a corruption): the
 * lowest-ranked candidates for that one key are dropped, not overflowed. */
#define IME_MAX_CAND 96

/* Candidates per page. 9 on purpose: it is the conventional "press a digit
 * 1-9 to pick a candidate" IME page size, and it is also EXACTLY where the
 * shipped dictionary's own frequency order places the alternate reading in
 * the "xian" gate case (先 at candidate 0, 西安 at candidate 8 of 32 total
 * under that one key) -- see the engine's final report for the measurement
 * and the fragility that number carries if the dictionary is regenerated
 * with different frequency data. */
#define IME_PAGE_SIZE 9

struct ime_candidate {
	uint32_t cp[IME_CAND_MAXCP];
	int ncp;
};

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

	/* Recomputed by recompute() (pinyin.c, static) on every ime_feed call
	 * that changes raw[]. ncand candidates, ranked whole-word matches (tier
	 * 0) before segmentation-composed candidates (tier 1) -- see pinyin.c. */
	struct ime_candidate cand[IME_MAX_CAND];
	int ncand;
};

/* Begin (or restart) a composition against `dict`. Clears raw/page/cand. */
void ime_reset(struct ime_state *st, const struct ime_dict *dict);

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
 *   'a'..'z'           a syllable letter -- appended, buffer re-segmented
 *   '\''                explicit syllable separator (see pinyin.c's segment
 *                       comment for what apostrophe forbids)
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

#endif /* C_LIB_IME_PINYIN_H_ */
