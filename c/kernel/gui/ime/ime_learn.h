#ifndef LOGIT_IME_LEARN_H
#define LOGIT_IME_LEARN_H

#include <stdint.h>

/* ===========================================================================
 * THE USER-WEIGHT STORE -- what this machine remembers about how its user
 * types Chinese.
 *
 * c/lib/ime/pinyin.c ranks candidates by the dictionary's own jieba frequency.
 * That frequency is a statistic about a corpus, not about a person: it is the
 * right INITIALISATION and it is the wrong final answer, because the whole
 * point of an input method is that two candidates a corpus cannot separate are
 * not remotely equal to the person typing. 你好 and 你号 both exist under
 * "nihao"; only one of them is what this user meant, and the machine is told
 * which one every single time they press a digit.
 *
 * This file is that signal, accumulated.
 *
 * ---------------------------------------------------------------------------
 * WHY IT IS NOT IN c/lib/ime, AND WHY THE ENGINE TAKES A CALLBACK
 * ---------------------------------------------------------------------------
 * pinyin.h states the property this store had to be designed around:
 *
 *     "One process loads pinyin.dat once, and every window's ime_state just
 *      points at the SAME struct ime_dict ... That index is READ-ONLY after
 *      ime_open() succeeds, which is what makes sharing it across states safe
 *      with no lock."
 *
 * A learned weight is MUTABLE. Putting it inside struct ime_dict would destroy
 * exactly that property, and the failure would be a wrong count rather than a
 * crash -- invisible to every host gate. So the engine grew
 * ime_set_user_weight(), the store lives here, and this header owns the
 * mutual-exclusion argument instead of leaving it implied.
 *
 * ---------------------------------------------------------------------------
 * WHAT MAKES THE MUTABLE TABLE SAFE, WRITTEN DOWN RATHER THAN ASSUMED
 * ---------------------------------------------------------------------------
 * The table is one global, shared by every window exactly as the dictionary
 * is. Three contexts touch it, and NONE of them can overlap:
 *
 *   1. ime_learn_weight() -- the ranking hook. Runs from ime_ui_key(), which
 *      runs from wm_process_key(), which runs from wm_drain_input() ON THE WM
 *      THREAD. It does not run in the keyboard IRQ: wm.c's inq ring was
 *      introduced precisely so that IRQ 1 only enqueues (see the long comment
 *      above inq_push()). So this is thread context, holding the BKL.
 *   2. ime_learn_note() -- the commit. Same call chain, same thread, same lock.
 *   3. ime_learn_flush() -- the writer. Runs on the `kworker` THREAD
 *      (c/kernel/sync/work.c), and sched.c:601 records the property that
 *      matters: "kernel threads keep the BKL while in kernel code".
 *
 * One lock, three holders, no overlap. That is the same argument the window
 * manager itself runs on, one level up.
 *
 * *** THE ONE PLACE IT IS NOT ENOUGH, AND WHAT IS DONE ABOUT IT. *** A
 * blocking call inside the writer (kmalloc that has to grow the heap;
 * vfs_write reaching the disk) goes through schedule(), which DROPS the BKL
 * and lets the WM thread run and mutate the table. So the writer is split:
 * every allocation happens BEFORE the snapshot, the snapshot itself is one
 * pass of pure memory work with no call that can sleep, and only then does the
 * single vfs_write() run. The file is therefore a snapshot of one instant and
 * never a mixture of two -- the same claim c/fs/logitfs.c makes about a
 * transaction, made here for the same reason.
 *
 * *** NOTE FOR WHOEVER REMOVES THE BKL. *** Everything above rests on it. When
 * the WM thread and the kworker can run at once, this table needs a lock of
 * its own -- a mutex, not a spinlock, because the writer may sleep -- and
 * ime_learn_weight() is on the per-keystroke ranking path, so it wants a
 * reader/writer shape rather than a mutex the ranking loop takes 3,000 times.
 * Correction after removal: only the memory snapshot holds learn_lock; all
 * allocation and VFS I/O remain outside. Thus a short spinlock suffices and
 * ranking never waits behind a disk operation. The old mutex proposal assumed
 * the lock would span the writer I/O, which the implemented split avoids.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS LEARNED: TEXT, NEVER AN OFFSET
 * ---------------------------------------------------------------------------
 * An entry is the pair (dictionary key, candidate UTF-8), both handed over by
 * ime_commit_source(). Byte offsets into pinyin.dat are NOT usable as an
 * identity, and pinyin_fmt.h says why: regenerate the dictionary and every
 * offset means a different word, so the store would silently promote the wrong
 * characters with no symptom at all.
 *
 * The key is the DICTIONARY key ("nihao"), not the buffer the user typed
 * ("nh"). That is a property of ime_user_weight_fn's contract, and it is worth
 * more than it looks: all four candidate classes reach 你好 through the same
 * key record, so ONE commit under "nh" also promotes 你好 for "nihao", "nih"
 * and "ni'hao". The user teaches the machine a word, not a shortcut.
 *
 * ---------------------------------------------------------------------------
 * THE WEIGHT: ADDITIVE, AND THE CASE IT DELIBERATELY DOES NOT COVER
 * ---------------------------------------------------------------------------
 * bonus = min(count, IME_LEARN_COUNT_MAX) * IME_LEARN_STEP. Flat, in the
 * dictionary's own frequency units, ignoring `base` entirely.
 *
 * pinyin.h offers the alternative -- "a store that wants [to promote 泥 over
 * 你] needs bonus proportional to `base`" -- and that alternative does not
 * work, which is worth stating because it is the obvious next idea:
 *
 *   A bonus proportional to the candidate's OWN base helps whoever is already
 *   winning. 泥 (4,354) needs to pass 你 (234,587); a bonus of base*k/8 per
 *   commit reaches 234,587 after 424 commits, and the same term hands 你 an
 *   extra 29,000 per commit whenever IT is chosen. The rival's frequency is
 *   the quantity that would have to appear in the formula and it is precisely
 *   the one the hook is not shown.
 *
 * So this store is honest about its range instead of pretending a formula
 * covers everything. MEASURED against the shipped dictionary by
 * `make test-imelearn` (tests/unit/ime_learn_test.c, which links the REAL
 * engine and loads the REAL fsroot/ime/pinyin.dat, so every rank it prints is
 * a rank the shipped machine would show):
 *
 *   - NEAR TIES -- which is the case the user actually described, 你好 vs 你号
 *     -- are learned in a handful of commits.
 *   - A FOUR-ORDERS-OF-MAGNITUDE gap is not closed and is not meant to be. A
 *     store that could flip 你 to 泥 in six keystrokes would also flip it back
 *     on six typos, and the dictionary's frequency would stop meaning anything.
 *
 * IME_LEARN_STEP is 256 because that is the increment pinyin.h's own worked
 * example already pins ("an ADDITIVE store (bonus = commits * 256) lands 你好
 * at rank 0 of the 'nh' bucket after 6 commits ... 8 6 5 2 1 1 0 0 0"). Keeping
 * it identical means this store reproduces a trajectory that was measured
 * independently of it -- a cross-check, not a coincidence.
 *
 * ---------------------------------------------------------------------------
 * BOUNDED, BECAUSE A USER TYPES FOR YEARS
 * ---------------------------------------------------------------------------
 * IME_LEARN_SLOTS entries and IME_LEARN_ARENA bytes of string pool, both fixed
 * .bss, both compile-time. When either fills, ime_learn_note() runs an AGE
 * SWEEP rather than refusing to learn: every count is halved and every entry
 * that reaches zero is dropped, then the table and the arena are rebuilt
 * compactly in one pass.
 *
 * Halving is chosen over LRU eviction for a reason that is about behaviour and
 * not about code size: it DECAYS. A word chosen forty times last year and
 * never since falls behind a word chosen four times this week, which is what
 * the user means by "it learns". An LRU would keep the ancient word at full
 * strength until the moment it is evicted outright.
 *
 * Progress is guaranteed and bounded: repeated halving drives every count to
 * zero, so the sweep loops at most 32 times and then the table is empty. It
 * reports on the serial line every time it runs, because a store that silently
 * forgot half of what it knew is exactly the kind of thing that gets diagnosed
 * as "the IME feels worse than it used to".
 *
 * ---------------------------------------------------------------------------
 * PERSISTENCE: ONE WRITER, NEVER ON THE KEYSTROKE
 * ---------------------------------------------------------------------------
 * settings.h states the constraint this machine imposes on anything that
 * remembers: "LogitFS rewrites a WHOLE FILE per write ... There has to be
 * exactly one writer." There is exactly one here -- this file, on the kworker
 * thread -- and /var/ime-learn.conf has no other author.
 *
 * And the keystroke never waits for it. The commit path does table work only
 * and re-arms a one-shot ktimer; the timer callback does nothing but
 * work_queue(), which work.h documents as safe from interrupt context; the
 * kworker does the write. That is the three-tier shape work.h was built for,
 * used exactly as written.
 *
 * The debounce is IME_LEARN_QUIET_MS of no commits. The cost of that choice,
 * stated rather than discovered: a power cut within the debounce window loses
 * the commits inside it. That is bounded by construction and is why toggling
 * the IME off and closing a window both flush immediately -- those are the two
 * moments a user is plausibly about to walk away.
 *
 * ---------------------------------------------------------------------------
 * WHAT THE HOST GATE CANNOT SEE, NAMED SO IT IS NOT ASSUMED AWAY
 * ---------------------------------------------------------------------------
 * `make test-imelearn` compiles this file with -DIME_LEARN_HOST, which is
 * everything above "THE KERNEL HALF" -- the table, the sweep, the serialiser
 * and the parser. The half below it (kmalloc, vfs_write, ktimer, work_queue)
 * has no host at all, so NOTHING in that gate is evidence that a byte ever
 * reached the disk. That claim is a DEVICE claim and it is settled by two
 * boots without -snapshot, not by any host target:
 *
 *   boot 1  toggle the IME on, type a word, pick a candidate that is not
 *           first, commit it several times, toggle the IME off. The serial log
 *           must carry "[ime] window N: pinyin OFF (learned: E entries, C
 *           commits)" with E > 0, and then, within a second,
 *           "[ime] learn: wrote /var/ime-learn.conf -- E entries, C commits, B
 *           bytes". Those two lines are the whole write path: the first proves
 *           the commit reached the table, the second that the kworker reached
 *           the filesystem.
 *   boot 2  the SAME disk image. ime_learn_init() must print
 *           "[ime] learn: /var/ime-learn.conf -- E entries, C commits, 0 lines
 *           rejected" with the SAME E and C, BEFORE any key is pressed. Then
 *           `cat /var/ime-learn.conf` on the machine shows the entry, and
 *           typing the same buffer must put the learned candidate where boot 1
 *           left it rather than where the dictionary would.
 *
 * The rank in boot 2 is the load-bearing half of that. A file that exists and
 * parses proves persistence and NOT application -- which is exactly the failure
 * LEARN_CTL_NOHOOK exists to make visible, and it is invisible to `cat`.
 * =========================================================================== */

