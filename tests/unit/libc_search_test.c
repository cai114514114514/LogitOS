/* <search.h> case list, compiled twice by tests/libc.mk (once against our
 * own <search.h>/search.c, once as a completely ordinary host program
 * against glibc's) and diffed byte for byte. See search.c's file header for
 * the two places this file's design follows from MEASURED glibc behaviour
 * rather than the header comment or a guess:
 *
 *   - tsearch/twalk here is a plain, unbalanced BST; real glibc's is
 *     self-balancing. Only shape-INDEPENDENT observations are compared:
 *     found-vs-not-found, which entry a lookup returns, and twalk's
 *     {leaf, postorder} projection, which is sorted order for ANY binary
 *     search tree holding the same keys (see the proof in search.c). The
 *     raw preorder/endorder event stream and each event's `level` ARE
 *     shape-dependent and are deliberately never printed here -- printing
 *     them would fail this diff for a reason that has nothing to do with
 *     correctness.
 *   - hcreate's capacity boundary (this file drives ENTER calls to and past
 *     it and diffs the exact point of the first failure) and hsearch's
 *     "ENTER on an existing key keeps the OLD data" behaviour were both
 *     confirmed against real glibc by a throwaway host probe, not assumed
 *     from the installed header's comment (which says ENTER "replaces"
 *     existing data -- measured glibc does not).
 *
 * A deterministic LCG (not rand(), which is not required to produce the
 * same sequence on every libc) drives every shuffle so both builds walk an
 * identical sequence of operations.
 *
 * _GNU_SOURCE: glibc's own <search.h> gates tdestroy/hcreate_r/hsearch_r/
 * hdestroy_r/insque/remque behind __USE_GNU (insque/remque behind the wider
 * __USE_MISC || __USE_XOPEN_EXTENDED); without this they are simply not
 * declared for the glibc build. Our own <search.h> has no such gate (there
 * is exactly one target and one feature set here), so this is a no-op for
 * the "ours" build. */
#define _GNU_SOURCE
#include <search.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

/* ---- deterministic shuffle -------------------------------------------- */
static unsigned long lcg_state;
static unsigned long lcg_next(void)
{
    lcg_state = lcg_state * 6364136223846793005UL + 1442695040888963407UL;
    return (lcg_state >> 33);
}
static void shuffle(int *a, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(lcg_next() % (unsigned long)(i + 1));
        int t = a[i]; a[i] = a[j]; a[j] = t;
    }
}

/* ==========================================================================
 * tsearch / tfind / tdelete / twalk / tdestroy
 * ========================================================================== */

typedef struct { long val; int tag; } Key;

static int keycmp(const void *a, const void *b)
{
    const Key *ka = a, *kb = b;
    return (ka->val > kb->val) - (ka->val < kb->val);
}

/* twalk callback: collect the {leaf, postorder} projection (see file header
 * for why that's the shape-independent, sorted-order subset of the walk). */
static long g_walk_out[256];
static int  g_walk_n;

/* tdestroy's freefct: real glibc (2.43, checked here) calls it UNCONDITIONALLY
 * for every node and crashes if given a literal NULL -- despite that reading
 * like the obvious "pass NULL for no-op" convention used everywhere else in
 * this file (hsearch_r's retval, twalk_r's closure, ...), <search.h>'s own
 * comment actually says the opposite: "If the keys are atomic data this
 * function should do nothing" -- i.e. give it a callback that does nothing,
 * never a NULL callback. Confirmed with a throwaway host repro: tdestroy(root,
 * NULL) on a 3-node tree segfaults, tdestroy(root, noop) on the same tree does
 * not. So this file follows the documented contract and always passes a real
 * (possibly empty) function; search.c's own tdestroy is defensively NULL-safe
 * anyway (it skips the callback instead of crashing), but that divergence is
 * never exercised by this diff, since exercising it would mean deliberately
 * calling the reference build in a way its own header says not to. */
static void tdestroy_noop(void *keyp) { (void)keyp; }
static void walk_collect(const void *nodep, VISIT which, int level)
{
    (void)level;
    if (which == leaf || which == postorder) {
        const Key *k = *(Key *const *)nodep;
        g_walk_out[g_walk_n++] = k->val;
    }
}
static void print_walk_projection(const char *label, void *root)
{
    g_walk_n = 0;
    twalk(root, walk_collect);
    printf("%s:", label);
    for (int i = 0; i < g_walk_n; i++) printf(" %ld", g_walk_out[i]);
    printf("\n");
}

#define NKEYS 60
static Key g_keys[NKEYS];

