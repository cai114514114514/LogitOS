/* c/kernel/gui/ime_learn.c -- the pinyin user-weight store.
 *
 * See ime_learn.h for the whole design argument: why the table is here and not
 * in c/lib/ime, what makes a mutable shared table safe without a lock on this
 * machine, why the weight is additive, why the bound is a halving sweep rather
 * than an LRU, and why no keystroke ever waits for the disk.
 *
 * THE FILE IS SPLIT BY A MACRO, NOT BY A TRANSLATION UNIT, and the split is the
 * same one c/kernel/exec/kpoll.c makes against kpollsys.c: everything above
 * "THE KERNEL HALF" is pure memory work over static arrays and compiles on the
 * host with no kernel header at all, so the table, the ageing sweep, the
 * serialiser and the parser are testable by a program rather than only by
 * booting QEMU. Below it is vfs/kheap/ktimer/work, none of which exists on a
 * host. -DIME_LEARN_HOST is what a harness passes.
 *
 * One TU rather than two because, unlike file.c, every line here is mine and
 * the untestable part is six functions -- kpoll's split exists because file.c
 * cannot be compiled for the host at ALL, which is not the situation here.
 */

#include <stdint.h>
#include <stddef.h>

#include "ime_learn.h"

#ifndef IME_LEARN_HOST
#include "vfs.h"
#include "kheap.h"
#include "kprintf.h"
#include "ktime.h"
#include "work.h"
#else
#include <stdio.h>
#define kprintf printf
#endif

/* ===================== the table ========================================== */

struct lslot {
	uint32_t hash;     /* 0 = empty. The stored hash is never 0 (see hash_of) */
	uint32_t count;    /* commits observed, after any ageing */
	uint16_t off;      /* arena offset of the key bytes; candidate follows */
	uint8_t  klen, clen;
};

/* TWO arenas, alternated. The ageing sweep drops entries and must then compact
 * the string pool, and compacting in place needs the entries in ARENA order --
 * which the slot table does not have and which would cost a sort of 1,024
 * indices on a kernel stack. Copying into the other arena in SLOT order is one
 * forward pass with no sort and no allocation, which is what lets
 * ime_learn_note() keep its "never allocates" promise on the path a user's
 * keystroke actually takes. The price is IME_LEARN_ARENA bytes of .bss, paid
 * once. */
static struct lslot g_slot[IME_LEARN_SLOTS];
static uint8_t      g_arena[2][IME_LEARN_ARENA];
static uint32_t     g_cur;              /* which arena is live */
static uint32_t     g_atop;             /* bytes used in the live arena */
static uint32_t     g_nent;             /* live entries */
static uint32_t     g_commits;          /* commits ever recorded (post-ageing) */
static uint32_t     g_gen;              /* bumped on every mutation */
static uint32_t     g_saved_gen;        /* the gen last written to disk */
static uint32_t     g_dict_id;          /* the dictionary this learned against */
static uint32_t     g_sweeps;           /* ageing sweeps run */
static int          g_ready;            /* ime_learn_init() has run */

static uint8_t *arena(void) { return g_arena[g_cur]; }

/* FNV-1a over key, a separator byte that cannot occur in either string, then
 * the candidate. The separator matters: without it ("ni" + "hao你") and
 * ("nih" + "ao你") would hash and compare as the same entry. Keys are
 * lowercase ASCII and candidates are non-ASCII UTF-8, so 0xFF appears in
 * neither -- but that is a property this file also ENFORCES in
 * ime_learn_note() rather than assumes, because the day it stops being true
 * the symptom is two words silently sharing one weight. */
static uint32_t hash_of(const char *k, int kl, const uint8_t *c, int cl)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < kl; i++) { h ^= (uint8_t)k[i]; h *= 16777619u; }
	h ^= 0xFFu; h *= 16777619u;
	for (int i = 0; i < cl; i++) { h ^= c[i]; h *= 16777619u; }
	return h ? h : 1u;    /* 0 is the empty marker, so it is not a hash */
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, int n)
{
	for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
	return 1;
}