/* Entries, and bytes of string pool. Sized against each other rather than
 * picked separately: the shipped dictionary's average (key + candidate) for a
 * multi-character entry is about 11 bytes ("nihao" + 你好 = 5 + 6), so 12 KiB
 * is what 1024 entries actually consume. Sizing the arena independently would
 * guarantee that one of the two limits is decorative -- and the gate confirms
 * the intended one binds first: 4,000 distinct notes stop at 160 live entries,
 * i.e. the SLOT table's 3/4 load factor, with the arena at 960 of 12,288 bytes.
 *
 * THE .bss COST, MEASURED rather than added up in prose. `nm -S` on the built
 * object (2026-08-28, x86_64-elf, -O2):
 *
 *     g_arena          24,576 B   (TWO arenas of IME_LEARN_ARENA -- see the
 *                                  note above g_slot in the .c for why
 *                                  compaction needs a second one)
 *     g_slot           12,288 B   (1024 x 12; sizeof(struct lslot) is 12, not 16)
 *     g_timer, g_work      80 B
 *     eleven counters      37 B
 *     ---------------------------
 *     total            36,981 B   = 36.1 KiB
 *
 * This block used to read "1024 x 16 B of slots + 12 KiB of arena = 28 KiB",
 * which was wrong twice in one sentence and in opposite directions: the slot is
 * 12 bytes, and the arena is doubled. Recorded rather than quietly corrected,
 * because the number a reader budgets against is the one they will quote back. */