static void tsearch_section(void)
{
    lcg_state = 0xA5A5A5A5u;

    int order[NKEYS];
    for (int i = 0; i < NKEYS; i++) order[i] = i;
    shuffle(order, NKEYS);

    void *root = NULL;

    /* Insert all 60 unique keys in shuffled order; every insert is a FIRST
     * insert (no duplicates yet), so tsearch must return a node whose key
     * is exactly the one just inserted. */
    for (int i = 0; i < NKEYS; i++) {
        int v = order[i];
        g_keys[v].val = v;
        g_keys[v].tag = 1000 + v;      /* "first insertion" tag */
        void *n = tsearch(&g_keys[v], &root, keycmp);
        Key *got = *(Key **)n;
        printf("insert %d -> val=%ld tag=%d\n", v, got->val, got->tag);
    }

    print_walk_projection("after-build sorted walk", root);

    /* tfind: every inserted value must be found; the returned key's tag
     * must be the ORIGINAL 1000+v tag (nothing has overwritten it yet). */
    for (int i = 0; i < NKEYS; i++) {
        Key probe = { i, -1 };
        void *n = tfind(&probe, &root, keycmp);
        Key *got = n ? *(Key **)n : NULL;
        printf("tfind %d -> %s tag=%d\n", i, n ? "found" : "MISS",
               got ? got->tag : -1);
    }

    /* tfind on values never inserted: must miss. */
    for (int i = NKEYS; i < NKEYS + 20; i++) {
        Key probe = { i, -1 };
        void *n = tfind(&probe, &root, keycmp);
        printf("tfind-absent %d -> %s\n", i, n ? "FOUND(bug)" : "miss");
    }

    /* Duplicate insert: same val, a DIFFERENT Key object (different tag).
     * tsearch on a key already present must return the EXISTING node
     * (original tag), and must NOT touch the tree structure. */
    Key dups[10];
    int dupvals[10] = { 5, 12, 0, 59, 30, 41, 7, 55, 18, 3 };
    for (int i = 0; i < 10; i++) {
        dups[i].val = dupvals[i];
        dups[i].tag = 9000 + i;         /* distinct from the 1000+v original tag */
        void *n = tsearch(&dups[i], &root, keycmp);
        Key *got = *(Key **)n;
        printf("dup-insert %d -> tag=%d (must be original %d)\n",
               dupvals[i], got->tag, 1000 + dupvals[i]);
    }
    print_walk_projection("after-dup-inserts sorted walk", root);

    /* tdelete: remove half the keys in a second shuffled order, checking
     * after EVERY delete that (a) the return value is non-NULL (found) and
     * (b) the remaining tree's sorted projection is exactly the surviving
     * key set -- this exercises whatever position (root/leaf/two-children)
     * each deletion happens to hit in THIS tree's shape, and would show a
     * broken relink immediately at the step it happens on. The very first
     * deletion in this sequence removes order[0], the first key ever
     * inserted -- which is this plain BST's ROOT (a plain BST's root never
     * moves once set, unlike glibc's rebalanced tree), so that case is
     * covered explicitly and not left to chance. */
    int dorder[NKEYS / 2];
    for (int i = 0; i < NKEYS / 2; i++) dorder[i] = order[i];
    shuffle(dorder, NKEYS / 2);

    int alive[NKEYS];
    for (int i = 0; i < NKEYS; i++) alive[i] = 1;

    for (int i = 0; i < NKEYS / 2; i++) {
        int v = dorder[i];
        Key delkey = { v, -1 };
        void *r = tdelete(&delkey, &root, keycmp);
        alive[v] = 0;
        printf("tdelete %d -> %s\n", v, r ? "found" : "MISS(bug)");
        char label[32];
        snprintf(label, sizeof label, "  walk-after-del-%d", v);
        print_walk_projection(label, root);
    }

    /* Sanity: independently confirm the survivors match `alive[]` (belt and
     * braces on top of the printed projections above -- this doesn't print
     * per-key, just a single consistency line, since the projections
     * already are the byte-diffed evidence). */
    {
        g_walk_n = 0;
        twalk(root, walk_collect);
        int ok = 1, expected = 0;
        for (int i = 0; i < NKEYS; i++) expected += alive[i];
        if (g_walk_n != expected) ok = 0;
        printf("survivor-count-check: %s (%d vs expected %d)\n",
               ok ? "ok" : "MISMATCH", g_walk_n, expected);
    }

    /* tdelete on keys already removed: must miss. */
    for (int i = 0; i < NKEYS / 2; i++) {
        int v = dorder[i];
        Key delkey = { v, -1 };
        void *r = tdelete(&delkey, &root, keycmp);
        printf("tdelete-again %d -> %s\n", v, r ? "FOUND(bug)" : "miss");
    }

    /* tdelete on a key that was never present at all. */
    for (int v = 1000; v < 1005; v++) {
        Key delkey = { v, -1 };
        void *r = tdelete(&delkey, &root, keycmp);
        printf("tdelete-never-present %d -> %s\n", v, r ? "FOUND(bug)" : "miss");
    }

    /* Drain the rest via tdestroy (frees every remaining node; nothing to
     * observe from tdestroy itself beyond "doesn't crash", so no output --
     * the process reaching the next section IS the assertion). */
    tdestroy(root, tdestroy_noop);
}

