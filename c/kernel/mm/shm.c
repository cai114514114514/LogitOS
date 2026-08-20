#include <stdint.h>
#include <stddef.h>
#include "shm.h"
#include "mm.h"
#include "pmm.h"
#include "mmhost.h"
#include "spinlock.h"
#include "kprintf.h"

/* See shm.h. The whole design argument is there; this file is the mechanism.
 *
 * Read shm.h's invariant section before changing anything here. The one rule
 * that must survive every edit: THE SEGMENT HOLDS EXACTLY ONE pmm REFERENCE
 * PER RESIDENT PAGE, for as long as the segment is live. That single reference
 * is what makes reclaim's existing equality test refuse the frame, with no
 * change to reclaim.c and no exception list -- and it is the only thing
 * standing between a shared page and a silent, permanent decoupling of the two
 * processes using it. */

void *memset(void *, int, size_t);

struct seg {
    uint64_t frame[SHM_PAGEMAX];  /* physical addresses; every entry live while used */
    /* THE NAME IS ALSO THE LIFETIME FLAG, and it is one field rather than two
     * because two was a bug. This started as `name[]` plus an `unlinked` flag,
     * with the free condition `refs == 0 && unlinked` -- which is correct for a
     * named segment and silently WRONG for an unnamed one: shm_create_anon()
     * has no name to unlink, so `unlinked` could never become true and the
     * segment leaked its frames for the life of the boot. Nothing could observe
     * it from ring 3 and no audit would flag it, because every refcount was
     * perfectly consistent -- the memory was simply never given back.
     *
     * Collapsing the two removes the bug class rather than the bug: "has a
     * name" and "can still be found" are the same fact, so they are the same
     * field, and an unnamed segment is born in exactly the state an unlinked
     * one reaches. Found by mm_shm_test's t_anon_segment. */
    char     name[SHM_NAMEMAX];   /* "" = no name: unnamed, or unlinked */
    uint64_t pages;
    int      refs;                /* VMAs + handles held across a syscall */
    unsigned owner;               /* uid that created it */
    unsigned mode;                /* rw bits, owner class << 6 | other class */
    int      used;
};

static struct seg g_seg[SHM_SEGMAX];
static spinlock_t shm_lock = SPINLOCK_INIT;
static int g_ready;
static uint64_t g_bugs;

/* A bug in this file is a shared page that stops being shared, which is
 * invisible from both processes. Counted and printed rather than ignored, the
 * same way pmm.c and rmap.c count theirs, so "shm is misbehaving" is a number
 * a harness can assert on instead of a string in a log. */
static void shm_bug(const char *what, int sh)
{
    g_bugs++;
    kprintf("[shm] BUG: %s (segment %d)\n", what, sh);
}

void shm_init(void)
{
    if (g_ready) return;
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    for (int i = 0; i < SHM_SEGMAX; i++) g_seg[i].used = 0;
    g_ready = 1;
    spin_unlock_irqrestore(&shm_lock, fl);
}

static int name_ok(const char *n)
{
    if (!n || !n[0]) return 0;
    int i = 0;
    for (; n[i]; i++) {
        if (i >= SHM_NAMEMAX - 1) return 0;
        /* No path separator. A segment name is a key in THIS table and nothing
         * else -- it is not a file, there is no directory to walk, and nothing
         * here resolves `..`. Allowing '/' would invite a caller to believe the
         * name means something to the filesystem, which is the sort of belief
         * that only fails once the two namespaces disagree. Refused rather than
         * silently rewritten, so the caller finds out at the call. */
        if (n[i] == '/') return 0;
    }
    return i > 0;
}

