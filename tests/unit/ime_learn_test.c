/* tests/unit/ime_learn_test.c -- the pinyin user-weight store, on the host.
 *
 * WHAT THIS CAN CHECK AND WHAT IT CANNOT, stated first because the second half
 * is the part a reader will otherwise assume away.
 *
 * c/kernel/gui/ime/ime_learn.c is split by -DIME_LEARN_HOST (its own header
 * explains the split): above the line is the hash table, the ageing sweep, the
 * serialiser and the parser -- pure memory work over static arrays, no kernel
 * header, no allocator, no disk. Below it are vfs_write, kmalloc, ktimer and
 * work_queue, none of which exists here. So this program checks EVERYTHING
 * ABOUT WHAT IS LEARNED AND WHAT IS WRITTEN, against the real shipped
 * dictionary and the real ranking engine, and checks NOTHING about when the
 * bytes reach the disk. The second half is a device claim and is stated as one
 * in this line's report -- it is not smuggled in here as a green tick.
 *
 * The engine is the real c/lib/ime/pinyin.c and the dictionary is the real
 * fsroot/ime/pinyin.dat, exactly as tests/unit/ime_test.c uses them: every rank
 * below is what the shipped machine would show, not what a mock would.
 *
 * THE CONTROLS (each its own binary; see tests/imelearn.mk):
 *   -DLEARN_CTL_DEAF    ime_learn_note() is a no-op -- the machine is told what
 *                       the user chose and does not record it. Section 2's
 *                       trajectory must stop moving.
 *   -DLEARN_CTL_NOHOOK  the ranking hook is never installed. Everything learned
 *                       is still learned and still written; nothing is applied.
 *                       This is the failure that a round-trip test alone cannot
 *                       see, and it is exactly the bug ime_ui.c's st_reset()
 *                       exists to prevent (four of five ime_reset sites right).
 *   -DLEARN_CTL_KEEPTAIL  ime_learn_parse() accepts an unterminated last line.
 *                       Section 5 truncates the file at every byte offset; this
 *                       is the control that shows the truncation sweep can fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pinyin.h"
#include "ime_learn.h"

void ime_learn_reset_for_test(void);

/* ---- harness ------------------------------------------------------------ */

static int g_pass, g_fail;

static void ck(int cond, const char *what, const char *detail)
{
	if (cond) { g_pass++; printf("  ok   %s%s\n", what, detail ? detail : ""); }
	else      { g_fail++; printf("  FAIL %s%s\n", what, detail ? detail : ""); }
}

static char *load_file(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)*len);
	if (fread(b, 1, (size_t)*len, f) != (size_t)*len) { fprintf(stderr, "short read\n"); exit(1); }
	fclose(f);
	return b;
}