/* Find the slot holding this pair, or -1. Open addressing, linear probe. The
 * table is never allowed past 3/4 full (see maybe_age), so a probe terminates
 * on an empty slot well before it degenerates. */
static int find(uint32_t h, const char *k, int kl, const uint8_t *c, int cl)
{
	uint32_t m = IME_LEARN_SLOTS - 1u;
	for (uint32_t i = h & m, n = 0; n < IME_LEARN_SLOTS; n++, i = (i + 1) & m) {
		struct lslot *s = &g_slot[i];
		if (!s->hash) return -1;                    /* empty: not present */
		if (s->hash == h && s->klen == kl && s->clen == cl) {
			const uint8_t *p = arena() + s->off;
			if (bytes_eq(p, (const uint8_t *)k, kl) &&
			    bytes_eq(p + kl, c, cl)) return (int)i;
		}
	}
	return -1;
}

/* Insert with a known-absent key. Returns 0, or -1 if the arena cannot hold
 * the strings (the caller has already made room; a second failure means the
 * entry itself is too big, which ime_learn_note() rules out by length). */
static int insert(uint32_t h, const char *k, int kl, const uint8_t *c, int cl,
                  uint32_t count)
{
	if (g_atop + (uint32_t)(kl + cl) > IME_LEARN_ARENA) return -1;
	if (g_nent >= IME_LEARN_SLOTS) return -1;
	uint32_t m = IME_LEARN_SLOTS - 1u;
	uint32_t i = h & m;
	while (g_slot[i].hash) i = (i + 1) & m;
	uint8_t *p = arena() + g_atop;
	for (int j = 0; j < kl; j++) p[j] = (uint8_t)k[j];
	for (int j = 0; j < cl; j++) p[kl + j] = c[j];
	g_slot[i].hash = h;
	g_slot[i].count = count;
	g_slot[i].off = (uint16_t)g_atop;
	g_slot[i].klen = (uint8_t)kl;
	g_slot[i].clen = (uint8_t)cl;
	g_atop += (uint32_t)(kl + cl);
	g_nent++;
	return 0;
}

/* Remove slot `i`, keeping every remaining probe chain intact.
 *
 * BACKWARD-SHIFT DELETION, and it is here instead of a tombstone or a rehash
 * for a reason that is about the KERNEL STACK. Clearing a slot in the middle
 * of a linear-probe cluster makes every entry after it in that cluster
 * unreachable -- find() stops at the first empty slot -- so a sweep that drops
 * entries must either leave tombstones (which never get collected, so the
 * table degrades permanently after enough ageing) or rebuild the table from a
 * copy. A copy is 1,024 * 12 = 12,288 bytes, which is not something to put on
 * a ring-0 stack, and a static scratch array is 12 KiB of .bss that exists to
 * be used for a few microseconds a year.
 *
 * Backward shift needs neither: it walks forward from the hole and pulls back
 * exactly those entries whose ideal slot is at or before the hole, which is
 * the standard result that open addressing with linear probing supports true
 * deletion. Bounded by the cluster length, which the 3/4 load factor bounds. */
static void erase(uint32_t i)
{
	uint32_t m = IME_LEARN_SLOTS - 1u;
	g_slot[i].hash = 0;
	g_nent--;
	for (uint32_t j = i;;) {
		j = (j + 1) & m;
		if (!g_slot[j].hash) return;
		uint32_t k = g_slot[j].hash & m;
		/* Keep j where it is iff its ideal slot k lies cyclically in (i, j]. */
		int keep = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
		if (keep) continue;
		g_slot[i] = g_slot[j];
		g_slot[j].hash = 0;
		i = j;
	}
}

/* THE AGEING SWEEP. Halve every count, drop what reaches zero, then compact
 * the string pool into the other arena in one forward pass.
 *
 * Returns the number of entries dropped. Called in a bounded loop by
 * maybe_age(): halving repeatedly drives every count to zero, so at most 32
 * rounds empties the table completely and the loop cannot spin. */