static int name_eq(const char *a, const char *b)
{
    for (int i = 0; i < SHM_NAMEMAX; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static void name_copy(char *dst, const char *src)
{
    int i = 0;
    for (; src && src[i] && i < SHM_NAMEMAX - 1; i++) dst[i] = src[i];
    for (; i < SHM_NAMEMAX; i++) dst[i] = 0;
}

/* THE PERMISSION RULE, and what it deliberately does not do.
 *
 * Two classes, owner and other -- not three. A group class would need the
 * asking process's group list, and this file is compiled for the host test
 * where c/fs (and therefore vfs_cred) is not on the include path; inventing a
 * gid parameter that only one of the two callers can fill would give the rule a
 * branch nothing exercises. So a non-owner is checked against the OTHER bits,
 * which is the LEAST permissive of the two answers a group class could have
 * given. Widening it later is additive and cannot retroactively grant access.
 *
 * Root bypasses, as it does everywhere in c/fs. */
static int may(const struct seg *s, unsigned uid, unsigned want)
{
    if (uid == 0) return 1;
    unsigned bits = (uid == s->owner) ? ((s->mode >> 6) & 7u) : (s->mode & 7u);
    return (bits & want) == want;
}

/* Give a segment its pages. EAGER -- see shm.h on why the fault path may not
 * allocate. All or nothing: a segment that got half its frames would be a
 * mapping whose upper half faults forever, which is worse than a refusal the
 * caller can act on.
 *
 * Called with shm_lock held. That puts pmm_lock under shm_lock, which is the
 * declared order (BKL -> shm_lock -> pmm_lock); it is safe because nothing
 * pmm_alloc can reach -- including the reclaim pass its watermark may trigger
 * -- calls back into this file. Reclaim walks the rmap and the page tables and
 * has no reason to know segments exist, which is exactly the property shm.h's
 * refcount choice was made to preserve. */
static int seg_fill(struct seg *s, uint64_t pages)
{
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t f = pmm_alloc();
        if (!f) {
            for (uint64_t k = 0; k < i; k++) pmm_free(s->frame[k]);
            return -1;
        }
        /* Zeroed for do_anon()'s reason: handing over a frame with the previous
         * owner's bytes in it is a disclosure bug, and a segment is handed to a
         * process that by construction did not own that memory before. */
        memset(mm_p2v(f), 0, 4096);
        s->frame[i] = f;
    }
    s->pages = pages;
    return 0;
}

/* Drop the segment's reference on every page and free the slot. Called with
 * shm_lock held, at the moment the last reference goes.
 *
 * A PAGE MAY STILL BE MAPPED HERE, and that is safe rather than merely
 * tolerated. It was worth checking, because "the segment released its pages
 * while a live PTE pointed at them" is the exact shape of a use-after-free --
 * so this function asserted the opposite for a while and the assertion FIRED,
 * on the ordinary process-exit path. The reason is in vmm.c: vmm_free_user()
 * calls vma_space_clear() FIRST and walks the page tables second, so the area's
 * segment reference is put while that space's PTEs are still installed. That
 * ordering is deliberate elsewhere too -- mmsys.c's SYS_MUNMAP drops the
 * reservation before the mapping, and argues that reversing it would let a
 * fault re-fill a page that is about to be thrown away.
 *
 * What makes it safe is pmm_free's contract: it is a DECREMENT, not a free, and
 * the frame returns to the allocator only at zero. Every live PTE holds its own
 * reference (fault.c's do_shm takes one, vmm.c's clone takes one per child
 * PTE), so releasing the segment's reference can never be the one that frees a
 * frame somebody still maps -- it takes the count to the number of PTEs, and
 * the last of those is what actually frees it. The page simply stops being
 * unevictable at that moment, which is correct: it is no longer shared with
 * anything that could still join.
 *
 * The property that WOULD be a bug -- a frame handed back while a PTE points at
 * it -- is not checked here because it is checked better elsewhere and from
 * independent data: rmap_audit() re-derives rmap_count <= pmm_refcount over
 * every frame on the machine, and pmm_audit() the allocator's own side. */
static void seg_free(struct seg *s)
{
    for (uint64_t i = 0; i < s->pages; i++) {
        if (!s->frame[i]) continue;
        pmm_free(s->frame[i]);
        s->frame[i] = 0;
    }
    s->pages = 0;
    s->used = 0;
    s->name[0] = 0;
}

static struct seg *find_named(const char *name, int *idx)
{
    for (int i = 0; i < SHM_SEGMAX; i++)
        if (g_seg[i].used && g_seg[i].name[0] &&
            name_eq(g_seg[i].name, name)) {
            if (idx) *idx = i;
            return &g_seg[i];
        }
    return NULL;
}

static int seg_new(const char *name, uint64_t pages, unsigned mode, unsigned uid)
{
    int slot = -1;
    for (int i = 0; i < SHM_SEGMAX; i++) if (!g_seg[i].used) { slot = i; break; }
    if (slot < 0) return SHM_E_NOMEM;

    struct seg *s = &g_seg[slot];
    s->used = 1;
    s->refs = 1;
    s->owner = uid;
    s->mode = mode;
    s->pages = 0;
    name_copy(s->name, name);
    if (seg_fill(s, pages) < 0) { s->used = 0; s->name[0] = 0; return SHM_E_NOMEM; }
    return slot;
}

int shm_open(const char *name, unsigned pages, unsigned mode, unsigned flags,
             unsigned uid)
{
    if (!g_ready) shm_init();
    if (!name_ok(name)) return SHM_E_INVAL;

    unsigned want = 4u | ((flags & SHM_WRITE) ? 2u : 0u);
    int ret;
    uint64_t fl = spin_lock_irqsave(&shm_lock);

    int idx = 0;
    struct seg *s = find_named(name, &idx);
    if (s) {
        if (flags & SHM_EXCL)      { ret = SHM_E_EXIST; goto out; }
        if (!may(s, uid, want))    { ret = SHM_E_ACCES; goto out; }
        s->refs++;
        ret = idx;
        goto out;
    }
    if (!(flags & SHM_CREAT)) { ret = SHM_E_NOENT; goto out; }
    /* Size is checked only on the CREATE path, and only here: an open of an
     * existing segment gets the size it already has, never the size the second
     * caller guessed. A "resize on open" would silently move every other
     * mapping's pages. */
    if (pages == 0 || pages > SHM_PAGEMAX) { ret = SHM_E_INVAL; goto out; }
    ret = seg_new(name, pages, mode & 0777u, uid);
out:
    spin_unlock_irqrestore(&shm_lock, fl);
    return ret;
}

int shm_create_anon(unsigned pages, unsigned uid)
{
    if (!g_ready) shm_init();
    if (pages == 0 || pages > SHM_PAGEMAX) return SHM_E_INVAL;
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    /* mode 0600: nothing can look it up by name anyway, so the bits only ever
     * matter if a handle reaches another uid -- which it can, through fork. The
     * owner keeps read+write; nobody else gets either. */
    int ret = seg_new("", pages, 0600u, uid);
    spin_unlock_irqrestore(&shm_lock, fl);
    return ret;
}

int shm_unlink(const char *name, unsigned uid)
{
    if (!g_ready) shm_init();
    if (!name_ok(name)) return SHM_E_INVAL;
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    int idx = 0, ret;
    struct seg *s = find_named(name, &idx);
    if (!s)                      ret = SHM_E_NOENT;
    /* Removing the NAME needs write permission on the object, which is the
     * closest this table has to "write permission on the directory it is in".
     * A reader that could unlink would be able to take the segment away from
     * everyone who has not opened it yet. */
    else if (!may(s, uid, 2u))   ret = SHM_E_ACCES;
    else {
        /* The name goes NOW; the object goes when the last reference does. That
         * is POSIX's rule and it is what makes create-map-unlink safe: every
         * mapping already made keeps working, and no later opener can find the
         * segment and join a conversation it was not part of. */
        s->name[0] = 0;
        if (s->refs == 0) seg_free(s);
        ret = 0;
    }
    spin_unlock_irqrestore(&shm_lock, fl);
    return ret;
}

static int live(int sh) { return sh >= 0 && sh < SHM_SEGMAX && g_seg[sh].used; }

int shm_ref(int sh)
{
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    int ret = -1;
    if (live(sh)) { g_seg[sh].refs++; ret = 0; }
    spin_unlock_irqrestore(&shm_lock, fl);
    return ret;
}

void shm_put(int sh)
{
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    if (live(sh)) {
        if (g_seg[sh].refs <= 0) shm_bug("reference put on a segment with none", sh);
        /* No name left to find it by -- unnamed from birth, or unlinked --
         * and no reference left holding it. Both halves are required: a NAMED
         * segment with no references is not dead, it is idle, and POSIX says a
         * later open must find it. */
        else if (--g_seg[sh].refs == 0 && !g_seg[sh].name[0])
            seg_free(&g_seg[sh]);
    }
    spin_unlock_irqrestore(&shm_lock, fl);
}

uint64_t shm_frame(int sh, uint64_t index)
{
    uint64_t f = 0;
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    if (live(sh) && index < g_seg[sh].pages) f = g_seg[sh].frame[index];
    spin_unlock_irqrestore(&shm_lock, fl);
    return f;
}

uint64_t shm_pages(int sh)
{
    uint64_t n = 0;
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    if (live(sh)) n = g_seg[sh].pages;
    spin_unlock_irqrestore(&shm_lock, fl);
    return n;
}

int shm_segs_live(void)
{
    int n = 0;
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    for (int i = 0; i < SHM_SEGMAX; i++) if (g_seg[i].used) n++;
    spin_unlock_irqrestore(&shm_lock, fl);
    return n;
}

uint64_t shm_frames_held(void)
{
    uint64_t n = 0;
    uint64_t fl = spin_lock_irqsave(&shm_lock);
    for (int i = 0; i < SHM_SEGMAX; i++) if (g_seg[i].used) n += g_seg[i].pages;
    spin_unlock_irqrestore(&shm_lock, fl);
    return n;
}

uint64_t shm_bugs(void) { return g_bugs; }

void shm_report(const char *tag)
{
    /* The KiB figure is the one worth printing: it is memory reclaim cannot
     * take back under pressure (shm.h), so it belongs beside the reclaim
     * counters rather than buried in a segment count. */
    uint64_t held = shm_frames_held();
    kprintf("[shm] %s: %d segments, %d pages (%d KiB unreclaimable), %d bugs\n",
            tag ? tag : "-", shm_segs_live(), (int)held,
            (int)(held * 4), (int)g_bugs);
}