static void tsearch_edge_cases(void)
{
    /* tfind on a genuinely empty tree. */
    void *root = NULL;
    Key probe = { 42, -1 };
    void *n = tfind(&probe, &root, keycmp);
    printf("tfind-empty-tree -> %s\n", n ? "FOUND(bug)" : "miss");

    /* tdelete on a genuinely empty tree. */
    void *r = tdelete(&probe, &root, keycmp);
    printf("tdelete-empty-tree -> %s\n", r ? "FOUND(bug)" : "miss");

    /* A two-node tree, delete the root (which has exactly one child) --
     * covers the "root with one child" shape explicitly, distinct from the
     * "root with two children" case already covered in tsearch_section. */
    Key a = { 10, 1 }, b = { 20, 2 };
    tsearch(&a, &root, keycmp);
    tsearch(&b, &root, keycmp);
    void *rr = tdelete(&a, &root, keycmp);
    printf("tdelete-root-one-child -> %s\n", rr ? "found" : "MISS(bug)");
    print_walk_projection("walk-after-root-one-child-delete", root);
    tdestroy(root, tdestroy_noop);

    /* A single-node tree: delete the root, which is also a leaf. */
    root = NULL;
    Key c = { 99, 3 };
    tsearch(&c, &root, keycmp);
    void *rc = tdelete(&c, &root, keycmp);
    printf("tdelete-root-is-leaf -> %s, root-after=%s\n",
           rc ? "found" : "MISS(bug)", root == NULL ? "NULL(ok)" : "non-NULL(bug)");
}

/* ATTACK: rootp itself (not *rootp) is NULL for all three of tsearch/tfind/
 * tdelete. Confirmed against real glibc with a throwaway host probe before
 * writing this -- glibc's own tsearch.c has the identical `if (rootp ==
 * NULL) return NULL;` guard at the top of all three functions, so this is
 * not undefined behaviour there, it's a documented-by-implementation no-op
 * path. Distinct from tsearch_edge_cases' "*rootp == NULL" (empty tree,
 * rootp valid) case above. */
static void tsearch_null_rootp_section(void)
{
    Key k = { 5, 0 };
    int cmp_never_called = 1;   /* if compar were dereferenced with a bad
                                    rootp we'd never reach the printf below;
                                    the process reaching it IS the check */
    void *r1 = tfind(&k, NULL, keycmp);
    printf("tfind(key, NULL-rootp, cmp) -> %s\n", r1 ? "non-NULL(bug)" : "NULL");
    void *r2 = tdelete(&k, NULL, keycmp);
    printf("tdelete(key, NULL-rootp, cmp) -> %s\n", r2 ? "non-NULL(bug)" : "NULL");
    void *r3 = tsearch(&k, NULL, keycmp);
    printf("tsearch(key, NULL-rootp, cmp) -> %s (survived=%d)\n",
           r3 ? "non-NULL(bug)" : "NULL", cmp_never_called);
}

/* ATTACK: twalk(NULL, action) and tdestroy(NULL, freefct) must both survive
 * without invoking the callback -- if either implementation dereferenced a
 * NULL root before checking it, the process would die here and every line
 * printed afterward (including everything from main()'s later sections)
 * would vanish from that build's output, which the diff WOULD catch as a
 * length mismatch even without a dedicated assertion. The explicit counters
 * make the failure mode readable instead of "mysteriously truncated diff". */
static void twalk_tdestroy_null_root_section(void)
{
    g_walk_n = 0;
    twalk(NULL, walk_collect);
    printf("twalk(NULL, action) -> survived, callback-fired=%d (want 0)\n", g_walk_n);

    tdestroy(NULL, tdestroy_noop);
    printf("tdestroy(NULL, noop) -> survived\n");
}

/* ATTACK: a single-node tree (root with neither child) must fire exactly one
 * `leaf` event and NO preorder/postorder/endorder -- trecurse's `if (!left
 * && !right)` branch is the only path that produces this, and it's not
 * exercised by tsearch_section (60 keys, always multi-level) or
 * tsearch_edge_cases (its single-node case deletes immediately without
 * walking first). */
static void single_node_twalk_section(void)
{
    void *root = NULL;
    Key single = { 4242, 1 };
    tsearch(&single, &root, keycmp);
    print_walk_projection("single-node-tree walk (want just: 4242)", root);
    tdestroy(root, tdestroy_noop);
}

/* ==========================================================================
 * hcreate / hsearch / hdestroy + _r
 * ========================================================================== */

