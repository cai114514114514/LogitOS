/* <search.h>: tsearch/tfind/tdelete/twalk/tdestroy (a caller-comparator
 * binary search tree), hcreate/hsearch/hdestroy + the reentrant _r trio (a
 * fixed-capacity open-addressed hash table), lsearch/lfind (linear array
 * search) and insque/remque (doubly-linked queue splice). Pure computation,
 * no syscall anywhere in it, so every case here is checked byte-for-byte
 * against real glibc (tests/unit/libc_search_test.c) -- several of the
 * choices below were reverse-engineered by PROBING glibc on the host rather
 * than guessed, because the man page and the installed header comment
 * disagree with what glibc actually does (see the hsearch ENTER note).
 *
 * ============================================================================
 * TSEARCH/TWALK SHAPE: DELIBERATELY NOT GLIBC'S, AND WHY THAT IS SAID OUT LOUD
 * ============================================================================
 * glibc's tsearch is NOT a plain binary search tree. It was rewritten (the
 * modern glibc source keeps a `struct node_t { ... int bal; }` in its
 * comments) into a self-balancing tree: inserting 100000 keys IN SORTED
 * ORDER on the host and walking the result gives a maximum twalk `level` of
 * 24 -- a plain BST would give 100000 (a single long chain) and a caller
 * doing that on a real machine would blow the stack in twalk's recursion
 * long before it got to 24. Verified with a throwaway host probe, not
 * assumed.
 *
 * This file is a PLAIN, unbalanced binary search tree. That is a real,
 * documented choice, not an oversight:
 *   - Reproducing glibc's exact rebalancing algorithm (which rotation is
 *     chosen, on which side, at which of insert/delete) would only be
 *     provably correct by ALSO reproducing glibc's exact tree SHAPE node for
 *     node, because twalk's preorder/postorder/endorder/level sequence is a
 *     function of shape, not just of the key set. That is a much bigger
 *     undertaking than the container library it lives in is worth, and a
 *     shape bug would be exactly the kind of thing that "looks like it
 *     passes" until a differently-ordered insert sequence finds it.
 *   - The alternative that is both honest and fully checkable: twalk's
 *     PREORDER/ENDORDER events (and a leaf's level) are shape-dependent and
 *     are NOT compared here. Its LEAF and POSTORDER events are NOT
 *     shape-dependent -- see the proof below -- so the diff test compares
 *     exactly those, plus the shape-INDEPENDENT parts of every other
 *     function (found vs not-found, which key's data a lookup returns,
 *     whether a duplicate insert keeps the original entry).
 *
 * The proof that {leaf, postorder} is shape-independent: walk a node with
 * both children: (preorder, recurse left, postorder, recurse right,
 * endorder) -- a node with only a right child still fires preorder then
 * postorder immediately (recurse left is skipped, but the postorder event
 * still happens between "left subtree done" and "right subtree begins").
 * So POSTORDER always fires for an internal node at exactly the point its
 * own key belongs relative to its two subtrees, and LEAF fires exactly once
 * for a node with neither child, at exactly that same point (it doesn't
 * NEED a separate before/after event because there is no subtree on either
 * side to separate). Filtering the whole walk down to {leaf, postorder}
 * therefore emits every key exactly once, in the tree's INORDER sequence --
 * which is sorted order for ANY valid binary search tree, balanced or not.
 * Two BSTs holding the same key set, of any two different shapes, produce
 * the identical {leaf, postorder} projection. tests/unit/libc_search_test.c
 * checks that projection, not the raw event stream.
 *
 * tdelete's return-value contract (checked here, and it IS shape-independent
 * unlike the tree's internal layout) was confirmed the same way: probing
 * real glibc shows NULL for "key not found" or "tree empty", and a non-NULL
 * pointer whenever the key WAS found and removed -- for a deleted leaf, a
 * deleted one-child node, AND a deleted root. The one thing the man page
 * warns never to do is dereference what's returned when the root was
 * deleted (it may point at memory just freed); this file follows the same
 * discipline that produces that contract -- see tdelete() below.
 *
 * Known cost of staying unbalanced, stated rather than hidden: inserting
 * already-sorted keys degenerates this tree into a linked list, O(n) per
 * operation and O(n) deep recursion in twalk/tdestroy. No caller in this
 * tree currently feeds tsearch a pre-sorted key stream; if one ever does,
 * that is the point to reconsider this trade-off, not silently accept O(n^2).
 *
 * ============================================================================
 * HCREATE CAPACITY: REVERSE-ENGINEERED FROM REAL BEHAVIOUR, NOT THE HEADER
 * ============================================================================
 * The installed glibc <search.h> comment claims ENTER on an existing key
 * "replace[s] existing data ... with ITEM.data". Probing real glibc on the
 * host (hcreate a small table, ENTER a key, ENTER the SAME key again with
 * different data) shows the ORIGINAL data survives -- the header comment is
 * wrong, or describes an intent glibc's actual implementation abandoned.
 * This file matches the MEASURED behaviour: ENTER on an existing key leaves
 * its data untouched and returns the existing entry, for both hsearch(_r)
 * and, by construction, everything built on the same probe loop below.
 *
 * hcreate's CAPACITY was also measured rather than guessed: filling a table
 * with hcreate(nel) for nel = 1..20 and counting successful ENTERs before
 * the first ENOMEM gives exactly "the smallest prime >= (nel | 1)" -- e.g.
 * nel=1..3 all cap at 3, nel=4..5 cap at 5, nel=8..11 cap at 11. next_prime()
 * below reproduces that formula (and was checked against the same probe
 * values). The exact HASH FUNCTION and probe STEP are not part of this: no
 * public API exposes table iteration order, so any hash that is deterministic
 * per key and any probe sequence that is guaranteed to visit every slot
 * (satisfied here because the table size is always prime -- see hsearch_r)
 * gives identical FIND/ENTER outcomes to glibc's, which is all the diff test
 * can observe and all it checks.
 *
 * A second measured (not documented) fact: calling hcreate() a second time
 * while a table is already active fails (returns 0) WITHOUT touching errno
 * -- confirmed by resetting errno=0 immediately before the call. This file
 * matches that: the "table already active" path in hcreate_r never sets
 * errno.
 *
 * NEVER-FREE CONTRACT: hsearch(_r) never frees a key or a data pointer, and
 * neither does hdestroy(_r) -- both are exactly the caller's memory, on
 * glibc and here alike. A ported program that keeps its own key strings
 * alive across hdestroy() and reuses them relies on this; freeing them here
 * "to be tidy" would be a real bug, not a cleanup. */