static uint32_t age_once(void)
{
	uint32_t dropped = 0, tot = 0;

	/* Halve, and erase what reaches zero. `i` is NOT advanced after an erase:
	 * the backward shift can move a later entry into the slot just vacated,
	 * and that entry has not been halved yet -- advancing would skip it. Each
	 * erase removes one entry, so the loop is bounded by 2 * IME_LEARN_SLOTS. */
	for (uint32_t i = 0; i < IME_LEARN_SLOTS;) {
		if (!g_slot[i].hash) { i++; continue; }
		uint32_t c = g_slot[i].count >> 1;
		if (c == 0) { erase(i); dropped++; continue; }
		g_slot[i].count = c;
		tot += c;
		i++;
	}
	g_commits = tot;

	/* Compact. Source and destination are DIFFERENT buffers, which is the
	 * whole reason there are two: a forward copy inside one arena would need
	 * the entries in arena order, and the slot table is in hash order. */
	uint32_t src = g_cur, dst = g_cur ^ 1u;
	const uint8_t *sp = g_arena[src];
	uint8_t *dp = g_arena[dst];
	uint32_t top = 0;
	for (uint32_t i = 0; i < IME_LEARN_SLOTS; i++) {
		struct lslot *s = &g_slot[i];
		if (!s->hash) continue;
		uint32_t n = (uint32_t)s->klen + s->clen;
		const uint8_t *q = sp + s->off;
		for (uint32_t j = 0; j < n; j++) dp[top + j] = q[j];
		s->off = (uint16_t)top;
		top += n;
	}
	g_cur = dst;
	g_atop = top;
	g_sweeps++;
	return dropped;
}

/* Make room for one more entry of `need` bytes, if there is not already room.
 * Two independent limits and BOTH are real: the slot table (3/4 full is where
 * linear probing stops being cheap) and the string pool. Whichever fills
 * first triggers the same sweep. */
static void maybe_age(uint32_t need)
{
	int rounds = 0;
	while ((g_nent + 1u > (IME_LEARN_SLOTS / 4u) * 3u ||
	        g_atop + need > IME_LEARN_ARENA) && rounds < 32) {
		uint32_t before = g_nent;
		uint32_t dropped = age_once();
		rounds++;
		kprintf("[ime] learn: age sweep %u -- %u of %u entries dropped,"
		        " %u bytes -> %u\n",
		        (unsigned)g_sweeps, (unsigned)dropped, (unsigned)before,
		        (unsigned)IME_LEARN_ARENA, (unsigned)g_atop);
		if (g_nent == 0) break;
	}
}

/* ===================== the ranking hook =================================== */

uint32_t ime_learn_weight(void *ctx, const char *key, int keylen,
                          const uint8_t *cand_utf8, int cand_len,
                          uint32_t base)
{
	(void)ctx;
	/* `base` IS IGNORED ON PURPOSE. ime_learn.h argues it at length: a bonus
	 * proportional to the candidate's own frequency helps whichever candidate
	 * is already winning, and the quantity that would have to appear in the
	 * formula -- the RIVAL's frequency -- is the one this hook is never shown. */
	(void)base;
	if (!g_nent) return 0;                      /* the overwhelmingly common case */
	uint32_t h = hash_of(key, keylen, cand_utf8, cand_len);
	int i = find(h, key, keylen, cand_utf8, cand_len);
	if (i < 0) return 0;
	uint32_t c = g_slot[i].count;
	if (c > IME_LEARN_COUNT_MAX) c = IME_LEARN_COUNT_MAX;
	return c * IME_LEARN_STEP;
}

/* ===================== the training signal ================================ */

/* The identity rule the hash's separator byte depends on, enforced instead of
 * assumed: a key is lowercase ASCII letters, a candidate is bytes above 0x20
 * with at least one above 0x7F. A candidate that is pure ASCII would be a
 * dictionary carrying Latin text, which ime_ui.c's U+4E00..U+9FFF refusal
 * already rejects on the way to the app -- refusing it here too keeps the two
 * doors saying the same thing. */