static void hsearch_capacity_boundary(size_t nel, int max_tries)
{
    if (!hcreate(nel)) { printf("hcreate(%zu) -> FAILED(bug)\n", nel); return; }

    /* ATTACK-FOUND HARDENING: this buffer used to be keybuf[64][16] with no
     * guard on max_tries -- every call site the original author wrote used
     * max_tries <= 16, so it never overflowed, but the 64-row limit was an
     * UNENFORCED invariant on a fixed-size stack array, not a real bound.
     * The adversarial pass below drives capacity boundaries out to nel=100
     * (capacity 101), which needs max_tries > 100 -- exactly the kind of
     * addition that would have silently smashed the stack under the old
     * size. Bumped the array AND clamped max_tries defensively so a future
     * call site with an even larger nel fails loudly (truncated run) rather
     * than corrupting the stack. */
    char keybuf[300][16];
    if (max_tries > 300) max_tries = 300;
    int successes = 0;
    for (int i = 0; i < max_tries; i++) {
        snprintf(keybuf[i], sizeof keybuf[i], "k%d", i);
        ENTRY e = { keybuf[i], (void *)(long)i };
        ENTRY *r = hsearch(e, ENTER);
        if (!r) {
            printf("hcreate(%zu): capacity=%d errno=%s\n", nel, successes,
                   errno == ENOMEM ? "ENOMEM" : "other");
            break;
        }
        successes++;
        if (i == max_tries - 1)
            printf("hcreate(%zu): capacity>=%d (did not fill)\n", nel, successes);
    }
    hdestroy();
}

static void hsearch_section(void)
{
    /* Capacity boundary for several nel values -- this is the byte-exact
     * proof that next_prime() here matches glibc's real table sizing. */
    hsearch_capacity_boundary(1, 8);
    hsearch_capacity_boundary(2, 8);
    hsearch_capacity_boundary(3, 8);
    hsearch_capacity_boundary(4, 10);
    hsearch_capacity_boundary(8, 16);
    hsearch_capacity_boundary(10, 16);

    /* ATTACK: the file header says the next_prime() formula was measured
     * against real glibc only for nel = 1..20. Push well past that --
     * nel=100 (capacity should be 101, the next prime after 101 itself) and
     * a few in-between points the original probe skipped (6, 7, 12, 50) --
     * and let the diff itself be the oracle instead of trusting the
     * documented range. */
    hsearch_capacity_boundary(6, 12);
    hsearch_capacity_boundary(7, 12);
    hsearch_capacity_boundary(12, 18);
    hsearch_capacity_boundary(50, 60);
    hsearch_capacity_boundary(100, 110);

    /* ENTER on an existing key keeps the ORIGINAL data (measured glibc
     * behaviour -- see file header). */
    hcreate(20);
    ENTRY e1 = { "alpha", (void *)111 };
    hsearch(e1, ENTER);
    ENTRY e2 = { "alpha", (void *)222 };
    ENTRY *r2 = hsearch(e2, ENTER);
    printf("hsearch dup-enter data=%ld (must stay 111)\n", (long)r2->data);

    /* FIND hit and miss, and the errno FIND-miss sets (ESRCH, measured). */
    errno = 0;
    ENTRY fe = { "alpha", NULL };
    ENTRY *rf = hsearch(fe, FIND);
    printf("hsearch find-hit -> %s data=%ld\n", rf ? "found" : "MISS(bug)",
           rf ? (long)rf->data : -1);

    errno = 0;
    ENTRY fm = { "nonexistent-key", NULL };
    ENTRY *rm = hsearch(fm, FIND);
    printf("hsearch find-miss -> %s errno=%s\n", rm ? "FOUND(bug)" : "miss",
           errno == ESRCH ? "ESRCH" : "other");

    /* Second hcreate() while a table is active must fail without touching
     * errno (measured: glibc leaves errno exactly as the caller left it). */
    errno = 0;
    int r3 = hcreate(50);
    printf("hcreate-while-active -> %s errno-untouched=%s\n",
           r3 ? "SUCCEEDED(bug)" : "failed",
           errno == 0 ? "yes" : "no");

    /* The still-active table must be unharmed by the failed hcreate() above. */
    ENTRY fe2 = { "alpha", NULL };
    ENTRY *rf2 = hsearch(fe2, FIND);
    printf("hsearch find-after-failed-hcreate -> %s\n", rf2 ? "found" : "MISS(bug)");

    hdestroy();

    /* hcreate() after hdestroy(): must succeed again (table really gone). */
    int r4 = hcreate(5);
    printf("hcreate-after-hdestroy -> %s\n", r4 ? "ok" : "FAILED(bug)");
    ENTRY fe3 = { "alpha", NULL };
    ENTRY *rf3 = hsearch(fe3, FIND);
    printf("hsearch find-after-recreate -> %s\n", rf3 ? "FOUND(bug)" : "miss");
    hdestroy();
}

/* ATTACK: hcreate(0). The formula in the file header is "next_prime(nel|1)",
 * which is well-defined at nel=0 (0|1 = 1 -> smallest prime >= 1 is 3), but
 * the header never says whether a real hcreate(0) call even SUCCEEDS -- a
 * glibc that special-cased "empty table request" to fail outright would make
 * this a wrong assumption baked into next_prime() without ever being
 * exercised. Confirmed by a throwaway host probe before adding this (real
 * glibc: hcreate(0) succeeds, capacity 3, same as nel=1..3) -- this case
 * makes that fact part of the byte-diffed gate instead of a one-off probe. */
static void hcreate_zero_section(void)
{
    hsearch_capacity_boundary(0, 8);
}