#include <search.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ==========================================================================
 * tsearch / tfind / tdelete / twalk / tdestroy
 * ========================================================================== */

/* `key` MUST be the first field: tsearch/tfind/tdelete all return a pointer
 * to one of these nodes, and the documented public contract (stated in the
 * glibc header comment, and relied on by real ported code doing
 * `*(void **)tsearch(...)` to recover the key) is that the first field of
 * whatever gets returned IS the key. Reordering these fields keeps this file
 * self-consistent but breaks every caller written against that contract. */
struct tnode {
    const void *key;
    struct tnode *left, *right;
};

void *tsearch(const void *key, void **rootp, __compar_fn_t compar)
{
    if (!rootp) return NULL;
    struct tnode **link = (struct tnode **)rootp;

    while (*link) {
        int c = compar(key, (*link)->key);
        if (c == 0) return *link;           /* already present: existing node, unmodified */
        link = c < 0 ? &(*link)->left : &(*link)->right;
    }
    struct tnode *n = malloc(sizeof *n);
    if (!n) return NULL;                    /* glibc: NULL on allocation failure */
    n->key = key;
    n->left = n->right = NULL;
    *link = n;
    return n;
}

void *tfind(const void *key, void *const *rootp, __compar_fn_t compar)
{
    if (!rootp) return NULL;
    const struct tnode *n = *rootp;         /* void* -> struct tnode* is implicit in C */
    while (n) {
        int c = compar(key, n->key);
        if (c == 0) return (void *)n;
        n = c < 0 ? n->left : n->right;
    }
    return NULL;
}

/* Detach and return the minimum node of the subtree rooted at *link,
 * rewriting *link to skip over it. The caller owns relinking the returned
 * node's ->left; its ->right is intentionally left as-is by this function --
 * see the comment in tdelete() for why that is exactly correct in both the
 * "successor is the direct right child" and "successor is deeper" cases. */