/* A candidate's codepoints as UTF-8, for comparing against a literal. */
static int cand_utf8(const struct ime_candidate *c, char *o, int max)
{
	int n = 0;
	for (int i = 0; i < c->ncp; i++) {
		uint32_t cp = c->cp[i];
		if (n + 4 > max) break;
		if (cp < 0x80) o[n++] = (char)cp;
		else if (cp < 0x800) { o[n++] = (char)(0xC0 | (cp >> 6)); o[n++] = (char)(0x80 | (cp & 0x3F)); }
		else { o[n++] = (char)(0xE0 | (cp >> 12)); o[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
		       o[n++] = (char)(0x80 | (cp & 0x3F)); }
	}
	o[n] = 0;
	return n;
}

static const struct ime_dict *g_dict;

/* Type `buf` from scratch. The hook installation is here and NOT in the caller
 * so that LEARN_CTL_NOHOOK is one #ifdef in one place -- a control that had to
 * be spelled at seven call sites would eventually be spelled at six. */
static void type(struct ime_state *st, const char *buf)
{
	ime_reset(st, g_dict);
#ifndef LEARN_CTL_NOHOOK
	ime_set_user_weight(st, ime_learn_weight, 0,
	                    256u, IME_LEARN_STEP * IME_LEARN_COUNT_MAX);
#endif
	for (const char *p = buf; *p; p++) ime_feed(st, *p);
}

/* Absolute rank of `text` in the current candidate list, or -1. */
static int rank_of(const struct ime_state *st, const char *text)
{
	char u[128];
	for (int i = 0; i < st->ncand; i++) {
		cand_utf8(&st->cand[i], u, (int)sizeof u - 1);
		if (strcmp(u, text) == 0) return i;
	}
	return -1;
}

/* Commit the candidate at absolute rank `r` exactly as ime_ui.c's emit() does:
 * page to it, ask ime_commit_source() who it was, hand that to the store.
 * Returns 1 if the store was told, 0 if the commit had no single source. */
static int commit_and_learn(struct ime_state *st, int r)
{
	int page = r / IME_PAGE_SIZE, idx = r % IME_PAGE_SIZE;
	for (int i = 0; i < page; i++) ime_feed(st, IME_KEY_PGDN);
	const char *key; int keylen;
	const uint8_t *ctext; int clen;
	if (!ime_commit_source(st, idx, &key, &keylen, &ctext, &clen)) return 0;
#ifndef LEARN_CTL_DEAF
	ime_learn_note(key, keylen, ctext, clen);
#endif
	return 1;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "fsroot/ime/pinyin.dat";
	long len = 0;
	char *dat = load_file(path, &len);
	g_dict = ime_open(dat, (size_t)len);
	if (!g_dict) { fprintf(stderr, "ime_open refused %s\n", path); return 1; }
	printf("dictionary %s: %u keys, %u candidates, build_id %08x\n",
	       path, g_dict->key_count, g_dict->cand_count, g_dict->build_id);
	printf("store: %u slots, %u arena bytes, step %u, cap %u commits\n",
	       IME_LEARN_SLOTS, IME_LEARN_ARENA, IME_LEARN_STEP, IME_LEARN_COUNT_MAX);

#ifdef LEARN_CTL_DEAF
	printf("[control LEARN_CTL_DEAF: the commit is never recorded]\n");
#endif
#ifdef LEARN_CTL_NOHOOK
	printf("[control LEARN_CTL_NOHOOK: the ranking hook is never installed]\n");
#endif
#ifdef LEARN_CTL_KEEPTAIL
	printf("[control LEARN_CTL_KEEPTAIL: the parser accepts an unterminated last line]\n");
#endif

	struct ime_state st;
	char d[256];

	/* ---- 1. the baseline, which is the whole point of the store ---------- */
	printf("\n-- 1. baseline: the dictionary's own order, store empty --\n");
	ime_learn_reset_for_test();
	ime_learn_init(g_dict->build_id);
	type(&st, "nh");
	int r0 = rank_of(&st, "你好");
	snprintf(d, sizeof d, " (nh -> %d candidates, 你好 at %d)", st.ncand, r0);
	ck(r0 == 8, "你好 starts at rank 8 of the nh bucket", d);
	ck(st.ncand == 32, "nh yields 32 candidates", "");
	{
		uint32_t e = 99, c = 99; int dirty = 9;
		ime_learn_stats(&e, &c, &dirty);
		snprintf(d, sizeof d, " (entries %u, commits %u, dirty %d)", e, c, dirty);
		ck(e == 0 && c == 0 && dirty == 0, "an empty store is clean and holds nothing", d);
	}

	/* ---- 2. THE TRAJECTORY ----------------------------------------------
	 * pinyin.h predicts 8 6 5 2 1 1 0 0 0 for an additive store of step 256,
	 * and it predicted it against a store that is not this one. Reproducing
	 * it here is a cross-check between two independent implementations of the
	 * same policy, not a tautology -- and the PLATEAU (1,1) is the shape that
	 * separates an accumulator from a promote-to-front bug, which would read
	 * 8 0 0 0 0 0 0 0 0. */
	printf("\n-- 2. 你好 climbs the nh bucket, one commit at a time --\n");
	int traj[9], nt = 0;
	traj[nt++] = r0;
	for (int i = 0; i < 8; i++) {
		type(&st, "nh");
		int r = rank_of(&st, "你好");
		if (r < 0) { printf("  FAIL 你好 vanished from the bucket\n"); g_fail++; break; }
		commit_and_learn(&st, r);
		type(&st, "nh");
		traj[nt++] = rank_of(&st, "你好");
	}
	{
		int n = 0;
		n += snprintf(d + n, sizeof d - (size_t)n, " (");
		for (int i = 0; i < nt; i++)
			n += snprintf(d + n, sizeof d - (size_t)n, "%s%d", i ? " " : "", traj[i]);
		snprintf(d + n, sizeof d - (size_t)n, ")");
	}
	/* NO #ifdef ON THE ASSERTION. A control that flips the expectation to
	 * match itself is a control that PASSES, and a passing control is the
	 * thing CLAUDE.md rule 5 says is worse than none: it reads like a check.
	 * Every build below asserts the same sentence; the control builds are
	 * supposed to go RED here, and tests/imelearn.mk requires exactly that. */
	{
		static const int want[9] = {8, 6, 5, 2, 1, 1, 0, 0, 0};
		int same = (nt == 9);
		for (int i = 0; i < nt && same; i++) same = (traj[i] == want[i]);
		ck(same, "trajectory is 8 6 5 2 1 1 0 0 0 -- pinyin.h's own prediction", d);
		int plateau = 0;
		for (int i = 1; i < nt; i++) if (traj[i] == traj[i - 1] && traj[i] != 0) plateau = 1;
		ck(plateau, "it PLATEAUS before reaching 0 -- an accumulator, not promote-to-front", "");
	}

	/* ---- 3. ONE COMMIT, EVERY SPELLING ---------------------------------
	 * ime_learn.h's claim: the key learned is the DICTIONARY key ("nihao"),
	 * not the buffer typed ("nh"), so a commit under the abbreviation also
	 * promotes the word under the full spelling and under every prefix of it.
	 * That is a claim about which of the two strings ime_user_weight_fn is
	 * handed, and it is invisible until something types the other spelling. */
	printf("\n-- 3. learned under \"nh\", applied under \"nihao\" and \"nih\" --\n");
	{
		type(&st, "nihao");
		int r = rank_of(&st, "你好");
		snprintf(d, sizeof d, " (nihao -> 你好 at %d of %d)", r, st.ncand);
		ck(r == 0, "你好 is first under nihao", d);
		type(&st, "nih");
		r = rank_of(&st, "你好");
		snprintf(d, sizeof d, " (nih -> 你好 at %d of %d)", r, st.ncand);
		ck(r == 0, "你好 is first under nih", d);
		/* And the one that would be true anyway if the store keyed on the
		 * TYPED buffer: a different word under the same abbreviation must
		 * NOT have moved. */
		type(&st, "nh");
		r = rank_of(&st, "南海");   /* 南海, the bucket's original leader */
		snprintf(d, sizeof d, " (南海 now at %d, was 0)", r);
		ck(r == 1, "南海 moved down exactly one place -- only 你好 was taught", d);
	}

	/* ---- 4. ROUND TRIP: serialise, forget, parse, and rank the same ------ */
	printf("\n-- 4. round trip through the file format --\n");
	{
		char buf[24576];
		int n = ime_learn_serialise(buf, (int)sizeof buf);
		ck(n > 0, "serialise succeeds", "");
		uint32_t e0 = 0, c0 = 0;
		ime_learn_stats(&e0, &c0, 0);

		ime_learn_reset_for_test();
		type(&st, "nh");
		int rr = rank_of(&st, "你好");
		snprintf(d, sizeof d, " (你好 back at %d)", rr);
		ck(rr == 8, "after a reset the ranking is the dictionary's again", d);

		int rej = -1;
		int acc = ime_learn_parse(buf, n, &rej);
		uint32_t e1 = 0, c1 = 0;
		ime_learn_stats(&e1, &c1, 0);
		snprintf(d, sizeof d, " (%d bytes, %d entries accepted, %d rejected;"
		         " %u/%u entries, %u/%u commits)", n, acc, rej, e1, e0, c1, c0);
		ck(rej == 0 && e1 == e0 && c1 == c0, "parse restores the table exactly", d);

		type(&st, "nh");
		rr = rank_of(&st, "你好");
		snprintf(d, sizeof d, " (你好 at %d)", rr);
		ck(rr == traj[nt - 1], "and the RANKING is the same as before the round trip", d);
	}

	/* ---- 5. TRUNCATE AT EVERY BYTE -------------------------------------
	 * settings.h's rule, applied here: every line is terminated and a final
	 * line without a newline is discarded, so cutting the file anywhere leaves
	 * a prefix of whole lines and never a half-parsed entry. The check is not
	 * "it does not crash" -- it is that the entry count is exactly the number
	 * of NEWLINE-terminated entry lines in the prefix, at every offset. */
	printf("\n-- 5. truncation at every byte offset --\n");
	{
		char buf[24576];
		int n = ime_learn_serialise(buf, (int)sizeof buf);
		int bad = 0, firstbad = -1;
		for (int cut = 0; cut <= n; cut++) {
			/* how many entry lines are wholly inside buf[0,cut) */
			int want = 0;
			for (int i = 0, ls = 0; i < cut; i++)
				if (buf[i] == '\n') { if (buf[ls] != '#') want++; ls = i + 1; }
			ime_learn_reset_for_test();
			int rej = 0;
			int acc = ime_learn_parse(buf, cut, &rej);
			if (acc != want || rej != 0) { bad++; if (firstbad < 0) firstbad = cut; }
		}
		snprintf(d, sizeof d, " (%d cuts, %d disagreed, first at %d)",
		         n + 1, bad, firstbad);
		ck(bad == 0, "every prefix parses to exactly its whole lines, no partial entry", d);
	}

	/* ---- 6. WHAT IS REFUSED -------------------------------------------- */
	printf("\n-- 6. refusals: the identity rule, enforced not assumed --\n");
	{
		ime_learn_reset_for_test();
		uint32_t e = 0;
		ime_learn_note("NiHao", 5, (const uint8_t *)"你好", 6);
		ime_learn_stats(&e, 0, 0);
		ck(e == 0, "a key with capitals is refused", "");
		ime_learn_note("nihao", 5, (const uint8_t *)"hello", 5);
		ime_learn_stats(&e, 0, 0);
		ck(e == 0, "a pure-ASCII candidate is refused", "");
		ime_learn_note("nihao", 5, (const uint8_t *)"你 好", 7);
		ime_learn_stats(&e, 0, 0);
		ck(e == 0, "a candidate containing a space is refused -- the file format has no quoting", "");
		{
			char big[IME_LEARN_KEYMAX + 2];
			memset(big, 'a', sizeof big);
			ime_learn_note(big, IME_LEARN_KEYMAX + 1, (const uint8_t *)"你好", 6);
			ime_learn_stats(&e, 0, 0);
			ck(e == 0, "a key longer than IME_LEARN_KEYMAX is refused, not truncated", "");
		}
		ime_learn_note("nihao", 5, (const uint8_t *)"你好", 6);
		ime_learn_stats(&e, 0, 0);
		ck(e == 1, "and a well-formed entry is still accepted afterwards", "");
	}

	/* ---- 7. A DAMAGED FILE DESCRIBES ITS OWN DAMAGE --------------------- */
	printf("\n-- 7. a hand-edited file --\n");
	{
		static const char f[] =
			"# a comment\n"
			"nihao \xe4\xbd\xa0\xe5\xa5\xbd 6\n"
			"nihao \xe4\xbd\xa0\xe5\xa5\xbd\n"          /* two fields, not three */
			"NIHAO \xe4\xbd\xa0\xe5\xa5\xbd 3\n"        /* capitals */
			"beijing \xe5\x8c\x97\xe4\xba\xac zzz\n"    /* count is not a number */
			"beijing \xe5\x8c\x97\xe4\xba\xac 0\n"      /* a zero count is not an entry */
			"\n"
			"beijing \xe5\x8c\x97\xe4\xba\xac 4\n";
		ime_learn_reset_for_test();
		int rej = 0;
		int acc = ime_learn_parse(f, (int)sizeof f - 1, &rej);
		snprintf(d, sizeof d, " (accepted %d, rejected %d)", acc, rej);
		ck(acc == 2 && rej == 4, "2 good lines accepted, 4 bad ones counted and skipped", d);
		ck(ime_learn_weight(0, "nihao", 5, (const uint8_t *)"你好", 6, 0)
		   == 6u * IME_LEARN_STEP, "the good line's weight is exactly commits * step", "");
	}

	/* ---- 8. BOUNDED: a user types for years ---------------------------- */
	printf("\n-- 8. the bound, and what it costs --\n");
	{
		ime_learn_reset_for_test();
		/* Distinct synthetic entries, well past both limits. The candidate is
		 * 3 UTF-8 bytes in the CJK range so identity_ok accepts it. */
		int made = 0;
		for (int i = 0; i < 4000; i++) {
			char k[8];
			uint8_t c[3];
			k[0] = (char)('a' + i % 26); k[1] = (char)('a' + (i / 26) % 26);
			k[2] = (char)('a' + (i / 676) % 26); k[3] = 0;
			uint32_t cp = 0x4E00u + (uint32_t)i;
			c[0] = (uint8_t)(0xE0 | (cp >> 12));
			c[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
			c[2] = (uint8_t)(0x80 | (cp & 0x3F));
			ime_learn_note(k, 3, c, 3);
			made++;
		}
		uint32_t e = 0, c = 0;
		ime_learn_stats(&e, &c, 0);
		snprintf(d, sizeof d, " (%d notes -> %u entries, %u commits, cap %u slots)",
		         made, e, c, IME_LEARN_SLOTS);
		ck(e <= (IME_LEARN_SLOTS / 4u) * 3u, "the table never exceeds its 3/4 load factor", d);
		ck(e > 0, "and it is not emptied to nothing", "");
		char buf[24576];
		int n = ime_learn_serialise(buf, (int)sizeof buf);
		snprintf(d, sizeof d, " (%d bytes for %u entries, buffer %d)", n, e, (int)sizeof buf);
		ck(n > 0, "a full table still fits the kernel's file buffer", d);
	}

	/* ---- 9. IT DECAYS, WHICH IS THE REASON IT IS NOT AN LRU -------------
	 * ime_learn.h's behavioural claim: halving means a word chosen often
	 * survives a sweep that empties the singletons around it, and its weight
	 * falls rather than staying at full strength until it is evicted outright.
	 * Section 8 shows only the worst case (everything a singleton, so a sweep
	 * empties the table); this is the case a real user produces. */
	printf("\n-- 9. a word the user chose often survives the flood --\n");
	{
		ime_learn_reset_for_test();
		for (int i = 0; i < 40; i++) ime_learn_note("nihao", 5, (const uint8_t *)"你好", 6);
		uint32_t w0 = ime_learn_weight(0, "nihao", 5, (const uint8_t *)"你好", 6, 0);
		/* Flood with singletons until the sweep has run at least twice. */
		for (int i = 0; i < 2000; i++) {
			char k[4]; uint8_t c[3];
			k[0] = (char)('a' + i % 26); k[1] = (char)('a' + (i / 26) % 26);
			k[2] = (char)('a' + (i / 676) % 26); k[3] = 0;
			uint32_t cp = 0x5000u + (uint32_t)i;
			c[0] = (uint8_t)(0xE0 | (cp >> 12));
			c[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
			c[2] = (uint8_t)(0x80 | (cp & 0x3F));
			ime_learn_note(k, 3, c, 3);
		}
		uint32_t w1 = ime_learn_weight(0, "nihao", 5, (const uint8_t *)"你好", 6, 0);
		snprintf(d, sizeof d, " (weight %u -> %u, cap %u)",
		         w0, w1, IME_LEARN_COUNT_MAX * IME_LEARN_STEP);
		ck(w1 > 0, "40 commits still outrank the singletons that swept the table", d);
		ck(w1 < w0, "and the weight FELL -- halving decays, it does not preserve", d);

		/* And a candidate the user chose ONCE, before the flood, is gone --
		 * which is the cost of the bound, stated rather than hidden. */
		ime_learn_reset_for_test();
		ime_learn_note("beijing", 7, (const uint8_t *)"北京", 6);
		for (int i = 0; i < 2000; i++) {
			char k[4]; uint8_t c[3];
			k[0] = (char)('a' + i % 26); k[1] = (char)('a' + (i / 26) % 26);
			k[2] = (char)('a' + (i / 676) % 26); k[3] = 0;
			uint32_t cp = 0x5000u + (uint32_t)i;
			c[0] = (uint8_t)(0xE0 | (cp >> 12));
			c[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
			c[2] = (uint8_t)(0x80 | (cp & 0x3F));
			ime_learn_note(k, 3, c, 3);
		}
		uint32_t w2 = ime_learn_weight(0, "beijing", 7, (const uint8_t *)"北京", 6, 0);
		snprintf(d, sizeof d, " (weight %u)", w2);
		ck(w2 == 0, "a single old commit does NOT survive 2000 newer ones -- the bound's cost", d);
	}

	printf("\n%d checks, %d failed\n", g_pass + g_fail, g_fail);
	return g_fail ? 1 : 0;
}