static int identity_ok(const char *k, int kl, const uint8_t *c, int cl)
{
	if (kl <= 0 || kl > IME_LEARN_KEYMAX) return 0;
	if (cl <= 0 || cl > IME_LEARN_CANDMAX) return 0;
	for (int i = 0; i < kl; i++) if (k[i] < 'a' || k[i] > 'z') return 0;
	int high = 0;
	for (int i = 0; i < cl; i++) {
		if (c[i] <= 0x20 || c[i] == 0xFF) return 0;
		if (c[i] > 0x7F) high = 1;
	}
	return high;
}

static void note_locked(const char *key, int keylen,
                        const uint8_t *cand, int cand_len, uint32_t add)
{
	uint32_t h = hash_of(key, keylen, cand, cand_len);
	int i = find(h, key, keylen, cand, cand_len);
	if (i >= 0) {
		uint32_t c = g_slot[i].count + add;
		if (c < g_slot[i].count) c = 0xFFFFFFFFu;    /* saturate, never wrap */
		g_slot[i].count = c;
	} else {
		maybe_age((uint32_t)(keylen + cand_len));
		if (insert(h, key, keylen, cand, cand_len, add) < 0) return;
	}
	g_commits += add;
	g_gen++;
}

/* ===================== the file format ====================================
 *
 * Text, and settings.h's argument carries over unchanged: the first time this
 * file confuses somebody they are going to `cat` it, and a store of what a
 * person types is exactly the file they may want to edit or delete by hand.
 *
 *   # LogitOS pinyin user weights -- what this account chose.
 *   # <pinyin key> <candidate> <commits>
 *   # dict = 0x1a2b3c4d
 *   nihao 你好 6
 *
 * Space separated and UNAMBIGUOUS BY CONSTRUCTION, not by escaping: keys are
 * a-z and candidates hold no byte <= 0x20, both enforced by identity_ok()
 * before an entry can ever exist. So a line has exactly three fields and a
 * parser needs no quoting rules.
 *
 * EVERY LINE IS TERMINATED and a final line without a newline is DISCARDED --
 * settings.h's rule, and it is what makes a torn or truncated file safe by
 * construction rather than by testing: cutting the file at any byte leaves a
 * prefix of whole lines plus at most one partial line, and the partial one is
 * never parsed.
 *
 * NO CHECKSUM, and that is a decision rather than an omission. settings.conf
 * carries a crc32 as a torn-write DIAGNOSTIC; here every line is independent
 * and self-validating (three fields, a key that must be a-z, a count that must
 * be a number), so a damaged line costs one learned word and the file reports
 * its own damage by the count of lines it rejected. A checksum would add a
 * number that must never be a gate, over a file where the per-line check
 * already is one. */

static int put(char *b, int n, int max, const char *s)
{
	while (*s && n < max) b[n++] = *s++;
	return n;
}

static int put_u32(char *b, int n, int max, uint32_t v)
{
	char t[12];
	int k = 0;
	if (!v) t[k++] = '0';
	while (v) { t[k++] = (char)('0' + v % 10u); v /= 10u; }
	while (k && n < max) b[n++] = t[--k];
	return n;
}

static int put_hex32(char *b, int n, int max, uint32_t v)
{
	static const char H[] = "0123456789abcdef";
	n = put(b, n, max, "0x");
	for (int sh = 28; sh >= 0 && n < max; sh -= 4) b[n++] = H[(v >> sh) & 0xF];
	return n;
}

/* Serialise the whole table into `buf`. Returns bytes written, or -1 if the
 * buffer is too small.
 *
 * *** ONE PASS OF PURE MEMORY WORK, NO CALL THAT CAN SLEEP. *** That is not a
 * performance note, it is the correctness argument in ime_learn.h: the writer
 * runs on the kworker thread and any blocking call inside this loop would drop
 * the BKL and let the WM thread mutate the table halfway through, producing a
 * file that is a mixture of two states. Allocation happens before this is
 * called; the disk write happens after it returns. */