static struct tnode *unlink_min(struct tnode **link)
{
    while ((*link)->left)
        link = &(*link)->left;
    struct tnode *m = *link;
    *link = m->right;
    return m;
}

void *tdelete(const void *restrict key, void **restrict rootp, __compar_fn_t compar)
{
    if (!rootp) return NULL;

    struct tnode **link = (struct tnode **)rootp;
    struct tnode *parent = NULL;
    int c = 0;
    while (*link && (c = compar(key, (*link)->key)) != 0) {
        parent = *link;
        link = c < 0 ? &(*link)->left : &(*link)->right;
    }
    struct tnode *victim = *link;
    if (!victim) return NULL;               /* not present (including: empty tree) */

    if (victim->left && victim->right) {
        /* Two children: splice the INORDER SUCCESSOR (leftmost node of the
         * right subtree) into victim's slot, rather than copying the
         * successor's KEY into victim and freeing the successor. Copying
         * would silently invalidate any pointer an earlier tsearch() call
         * returned for victim's key -- a caller comparing that pointer by
         * address later would find it now holds a different key's node.
         * Splicing keeps every previously-returned node pointer valid for
         * exactly as long as its key remains in the tree.
         *
         * NOTE, confirmed by a throwaway host probe and stated precisely
         * because the claim above is easy to misread as "and this is what
         * glibc does too": it is NOT. Probing real glibc with the smallest
         * possible two-children case (a 3-node tree, root with two LEAF
         * children -- deliberately too small for its rebalancing to be a
         * factor either way) shows the immediate successor's OWN pointer,
         * captured from an earlier tsearch(), does NOT survive deleting its
         * parent: a later tfind() for that same key returns a DIFFERENT
         * address. glibc's real tdelete evidently reuses/copies rather than
         * splicing in at least that case. That makes node-pointer identity
         * across a two-children tdelete implementation-defined -- POSIX
         * documents tdelete's RETURN value contract (checked above) but
         * says nothing about surviving nodes' addresses -- so it is
         * deliberately not part of the byte-diffed gate (comparing raw
         * addresses across two different processes is meaningless anyway;
         * what would be diffable is the same same/different BOOLEAN my
         * probe used, and that boolean does not match glibc's). This
         * function keeps the splice-not-copy behavior anyway because the
         * alternative (silently repointing a live caller-held pointer at a
         * different key) is a worse failure mode for a freestanding kernel
         * caller than diverging from an unspecified, unobservable-by-diff
         * corner of glibc's own implementation. */
        struct tnode *succ = unlink_min(&victim->right);
        succ->left = victim->left;
        /* If succ WAS victim->right (no left subtree of its own), unlink_min
         * already rewrote victim->right to succ's old right child -- so this
         * assignment reads back exactly succ's own unchanged right pointer,
         * a harmless no-op. If succ was deeper, victim->right was never
         * touched by unlink_min (it only rewrites its OWN link chain), so
         * this correctly hands succ the whole (internally-repaired) right
         * subtree. Either way this line is correct without a special case. */
        succ->right = victim->right;
        *link = succ;
    } else {
        *link = victim->left ? victim->left : victim->right;
    }

    /* Compute the return value BEFORE freeing: parent-of-deleted-node, or,
     * when victim was the root (parent is NULL), some non-NULL value that
     * is NOT to be dereferenced afterward -- POSIX documents this and real
     * glibc genuinely returns a pointer into memory it just freed for this
     * exact case, so a caller that only tests it against NULL (the
     * documented and only correct use) is unaffected either way. */
    void *ret = parent ? (void *)parent : (void *)victim;
    free(victim);
    return ret;
}

static void trecurse(const struct tnode *n, __action_fn_t action, int level)
{
    if (!n->left && !n->right) {
        action(n, leaf, level);
    } else {
        action(n, preorder, level);
        if (n->left) trecurse(n->left, action, level + 1);
        action(n, postorder, level);
        if (n->right) trecurse(n->right, action, level + 1);
        action(n, endorder, level);
    }
}

void twalk(const void *root, __action_fn_t action)
{
    if (root) trecurse(root, action, 0);
}