#define IME_LEARN_SLOTS 1024u
#define IME_LEARN_ARENA 12288u

/* Frequency units added per commit. See the long note above for why this is
 * flat and why it is 256. */
#define IME_LEARN_STEP 256u

/* The bonus stops growing here. It is a bound on how far the store may move a
 * ranking, and it is also what ime_ui.c publishes to the engine as `max_add`
 * -- pinyin.h's score_bound() prunes with it, so a larger cap costs ranking
 * work on every keystroke whether or not anything was ever learned. Measured:
 * see this line's report for the candidates-considered counts at this value. */
#define IME_LEARN_COUNT_MAX 64u

/* Longest key and candidate an entry may hold. The shipped dictionary's
 * longest key is 48 bytes and its longest candidate is 15 codepoints = 45 UTF-8
 * bytes; IME_CAND_MAXCP (20) at 3 bytes each is 60. Anything longer is REFUSED
 * by ime_learn_note() rather than truncated -- a truncated key is a different
 * word that would then collect somebody else's weight. */
#define IME_LEARN_KEYMAX 48
#define IME_LEARN_CANDMAX 60

/* Milliseconds of no commits before the store is written. */
#define IME_LEARN_QUIET_MS 3000u

/* Where it lives. NOT /etc: /etc is system configuration that a machine's
 * administrator edits and an image ships, and this file is neither -- it is
 * accumulated observation, rewritten by the machine without anyone asking.
 * Putting it in /etc would mean `settings.conf` and a keystroke histogram were
 * the same kind of thing. */