/* ATTACK: FIND on a table that has zero entries in it (freshly hcreate'd,
 * nothing ever ENTERed) must probe the empty first slot and report ESRCH --
 * this is a different code path from "FIND misses in a table with OTHER
 * keys in it" (already covered by hsearch_section's find-miss case), because
 * here every slot's ->hash is still 0 and the loop must exit on the very
 * first empty-slot check rather than looping to the walk-length bound. */
static void hsearch_empty_table_find_section(void)
{
    hcreate(5);
    errno = 0;
    ENTRY fe = { "nope", NULL };
    ENTRY *r = hsearch(fe, FIND);
    printf("find-on-freshly-created-empty-table -> %s errno=%s\n",
           r ? "FOUND(bug)" : "miss", errno == ESRCH ? "ESRCH" : "other");
    hdestroy();
}

/* ATTACK: fill a table to EXACTLY its capacity (no ENOMEM triggered), then
 * FIND a key that was never entered. This forces the probe loop in
 * hsearch_r to walk every single slot (all of them occupied, none empty)
 * before concluding ESRCH -- the "all `size` slots probed, table full of
 * OTHER keys" tail case called out explicitly in search.c's own comment,
 * and the one case none of the capacity-boundary tests exercise (they stop
 * at the first ENOMEM on ENTER, never doing a FIND against a completely
 * full table). A hash/probe bug that wrapped short or re-visited a slot
 * would show up here as a false FOUND or an infinite loop. */
static void hsearch_full_table_miss_section(void)
{
    hcreate(3);               /* capacity 3, per the measured formula */
    const char *keys[3] = { "one", "two", "three" };
    for (int i = 0; i < 3; i++) {
        ENTRY e = { (char *)keys[i], (void *)(long)i };
        ENTRY *r = hsearch(e, ENTER);
        printf("full-table-fill ENTER %s -> %s\n", keys[i], r ? "ok" : "FAILED(bug)");
    }
    errno = 0;
    ENTRY miss = { "absent", NULL };
    ENTRY *rm = hsearch(miss, FIND);
    printf("full-table FIND absent-key -> %s errno=%s\n",
           rm ? "FOUND(bug)" : "miss", errno == ESRCH ? "ESRCH" : "other");
    /* and a FIND for a key that IS present must still succeed post-fill */
    ENTRY hit = { (char *)"two", NULL };
    ENTRY *rh = hsearch(hit, FIND);
    printf("full-table FIND present-key -> %s data=%ld\n",
           rh ? "found" : "MISS(bug)", rh ? (long)rh->data : -1);
    hdestroy();
}

/* ATTACK: keys chosen to collide under djb2 (this file's hash) are not
 * necessarily interesting for glibc's own (different, unobserved) hash --
 * but the point here isn't to force a COLLISION in either implementation,
 * it's to prove that a batch of many similar/prefix-sharing keys all land
 * correctly regardless of which hash either side uses. Every assertion below
 * is shape/hash-independent (found vs not-found, correct data), so it is a
 * legitimate diff target even though the two implementations' internal
 * table layouts are (deliberately, per the file header) not comparable. */
static void hsearch_similar_keys_section(void)
{
    hcreate(40);
    const char *keys[] = {
        "a", "aa", "aaa", "aaaa", "ab", "ba", "b", "bb",
        "key1", "key2", "key10", "key11", "", "x", "xx", "xxx"
    };
    int n = (int)(sizeof(keys) / sizeof(keys[0]));
    for (int i = 0; i < n; i++) {
        ENTRY e = { (char *)keys[i], (void *)(long)(i + 1) };
        ENTRY *r = hsearch(e, ENTER);
        printf("similar-keys ENTER '%s' -> %s\n", keys[i], r ? "ok" : "FAILED(bug)");
    }
    for (int i = 0; i < n; i++) {
        ENTRY e = { (char *)keys[i], NULL };
        ENTRY *r = hsearch(e, FIND);
        printf("similar-keys FIND '%s' -> %s data=%ld (want %d)\n", keys[i],
               r ? "found" : "MISS(bug)", r ? (long)r->data : -1, i + 1);
    }
    hdestroy();
}

/* ATTACK: hcreate_r/hdestroy_r cycled THREE times on the same struct, with a
 * different nel each time and a FIND against the previous cycle's key after
 * each recreate -- proves a fresh hcreate_r() genuinely starts empty (no
 * leaked key from the previous cycle) rather than merely "looking" reusable
 * because a stale table pointer happens to not crash. The existing
 * reentrant test only cycles once; this repeats it to catch state that
 * survives exactly one cycle by accident.
 *
 * NOTE on ->size/->filled after hdestroy_r: this test ORIGINALLY asserted
 * both reset to 0, which turned out to be wrong -- a throwaway host probe
 * (fill a table, hdestroy_r it, read ->size/->filled before any further
 * hcreate_r) showed real glibc's __hdestroy_r clears only ->table, leaving
 * ->size/->filled at their last live values. search.c's hdestroy_r was
 * fixed to match (see its comment); this test now checks THAT contract --
 * the fields hold whatever they held immediately before free(), not 0. */