static void tdestroy_recurse(struct tnode *n, __free_fn_t freefct)
{
    if (!n) return;
    tdestroy_recurse(n->left, freefct);
    tdestroy_recurse(n->right, freefct);
    if (freefct) freefct((void *)n->key);   /* the KEY, not the node -- see man page */
    free(n);
}

void tdestroy(void *root, __free_fn_t freefct)
{
    tdestroy_recurse(root, freefct);
}

/* ==========================================================================
 * hcreate / hsearch / hdestroy + _r
 * ========================================================================== */

struct __hsearch_entry {
    unsigned long hash;   /* 0 marks an empty slot; a real hash of 0 is remapped to 1 */
    ENTRY entry;
};

static int is_prime(unsigned long n)
{
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    /* `d <= n / d` instead of the more obvious `d * d <= n`: the direct
     * multiplication overflows unsigned long once d approaches 2^32 (its
     * square wraps past 2^64), which is reachable for a caller-supplied nel
     * near ULONG_MAX -- the wrapped product is small, so `d*d <= n` reads
     * TRUE past the real sqrt(n) boundary and the loop stops trusting its
     * own termination condition. Integer division never overflows, and
     * `d <= n/d` is exactly equivalent to `d*d <= n` for positive d, n (if
     * d*d <= n then n/d >= d by construction; if d*d > n then n/d < d) --
     * so this changes no result for any input, it only removes the one
     * input range where the multiplication form was quietly wrong. */
    for (unsigned long d = 3; d <= n / d; d += 2)
        if (n % d == 0) return 0;
    return 1;
}

/* Smallest prime >= (n | 1). See the file header for how this formula was
 * measured against real glibc rather than assumed. Forcing the low bit odd
 * before searching means the search never lands on 2, and the loop below
 * only ever tests odd candidates -- both match the measured n=1..3 -> 3
 * boundary (a naive "smallest prime >= n" would give 2 for n<=2). */
static unsigned long next_prime(unsigned long n)
{
    unsigned long c = n | 1;
    while (!is_prime(c))
        c += 2;
    return c;
}

static unsigned long hash_str(const char *s)
{
    /* djb2. Not glibc's hash -- no public API exposes table iteration order
     * or slot placement, so any deterministic hash gives identical FIND/
     * ENTER outcomes; see the file header. */
    unsigned long h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h;
}

int hcreate_r(size_t nel, struct hsearch_data *htab)
{
    if (!htab) { errno = EINVAL; return 0; }
    if (htab->table != NULL) return 0;      /* an existing table: refuse. Measured: errno untouched. */

    unsigned long size = next_prime((unsigned long)nel);
    struct __hsearch_entry *t = calloc(size, sizeof *t);
    if (!t) { errno = ENOMEM; return 0; }

    htab->table = t;
    htab->size = (unsigned int)size;
    htab->filled = 0;
    return 1;
}

void hdestroy_r(struct hsearch_data *htab)
{
    if (!htab || !htab->table) return;
    free(htab->table);                      /* keys/data are the caller's -- see file header */
    /* Deliberately do NOT zero ->size / ->filled here. Confirmed by a
     * throwaway host probe against real glibc (fill a table, hdestroy_r it,
     * read ->size/->filled back before any hcreate_r): glibc's own
     * __hdestroy_r clears only ->table, leaving ->size and ->filled at
     * their last live values -- a caller that pokes at ->filled for a
     * fill-ratio metric (the exact scenario the struct's field-naming
     * comment up in search.h calls out) would read that STALE count on
     * glibc. An earlier version of this function zeroed all three fields,
     * which is tidier but is not what a program written against glibc
     * observes, and this library's whole premise is matching what ported
     * code actually sees. hcreate_r always overwrites both fields
     * unconditionally on success (see above), so leaving them stale here
     * costs nothing on the next create -- the only thing it changes is what
     * a caller reads in the window between hdestroy_r and the next
     * hcreate_r, which is exactly the window this now matches glibc on. */
    htab->table = NULL;
}