int ime_learn_serialise(char *buf, int max)
{
	int n = 0;
	n = put(buf, n, max,
	        "# LogitOS pinyin user weights -- what this account chose.\n"
	        "# One entry per line: <pinyin key> <candidate> <commits>.\n"
	        "# Safe to edit or delete by hand; a line that does not parse is\n"
	        "# ignored and deleting this file forgets everything the input\n"
	        "# method learned, restoring the dictionary's own frequency order.\n"
	        "# dict = ");
	n = put_hex32(buf, n, max, g_dict_id);
	n = put(buf, n, max, "\n");
	if (n >= max) return -1;

	for (uint32_t i = 0; i < IME_LEARN_SLOTS; i++) {
		struct lslot *s = &g_slot[i];
		if (!s->hash) continue;
		int need = s->klen + s->clen + 16;
		if (n + need >= max) return -1;
		const uint8_t *p = arena() + s->off;
		for (int j = 0; j < s->klen; j++) buf[n++] = (char)p[j];
		buf[n++] = ' ';
		for (int j = 0; j < s->clen; j++) buf[n++] = (char)p[s->klen + j];
		buf[n++] = ' ';
		n = put_u32(buf, n, max, s->count);
		buf[n++] = '\n';                     /* EVERY line is terminated */
	}
	return n;
}

/* Parse a file image into the table, which must be empty. Returns entries
 * accepted; `*rejected` (may be NULL) gets the number of non-comment lines
 * that did not parse, which is the file's own damage report. */
int ime_learn_parse(const char *buf, int len, int *rejected)
{
	int acc = 0, rej = 0, i = 0;
	while (i < len) {
		int e = i;
		while (e < len && buf[e] != '\n') e++;
#ifndef LEARN_CTL_KEEPTAIL
		if (e >= len) break;                 /* UNTERMINATED LAST LINE: discarded */
#else
		/* THE NEGATIVE CONTROL (tests/imelearn.mk). Accepting the final
		 * unterminated line is the plausible wrong parser -- it looks more
		 * forgiving and it reads a torn write as data. It exists so the
		 * "truncate at every byte offset" sweep can be watched FAILING;
		 * a sweep that has never gone red proves nothing. */
		if (e >= len && i >= len) break;
#endif
		int a = i, b = e;
		i = e + 1;
		while (a < b && (buf[a] == ' ' || buf[a] == '\t' || buf[a] == '\r')) a++;
		while (b > a && (buf[b - 1] == ' ' || buf[b - 1] == '\t' || buf[b - 1] == '\r')) b--;
		if (a == b || buf[a] == '#') continue;

		int k0 = a, k1 = a;
		while (k1 < b && buf[k1] != ' ') k1++;
		int c0 = k1;
		while (c0 < b && buf[c0] == ' ') c0++;
		int c1 = c0;
		while (c1 < b && buf[c1] != ' ') c1++;
		int n0 = c1;
		while (n0 < b && buf[n0] == ' ') n0++;
		if (n0 >= b) { rej++; continue; }

		uint32_t cnt = 0;
		int ok = 1;
		for (int p = n0; p < b; p++) {
			if (buf[p] < '0' || buf[p] > '9') { ok = 0; break; }
			cnt = cnt * 10u + (uint32_t)(buf[p] - '0');
			if (cnt > 0x00FFFFFFu) { cnt = 0x00FFFFFFu; }
		}
		if (!ok || !cnt) { rej++; continue; }
		if (!identity_ok(buf + k0, k1 - k0, (const uint8_t *)buf + c0, c1 - c0)) {
			rej++; continue;
		}
		note_locked(buf + k0, k1 - k0, (const uint8_t *)buf + c0, c1 - c0, cnt);
		acc++;
	}
	if (rejected) *rejected = rej;
	return acc;
}

/* ===================== THE KERNEL HALF ====================================
 * Everything below needs vfs/kheap/ktimer/work and exists only in the kernel.
 * ========================================================================== */