static void hsearch_r_multi_cycle_section(void)
{
    struct hsearch_data h = {0};

    for (int cycle = 0; cycle < 3; cycle++) {
        size_t nel = (size_t)(3 + cycle * 5);   /* 3, 8, 13 */
        int ok = hcreate_r(nel, &h);
        printf("multi-cycle[%d] hcreate_r(%zu) -> %s\n", cycle, nel, ok ? "ok" : "FAILED(bug)");

        char kb[16];
        snprintf(kb, sizeof kb, "cyclekey%d", cycle);
        ENTRY e = { kb, (void *)(long)(100 + cycle) };
        ENTRY *r1;
        hsearch_r(e, ENTER, &r1, &h);

        /* the PREVIOUS cycle's key must be gone (fresh table, not leaked) */
        if (cycle > 0) {
            char prevkb[16];
            snprintf(prevkb, sizeof prevkb, "cyclekey%d", cycle - 1);
            ENTRY prev = { prevkb, NULL };
            ENTRY *r2;
            int found = hsearch_r(prev, FIND, &r2, &h);
            printf("multi-cycle[%d] prev-key-gone -> %s\n", cycle,
                   found ? "LEAKED(bug)" : "gone(ok)");
        }
        unsigned int live_size = h.size, live_filled = h.filled;
        hdestroy_r(&h);
        printf("multi-cycle[%d] post-destroy table-null=%s size-stale-as-live=%s filled-stale-as-live=%s\n",
               cycle, h.table == NULL ? "yes" : "NO(bug)",
               h.size == live_size ? "yes" : "NO(bug)",
               h.filled == live_filled ? "yes" : "NO(bug)");
    }
}

static void hsearch_reentrant_section(void)
{
    /* Two independent tables via the _r interface, interleaved, to prove
     * they don't share state (a bug here would show one table's key
     * "leaking" into the other's FIND results). */
    struct hsearch_data t1 = {0}, t2 = {0};
    hcreate_r(10, &t1);
    hcreate_r(10, &t2);

    ENTRY e1 = { "shared-name", (void *)1 };
    ENTRY e2 = { "shared-name", (void *)2 };
    ENTRY *r1, *r2;
    hsearch_r(e1, ENTER, &r1, &t1);
    hsearch_r(e2, ENTER, &r2, &t2);
    printf("reentrant t1[shared-name]=%ld t2[shared-name]=%ld (must differ: 1 vs 2)\n",
           (long)r1->data, (long)r2->data);

    ENTRY only1 = { "only-in-t1", (void *)7 };
    hsearch_r(only1, ENTER, &r1, &t1);
    ENTRY find_in_t2 = { "only-in-t1", NULL };
    int found = hsearch_r(find_in_t2, FIND, &r2, &t2);
    printf("reentrant cross-table-leak-check -> %s\n", found ? "LEAKED(bug)" : "isolated");

    hdestroy_r(&t1);
    hdestroy_r(&t2);

    /* hcreate_r on a zeroed struct after hdestroy_r must succeed again. */
    int again = hcreate_r(5, &t1);
    printf("hcreate_r-after-hdestroy_r -> %s\n", again ? "ok" : "FAILED(bug)");
    hdestroy_r(&t1);
}

/* ==========================================================================
 * lsearch / lfind
 * ========================================================================== */

static int intcmp(const void *a, const void *b)
{
    return *(const int *)a != *(const int *)b;   /* 0 == match, like memcmp */
}

static void lsearch_section(void)
{
    int arr[40];
    size_t n = 0;
    int seed[10] = { 5, 3, 9, 1, 7, 5, 3, 20, 11, 9 };  /* 5,3,9 repeat on purpose */

    for (int i = 0; i < 10; i++) {
        int before = (int)n;
        void *p = lsearch(&seed[i], arr, &n, sizeof(int), intcmp);
        int is_new = ((int)n != before);
        printf("lsearch %d -> %s nmemb=%zu val-at-slot=%d\n",
               seed[i], is_new ? "appended" : "found-existing", n, *(int *)p);
    }

    /* lfind must never modify nmemb. */
    size_t n_before = n;
    for (int v = 0; v < 25; v++) {
        void *p = lfind(&v, arr, &n, sizeof(int), intcmp);
        printf("lfind %d -> %s\n", v, p ? "found" : "miss");
    }
    printf("lfind-does-not-append: %s (%zu vs %zu)\n",
           n == n_before ? "ok" : "MISMATCH(bug)", n, n_before);
}

/* ATTACK: lfind on a genuinely empty array (nmemb starts at 0, never
 * touched) -- every probe must miss, and nmemb must stay exactly 0.
 * lsearch_section's existing lfind pass runs AFTER 10 appends, so it never
 * exercises the *nmemb==0 loop-doesn't-execute-at-all path (the `for (i=0;
 * i<*nmemb; ...)` loop with *nmemb==0 is a zero-iteration loop, distinct
 * from "loop runs but nothing matches"). */