int hsearch_r(ENTRY item, ACTION action, ENTRY **retval, struct hsearch_data *htab)
{
    if (!htab || !htab->table) {
        /* Real glibc has no such guard here and dereferences htab->table
         * unconditionally -- calling hsearch_r without a prior hcreate_r is
         * undefined behaviour there (in practice, a crash), which is not a
         * thing a host diff test can observe from the reference process
         * anyway. Refusing cleanly instead is strictly safer for a
         * freestanding ring-3 caller and is a deliberate, noted departure,
         * not an attempt to make this path match glibc byte for byte. */
        errno = EINVAL;
        if (retval) *retval = NULL;
        return 0;
    }

    unsigned long h = hash_str(item.key);
    if (h == 0) h = 1;
    unsigned long size = htab->size;
    unsigned long idx = h % size;
    /* `size` is always prime (next_prime never returns otherwise) and > 2,
     * so step is in [1, size-1] and gcd(step, size) == 1 for every possible
     * step -- the probe sequence below is guaranteed to visit all `size`
     * slots before repeating any of them. */
    unsigned long step = 1 + (h % (size - 1));

    for (unsigned long i = 0; i < size; i++) {
        struct __hsearch_entry *e = &htab->table[idx];
        if (e->hash == 0) {
            if (action == FIND) {
                errno = ESRCH;
                if (retval) *retval = NULL;
                return 0;
            }
            e->hash = h;
            e->entry.key = item.key;
            e->entry.data = item.data;
            htab->filled++;
            if (retval) *retval = &e->entry;
            return 1;
        }
        if (e->hash == h && strcmp(e->entry.key, item.key) == 0) {
            /* Key already present: measured glibc behaviour keeps the
             * ORIGINAL data for ENTER too -- see the file header. */
            if (retval) *retval = &e->entry;
            return 1;
        }
        idx = (idx + step) % size;
    }

    /* All `size` slots probed with no empty slot and no match: the table is
     * full of OTHER keys. A FIND that gets here is simply absent (ESRCH); an
     * ENTER that gets here has nowhere to go (ENOMEM), matching the
     * capacity boundary measured in the file header. */
    if (action == FIND) {
        errno = ESRCH;
    } else {
        errno = ENOMEM;
    }
    if (retval) *retval = NULL;
    return 0;
}

static struct hsearch_data g_htab;   /* the single table hcreate/hsearch/hdestroy share */

int hcreate(size_t nel)
{
    return hcreate_r(nel, &g_htab);
}

void hdestroy(void)
{
    hdestroy_r(&g_htab);
}

ENTRY *hsearch(ENTRY item, ACTION action)
{
    ENTRY *r = NULL;
    hsearch_r(item, action, &r, &g_htab);
    return r;
}

/* ==========================================================================
 * lsearch / lfind
 * ========================================================================== */

void *lfind(const void *key, const void *base, size_t *nmemb, size_t size, __compar_fn_t compar)
{
    const char *p = base;
    for (size_t i = 0; i < *nmemb; i++, p += size)
        if (compar(key, p) == 0) return (void *)p;
    return NULL;
}

void *lsearch(const void *key, void *base, size_t *nmemb, size_t size, __compar_fn_t compar)
{
    void *found = lfind(key, base, nmemb, size, compar);
    if (found) return found;

    char *slot = (char *)base + (*nmemb) * size;
    memcpy(slot, key, size);                /* lsearch copies size BYTES of *key in, not the pointer */
    (*nmemb)++;
    return slot;
}

/* ==========================================================================
 * insque / remque
 * ========================================================================== */

void insque(void *elem, void *prev)
{
    struct qelem *e = elem;
    struct qelem *p = prev;

    if (p == NULL) {
        /* The linear-list case: elem becomes an isolated head with no
         * predecessor. (A caller building a linear list inserts each new
         * element with prev = the current tail, or NULL for the very first
         * element -- this branch is what makes that first insert work.) */
        e->q_forw = NULL;
        e->q_back = NULL;
        return;
    }
    e->q_forw = p->q_forw;
    p->q_forw = e;
    e->q_back = p;
    if (e->q_forw != NULL)
        e->q_forw->q_back = e;
}

void remque(void *elem)
{
    struct qelem *e = elem;
    if (e->q_forw != NULL)
        e->q_forw->q_back = e->q_back;
    if (e->q_back != NULL)
        e->q_back->q_forw = e->q_forw;
}