#ifndef IME_LEARN_HOST

static struct ktimer g_timer;
static struct work   g_work;
static int           g_write_fail;     /* consecutive failed writes */

/* The buffer the file is built in. NOT static .bss: it is 20 KiB used for a
 * few microseconds every few seconds at most, and the kworker is a thread that
 * may allocate. .bss would make the store cost 20 KiB forever to save one
 * kmalloc on a path that is by construction not hot. */
#define LEARN_FILEMAX 24576

static void flush_now(void *arg)
{
	(void)arg;
	if (!g_ready) return;
	uint32_t want = g_gen;
	if (want == g_saved_gen) return;              /* nothing changed */
	if (g_write_fail >= 3) return;                /* stop shouting; see below */

	/* ALLOCATE FIRST. kmalloc may grow the heap, which may sleep, which drops
	 * the BKL -- so it must happen before the snapshot, never inside it. */
	char *buf = kmalloc(LEARN_FILEMAX);
	if (!buf) {
		kprintf("[ime] learn: oom (%d bytes) -- not written this round\n",
		        LEARN_FILEMAX);
		return;
	}

	/* THE SNAPSHOT. One pass, no call that can sleep, so the table cannot
	 * change under it and the file is one instant rather than a mixture. */
	want = g_gen;
	int n = ime_learn_serialise(buf, LEARN_FILEMAX);
	uint32_t ents = g_nent, coms = g_commits;

	if (n < 0) {
		/* The table outgrew the file buffer. Bounded and reportable rather
		 * than a silent truncation: a half-written store would lose entries
		 * with no signal at all. */
		kprintf("[ime] learn: %u entries do not fit in %d bytes -- NOT written\n",
		        (unsigned)ents, LEARN_FILEMAX);
		kfree(buf);
		g_write_fail++;
		return;
	}

	if (vfs_mkdir(IME_LEARN_DIR) < 0) { /* already there: fine */ }
	int rc = vfs_write(IME_LEARN_PATH, buf, n);
	kfree(buf);
	if (rc < 0) {
		g_write_fail++;
		/* THREE AND THEN QUIET. A store that cannot be written prints once
		 * per attempt forever, and this console is also /bin/sh's stdout --
		 * wm.c's own report functions make the same argument for the same
		 * reason. The count is in ime_learn_stats() for anyone who asks. */
		kprintf("[ime] learn: write %s FAILED (%d), %d bytes%s\n",
		        IME_LEARN_PATH, rc, n,
		        g_write_fail >= 3 ? " -- giving up until the next boot" : "");
		return;
	}
	g_write_fail = 0;
	g_saved_gen = want;
	kprintf("[ime] learn: wrote %s -- %u entries, %u commits, %d bytes\n",
	        IME_LEARN_PATH, (unsigned)ents, (unsigned)coms, n);

	/* A commit that landed between the snapshot and here is not lost: the gen
	 * moved past `want`, so re-arm and write again. */
	if (g_gen != g_saved_gen) ime_learn_flush_soon();
}

/* THE TIMER CALLBACK, and it does exactly one thing.
 *
 * ktime.h: "THE CALLBACK RUNS IN INTERRUPT CONTEXT ... A callback must not
 * block, must not take the BKL, and must not touch the heap. Wake a thread and
 * get out." work.h's other half: "Waking is safe from anywhere, including
 * interrupt context -- that asymmetry is the whole point of work.c." So this
 * is the documented pattern used exactly as documented: timer -> workqueue ->
 * a thread that may sleep on the disk. */
static void learn_timer(struct ktimer *t)
{
	(void)t;
	work_queue(&g_work);
}

static void arm(uint64_t ms)
{
	/* Re-arming an already-queued timer MOVES it (ktime.h), which is exactly
	 * debounce semantics: a burst of typing writes once, at the end. */
	if (ktimer_add(&g_timer, ms * 1000000ull, 0, learn_timer, 0, "ime-learn") != 0)
		work_queue(&g_work);   /* timer heap full: write now rather than never */
}