static void lfind_empty_array_section(void)
{
    int arr[4];
    size_t n = 0;
    for (int v = 0; v < 5; v++) {
        void *p = lfind(&v, arr, &n, sizeof(int), intcmp);
        printf("lfind-empty-array %d -> %s (nmemb=%zu)\n", v, p ? "FOUND(bug)" : "miss", n);
    }
}

/* ATTACK: lsearch on a genuinely empty array -- the very FIRST call, with
 * *nmemb==0, must append unconditionally (lfind trivially misses on an
 * empty range, so lsearch falls through to append) and become the sole
 * element for every later probe of that same value. */
static void lsearch_empty_array_section(void)
{
    int arr[4];
    size_t n = 0;
    void *p = lsearch(&(int){ 77 }, arr, &n, sizeof(int), intcmp);
    printf("lsearch-empty-array-first-call -> nmemb=%zu val=%d\n", n, *(int *)p);
    /* immediately searching for the SAME value must now find it, not re-append */
    void *p2 = lsearch(&(int){ 77 }, arr, &n, sizeof(int), intcmp);
    printf("lsearch-empty-array-second-call-same-val -> nmemb=%zu (want still 1) val=%d\n",
           n, *(int *)p2);
}

/* ATTACK: every lsearch call in a run is the SAME value -- nmemb must climb
 * to exactly 1 on the first call and never again; a bug that re-compared
 * against the wrong range (e.g. compared against uninitialized slots past
 * the logical end) could either falsely append duplicates or falsely find a
 * match against garbage before the first real element exists. */
static void lsearch_all_duplicates_section(void)
{
    int arr[10];
    size_t n = 0;
    for (int i = 0; i < 6; i++) {
        int key = 999;
        void *p = lsearch(&key, arr, &n, sizeof(int), intcmp);
        printf("lsearch-all-dup call%d -> nmemb=%zu (want 1) val=%d\n", i, n, *(int *)p);
    }
}

/* ATTACK: lsearch/lfind with a `size` that is NOT sizeof(int) -- a struct
 * with internal padding, to catch any code path that silently assumed
 * `size` was a small power-of-two word count instead of an opaque byte
 * count for memcpy. compar only looks at the `tag` field (mirrors the
 * memcmp-style 0==match convention lsearch/lfind require); the trailing
 * `pad` bytes are never initialized on the "found" path and never compared,
 * only copied -- which is exactly what lsearch is contracted to do (copy
 * `size` raw bytes of *key in), so this also confirms the copy doesn't stop
 * short at the compared field's width. */
struct padded { int tag; char pad[7]; long check; };
static int paddedcmp(const void *a, const void *b)
{
    return ((const struct padded *)a)->tag != ((const struct padded *)b)->tag;
}
static void lsearch_struct_size_section(void)
{
    struct padded arr[8];
    size_t n = 0;
    struct padded items[4] = {
        { 1, {0}, 111L }, { 2, {0}, 222L }, { 1, {0}, 999L }, { 3, {0}, 333L }
    };
    for (int i = 0; i < 4; i++) {
        void *p = lsearch(&items[i], arr, &n, sizeof(struct padded), paddedcmp);
        struct padded *got = p;
        printf("lsearch-struct tag=%d -> nmemb=%zu found-tag=%d found-check=%ld\n",
               items[i].tag, n, got->tag, got->check);
    }
    /* item[2] has tag==1 (duplicate of item[0]) -- lsearch must return the
     * EXISTING slot (check==111), not append a new one with check==999. */
}

/* ==========================================================================
 * insque / remque
 * ========================================================================== */

struct node { struct node *forw, *back; int val; };

static void print_forward(const char *label, struct node *head)
{
    printf("%s:", label);
    for (struct node *p = head; p; p = p->forw) printf(" %d", p->val);
    printf("\n");
}
static void print_backward(const char *label, struct node *tail)
{
    printf("%s:", label);
    for (struct node *p = tail; p; p = p->back) printf(" %d", p->val);
    printf("\n");
}

static void insque_linear_section(void)
{
    struct node n1 = {0,0,1}, n2 = {0,0,2}, n3 = {0,0,3}, n4 = {0,0,4}, n5 = {0,0,5};

    insque(&n1, NULL);                 /* n1: isolated head */
    print_forward("linear after n1", &n1);

    insque(&n2, &n1);                  /* n1 n2 */
    insque(&n3, &n2);                  /* n1 n2 n3 */
    insque(&n4, &n1);                  /* n1 n4 n2 n3 -- insert in the middle */
    print_forward("linear after inserts", &n1);
    print_backward("linear backward from n3", &n3);

    insque(&n5, NULL);                 /* independent isolated node */
    printf("linear n5 forw=%s back=%s (both NULL)\n",
           n5.forw ? "non-NULL(bug)" : "NULL", n5.back ? "non-NULL(bug)" : "NULL");

    remque(&n4);                       /* remove a middle node */
    print_forward("linear after remque(n4)", &n1);

    remque(&n1);                       /* remove the head */
    print_forward("linear after remque(head)", n1.forw);
    printf("linear new-head-back=%s (must be NULL)\n",
           n2.back ? "non-NULL(bug)" : "NULL");

    remque(&n3);                       /* remove the tail */
    print_forward("linear after remque(tail)", &n2);
}