#define IME_LEARN_PATH "/var/ime-learn.conf"
#define IME_LEARN_DIR  "/var"

/* Load the store from disk and make it live. `build_id` is the dictionary's
 * (struct ime_dict.build_id) and is RECORDED, not enforced: entries are keyed
 * on text, so they stay meaningful across a dictionary regeneration. A
 * mismatch is reported on the serial line because it means some entries may
 * now name words the dictionary no longer has, i.e. dead weight occupying a
 * bounded table -- a fact worth knowing and not a reason to throw away what
 * the user taught the machine.
 *
 * Safe to call twice; the second call is a no-op. Thread context only (it
 * reads a file). */
void ime_learn_init(uint32_t build_id);

/* THE RANKING HOOK. Matches ime_user_weight_fn exactly; ime_ui.c installs it
 * with ime_set_user_weight(&st, ime_learn_weight, 0, 256, IME_LEARN_STEP *
 * IME_LEARN_COUNT_MAX). `ctx` is unused -- there is one store, for the same
 * reason there is one settings file.
 *
 * HOT: the engine calls this once per candidate that survives its prune, which
 * on a one-letter buffer is hundreds of times per keystroke. One FNV-1a over
 * (key + candidate) and one open-addressed probe; no allocation, no lock, no
 * call out. `base` is deliberately ignored -- see the note above. */
uint32_t ime_learn_weight(void *ctx, const char *key, int keylen,
                          const uint8_t *cand_utf8, int cand_len,
                          uint32_t base);

/* THE TRAINING SIGNAL: the user just chose `cand_utf8` under `key`. Bumps the
 * count, ages the table if it is full, and schedules a write for
 * IME_LEARN_QUIET_MS from now (re-arming, so a burst of typing writes once).
 * Never blocks, never allocates, never touches the disk.
 *
 * Call ONLY for a commit that names a real dictionary entry -- ime_commit_source()
 * returning 1. A raw-letter commit (Enter) and a TIER_SEG composition have no
 * single source and there is nothing honest to attribute a weight to. */
void ime_learn_note(const char *key, int keylen,
                    const uint8_t *cand_utf8, int cand_len);

/* Write NOW-ish rather than after the debounce: the IME was toggled off, or
 * the window owning a composition went away. Still asynchronous -- it queues
 * the kworker and returns, so even this path cannot make a keystroke wait on
 * the disk. No-op if nothing changed since the last successful write. */
void ime_learn_flush_soon(void);

/* Introspection for a boot harness and for a serial line: entries held, total
 * commits recorded, and whether the in-memory table differs from what is on
 * disk. Any pointer may be NULL. */
void ime_learn_stats(uint32_t *entries, uint32_t *commits, int *dirty);

/* ---- the file, as a pure function of the table ---------------------------
 *
 * Exposed rather than static because they are the half of this store that a
 * HOST program can check: a round trip (learn -> serialise -> reset -> parse ->
 * the same weights) needs no kernel, no disk and no QEMU. The kernel's writer
 * is the only in-tree caller; see the note at the top of ime_learn.c about the
 * -DIME_LEARN_HOST split.
 *
 * ime_learn_serialise() returns bytes written or -1 if `max` is too small, and
 * makes NO call that can sleep -- that is a correctness requirement, not a
 * performance one (see the BKL note above). ime_learn_parse() returns entries
 * accepted and reports through `*rejected` how many non-comment lines did not
 * parse, which is how a damaged file describes its own damage. */
int ime_learn_serialise(char *buf, int max);
int ime_learn_parse(const char *buf, int len, int *rejected);

#endif /* LOGIT_IME_LEARN_H */