void ime_learn_flush_soon(void)
{
	if (!g_ready || g_gen == g_saved_gen) return;
	arm(1);
}

void ime_learn_note(const char *key, int keylen,
                    const uint8_t *cand_utf8, int cand_len)
{
	if (!g_ready) return;
	if (!identity_ok(key, keylen, cand_utf8, cand_len)) {
		kprintf("[ime] learn: REFUSED an entry -- key %d bytes, candidate %d"
		        " bytes, outside the a-z / non-ASCII identity rule\n",
		        keylen, cand_len);
		return;
	}
	note_locked(key, keylen, cand_utf8, cand_len, 1);
	arm(IME_LEARN_QUIET_MS);
}

void ime_learn_init(uint32_t build_id)
{
	if (g_ready) return;
	work_item_init(&g_work, flush_now, 0);
	g_dict_id = build_id;
	g_ready = 1;

	int sz = vfs_size(IME_LEARN_PATH);
	if (sz <= 0) {
		kprintf("[ime] learn: no %s yet -- ranking is the dictionary's own"
		        " frequency order until something is committed\n",
		        IME_LEARN_PATH);
		g_saved_gen = g_gen;
		return;
	}
	if (sz > LEARN_FILEMAX) {
		kprintf("[ime] learn: %s is %d bytes (cap %d) -- REFUSED, a truncated"
		        " read would silently drop entries\n",
		        IME_LEARN_PATH, sz, LEARN_FILEMAX);
		g_saved_gen = g_gen;
		return;
	}
	char *buf = kmalloc((unsigned)sz);
	if (!buf) { g_saved_gen = g_gen; return; }
	int off = 0;
	while (off < sz) {
		int want = sz - off;
		if (want > 65536) want = 65536;
		int got = vfs_pread(IME_LEARN_PATH, buf + off, want, off);
		if (got <= 0) {
			kprintf("[ime] learn: pread at %d returned %d -- keeping the %u"
			        " entries parsed so far\n", off, got, (unsigned)g_nent);
			break;
		}
		off += got;
	}
	int rej = 0;
	int acc = ime_learn_parse(buf, off, &rej);
	kfree(buf);
	g_saved_gen = g_gen;                 /* what we just read IS what is on disk */

	kprintf("[ime] learn: %s -- %d entries, %u commits, %d lines rejected\n",
	        IME_LEARN_PATH, acc, (unsigned)g_commits, rej);
	if (g_dict_id && g_dict_id != build_id)
		kprintf("[ime] learn: dictionary build_id changed -- entries are keyed"
		        " on TEXT so they still apply, but some may name words this"
		        " dictionary no longer has\n");
}

void ime_learn_stats(uint32_t *entries, uint32_t *commits, int *dirty)
{
	if (entries) *entries = g_nent;
	if (commits) *commits = g_commits;
	if (dirty)   *dirty = (g_gen != g_saved_gen);
}

#else  /* IME_LEARN_HOST -- the harness drives the table directly */

void ime_learn_note(const char *key, int keylen,
                    const uint8_t *cand_utf8, int cand_len)
{
	if (!identity_ok(key, keylen, cand_utf8, cand_len)) return;
	note_locked(key, keylen, cand_utf8, cand_len, 1);
}
void ime_learn_init(uint32_t build_id) { g_dict_id = build_id; g_ready = 1; }
void ime_learn_flush_soon(void) { }
void ime_learn_stats(uint32_t *e, uint32_t *c, int *d)
{
	if (e) *e = g_nent;
	if (c) *c = g_commits;
	if (d) *d = (g_gen != g_saved_gen);
}
/* The harness needs to start from nothing between cases. */
void ime_learn_reset_for_test(void)
{
	for (uint32_t i = 0; i < IME_LEARN_SLOTS; i++) g_slot[i].hash = 0;
	g_cur = g_atop = g_nent = g_commits = g_gen = g_saved_gen = g_sweeps = 0;
}

#endif /* IME_LEARN_HOST */