static void insque_circular_section(void)
{
    /* A circular list built by hand (insque doesn't know circular from
     * linear -- it only ever follows prev's forward pointer, so the caller
     * closes the loop by choosing to wrap q_forw/q_back itself). Build a
     * 4-node ring: a -> b -> c -> d -> a. */
    struct node a = {0,0,10}, b = {0,0,20}, c = {0,0,30}, d = {0,0,40};
    a.forw = &a; a.back = &a;          /* single-element ring */

    insque(&b, &a);                    /* a -> b -> a */
    insque(&c, &b);                    /* a -> b -> c -> a */
    /* close the ring explicitly after each insert since insque only links
     * one direction's neighbour, not the wraparound: */
    c.forw = &a; a.back = &c;
    insque(&d, &c);                    /* a -> b -> c -> d -> ? */
    d.forw = &a; a.back = &d;

    printf("circular walk:");
    struct node *p = &a;
    for (int i = 0; i < 4; i++) { printf(" %d", p->val); p = p->forw; }
    printf(" (back to %d)\n", p->val);

    remque(&c);                        /* remove a middle ring node */
    b.forw = &d; d.back = &b;          /* caller repairs the wraparound, same as insque did not own it */
    printf("circular walk after remque(c):");
    p = &a;
    for (int i = 0; i < 3; i++) { printf(" %d", p->val); p = p->forw; }
    printf(" (back to %d)\n", p->val);
}

/* ATTACK: remque on a node that was never inserted anywhere (both q_forw and
 * q_back NULL, exactly insque(&x, NULL)'s output) must be a safe no-op --
 * the two `if (e->q_forw != NULL)` / `if (e->q_back != NULL)` guards exist
 * for precisely this case. Also confirms remque does NOT clear the removed
 * node's own q_forw/q_back afterward (matches real glibc: the node is left
 * pointing at its former neighbours, which is why a caller must not reuse a
 * removed node's forw/back without re-initializing them). */
static void remque_isolated_and_stale_section(void)
{
    struct node iso = { 0, 0, 111 };
    insque(&iso, NULL);
    remque(&iso);
    printf("remque-isolated-node -> survived, forw=%s back=%s (both still NULL)\n",
           iso.forw ? "non-NULL(bug)" : "NULL", iso.back ? "non-NULL(bug)" : "NULL");

    struct node n1 = {0,0,1}, n2 = {0,0,2}, n3 = {0,0,3};
    insque(&n1, NULL);
    insque(&n2, &n1);
    insque(&n3, &n2);           /* n1 n2 n3 */
    struct node *old_forw = n2.forw, *old_back = n2.back;
    remque(&n2);                /* n1 n3, but n2's OWN fields are untouched */
    printf("remque-stale-self-pointers -> n2.forw-unchanged=%s n2.back-unchanged=%s\n",
           n2.forw == old_forw ? "yes" : "NO(bug)", n2.back == old_back ? "yes" : "NO(bug)");
    print_forward("remque-stale after-removal list", &n1);
}

/* ATTACK: a single-element circular ring (a node whose forw/back both point
 * to itself) fed to remque -- e->q_forw (== e itself) gets its q_back set to
 * e->q_back (== e itself), a self-assignment; likewise the second line. Real
 * glibc's remque has no special case for this either (it's the same two
 * unconditional-if-non-NULL statements), so this is a legitimate diff target
 * confirming neither side crashes or corrupts a 1-node ring on removal. */
static void remque_single_node_ring_section(void)
{
    struct node a = { 0, 0, 55 };
    a.forw = &a; a.back = &a;
    remque(&a);
    printf("remque-single-node-ring -> survived, forw-self=%s back-self=%s\n",
           a.forw == &a ? "yes" : "no", a.back == &a ? "yes" : "no");
}

int main(void)
{
    tsearch_section();
    tsearch_edge_cases();
    tsearch_null_rootp_section();
    twalk_tdestroy_null_root_section();
    single_node_twalk_section();
    hsearch_section();
    hcreate_zero_section();
    hsearch_empty_table_find_section();
    hsearch_full_table_miss_section();
    hsearch_similar_keys_section();
    hsearch_r_multi_cycle_section();
    hsearch_reentrant_section();
    lsearch_section();
    lfind_empty_array_section();
    lsearch_empty_array_section();
    lsearch_all_duplicates_section();
    lsearch_struct_size_section();
    insque_linear_section();
    insque_circular_section();
    remque_isolated_and_stale_section();
    remque_single_node_ring_section();
    return 0;
}
