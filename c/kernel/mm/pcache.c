#include <stdint.h>
#include <stddef.h>
#include "pcache.h"
#include "pmm.h"
/* For reclaim_low() only, and only so readahead can refuse to be the thing
 * that pushes the machine into reclaim. Nothing here calls a reclaim path --
 * that direction (reclaim.c -> pcache.h) is the one that already existed. */
#include "reclaim.h"
#include "mm.h"
#include "mmhost.h"
#include "spinlock.h"
#include "kprintf.h"

/* See pcache.h for the design and, in particular, for the refcount decision.
 * This file is the mechanics. */

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

/* THE KERNEL HAS NO str* FUNCTIONS, and that is a property worth keeping rather
 * than a gap to fill. c/lib/string.c provides memset/memcpy/memmove/memcmp and
 * nothing else; every kernel file that needs to walk a C string writes the four
 * lines itself -- c/fs/vfs_path.c:5's `p_len` is the same helper under a
 * different name. Forward-declaring strlen/strncmp here, which is what this
 * file did until the link failed, does not import them: it promises the linker
 * something no object in the kernel defines. Adding them to the shared library
 * instead would put two new symbols in reach of the whole kernel to serve one
 * caller. So: local, static, bounded. */
static size_t pc_slen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

/* Bounded equality, not an ordering -- every caller only ever tests == 0, and a
 * three-way compare would invite someone to sort paths with it and inherit a
 * locale question the kernel has no answer to. Returns 0 when equal, matching
 * the strncmp convention the call sites were written against so the comparison
 * reads the same way. */
static int pc_sneq(const char *a, const char *b, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return 1;
        if (!ca) return 0;          /* both ended here */
    }
    return 0;                        /* equal for the whole bounded length */
}

/* LOCKING. One lock over both tables. It is taken UNDER the big kernel lock,
 * like every other structure in c/kernel/mm, and it never calls out while held
 * -- in particular the DEVICE READ on a miss happens with the lock DROPPED,
 * because a 4 KiB read off a disk under a spinlock with interrupts disabled is
 * not a lock, it is a stall. The miss path therefore re-checks the table after
 * the read; under the BKL nothing can have raced, and the re-check costs one
 * hash probe and makes that assumption unnecessary rather than load-bearing. */
static spinlock_t pc_lock = SPINLOCK_INIT;

struct pfile {
    uint64_t dev, ino, size;
    int      refs;          /* VMA references + transient lookups */
    int      used;
    /* The highest page index ever installed for this file, +1, or 0 for none.
     * purge() walks the INDEX SPACE rather than the pool (see purge_locked),
     * and this is the bound that makes that walk exact: pf.size can shrink
     * under a rewrite, so sizing the walk from it would leave the pages past
     * the new end resident and stale. Never decreased while any entry of the
     * file lives; reset to 0 by a purge that emptied the file. */
    uint64_t hiwater;
    /* READAHEAD'S ENTIRE STATE, and it is here rather than in a table of its
     * own because a second table keyed on (dev, ino) would be a second thing
     * to keep in step with this one -- and the one that fell behind would be
     * the one nothing audits. Eight bytes on a struct that already exists.
     *
     * ra_next is the index the NEXT request must carry to count as sequential,
     * i.e. (last index asked) + 1, with 0 meaning "nothing asked yet". That
     * encoding is why a first touch of page 0 never prefetches: 0 is not a
     * page index anyone can be resuming from. (An index of 0xFFFFFFFF wraps it
     * back to 0 and merely turns readahead off for a 16 TiB file's last page,
     * which is the harmless direction to fail in.)
     *
     * ra_win is K: how many pages the LAST batch fetched beyond the faulting
     * one. 0 means the window is cold -- set by any non-sequential request and
     * by a short read from the backend. */
    uint32_t ra_next;
    uint32_t ra_win;
    char     path[PCACHE_PATHMAX];
};

struct pentry {
    uint64_t phys;
    uint32_t index;         /* page index within the file */
    int16_t  fh;            /* -1 = free */
    int32_t  hnext;         /* next in this hash bucket, or -1 */
};

static struct pfile  pf[PCACHE_MAXFILE];
/* THE POOL IS ALLOCATED, NOT DECLARED -- see the sizing note in pcache.h.
 * Both of these used to be static arrays (98,304 + 4,096 bytes of kernel .bss
 * on every machine, spent whether or not a file was ever opened); they come
 * from one pmm_alloc_contig() at init now, sized from RAM. */
static struct pentry *pg;
static int32_t       *bucket;
static uint32_t       pc_nbucket;       /* power of two */
static uint32_t       pc_bmask;         /* pc_nbucket - 1 */
static int32_t       pc_free;           /* head of the free entry list (via hnext) */
static uint32_t      pc_npage;          /* entries actually in use as a pool */
static uint32_t      pc_hand;           /* the cache's own eviction hand */

/* THE THIRD STRUCTURE (see pcache.h): frame -> entry index + 1, or 0.
 *
 * Taken from the PMM at init, never grown, like the reverse map's pool and for
 * the same reason -- it is read by reclaim on the path that runs when memory is
 * short, so it may not be allocated there. 4 bytes a frame is 512 KiB on a
 * 512 MiB machine, 0.1% of RAM.
 *
 * It is what makes pcache_holds() one aligned load rather than a hash probe.
 * That matters: the clock visits every frame on the machine, and a structure
 * consulted per frame per sweep has to cost what rmap_mapped() costs. */
static uint32_t *pc_of_frame;
static uint64_t  pc_frames;
static int       pc_ready;

static const struct pcache_ops *pc_ops;

/* Defined below with the page table; needed earlier by pcache_file_open's
 * idle-slot eviction. */
static void purge(int fh, uint64_t *counter);

static uint64_t c_hit, c_miss, c_drop, c_evict, c_inval, c_bypass, c_peak, c_resident;
static uint64_t c_orphan, c_uncached;
static uint64_t c_ra_run, c_ra_pages, c_ra_reads, c_ra_short;
static uint64_t c_bug;

uint64_t pcache_hits(void)        { return c_hit; }
uint64_t pcache_misses(void)      { return c_miss; }
uint64_t pcache_resident(void)    { return c_resident; }
uint64_t pcache_peak(void)        { return c_peak; }
uint64_t pcache_dropped(void)     { return c_drop; }
uint64_t pcache_evicted(void)     { return c_evict; }
uint64_t pcache_invalidated(void) { return c_inval; }
uint64_t pcache_bypassed(void)    { return c_bypass; }
uint64_t pcache_orphaned(void)    { return c_orphan; }
uint64_t pcache_uncached(void)    { return c_uncached; }
uint64_t pcache_slots(void)       { return pc_npage; }
uint64_t pcache_ra_runs(void)     { return c_ra_run; }
uint64_t pcache_ra_pages(void)    { return c_ra_pages; }
uint64_t pcache_ra_reads(void)    { return c_ra_reads; }
uint64_t pcache_ra_short(void)    { return c_ra_short; }
int      pcache_ready(void)       { return pc_ready; }

uint64_t pcache_files(void)
{
    uint64_t n = 0;
    for (int i = 0; i < PCACHE_MAXFILE; i++) if (pf[i].used) n++;
    return n;
}

uint64_t pcache_shared(void)
{
    uint64_t n = 0;
    if (!pc_ready) return 0;         /* pg[] is a pointer now; nothing to walk */
    for (uint32_t i = 0; i < pc_npage; i++)
        if (pg[i].fh >= 0 && pmm_refcount(pg[i].phys) > 2) n++;
    return n;
}

void pcache_set_ops(const struct pcache_ops *ops) { pc_ops = ops; }

/* ------------------------------------------------------------------ init -- */

void pcache_init(uint64_t total_frames)
{
    if (pc_ready || total_frames == 0) return;

    /* The pool is a CEILING, not a target, and its size is DERIVED from RAM --
     * read the sizing note in pcache.h before changing either the fraction or
     * the fact that this is not a constant. */
#ifdef PCACHE_LEGACY_POOL
    /* NEGATIVE CONTROL: the expression this file shipped with, verbatim, so
     * the control is the OLD CODE rather than an approximation of it. */
    uint64_t n = PCACHE_LEGACY_MAXPAGE;
    if (n > total_frames / PCACHE_LEGACY_SHARE) n = total_frames / PCACHE_LEGACY_SHARE;
#else
    uint64_t n = total_frames / PCACHE_FRAME_SHARE;
#endif
    if (n < PCACHE_MINPAGE) n = PCACHE_MINPAGE;
    if (n > 0x7FFFFFFEull) n = 0x7FFFFFFEull;   /* the entry index is int32_t */
    pc_npage = (uint32_t)n;

    /* One bucket per four entries, rounded UP to a power of two so the hash
     * can mask instead of divide. Four is the load factor the fixed 1024/4096
     * pair already had; keeping it means the chain length does not change as
     * the pool grows, which is the only thing that would make a bigger pool
     * slower per lookup rather than merely larger. */
    pc_nbucket = 16;
    while (pc_nbucket < pc_npage / 4 && pc_nbucket < (1u << 30)) pc_nbucket <<= 1;
    pc_bmask = pc_nbucket - 1;

    /* ONE allocation for all three tables, not three. Not tidiness: this runs
     * at init and must either give the cache everything it needs or leave it
     * off, and three allocations have three chances to half-succeed and a
     * rollback path each. pmm_alloc_contig() is a linear first-fit with no
     * fallback (pmm.c), which is exactly why the whole request goes in once,
     * while the frame bitmap is still empty. */
    uint64_t b_frame  = total_frames * 4;                    /* pc_of_frame[] */
    uint64_t b_pages  = (uint64_t)pc_npage * sizeof(struct pentry);
    uint64_t b_bucket = (uint64_t)pc_nbucket * 4;
    b_frame  = (b_frame  + 7) & ~7ull;                       /* keep pg[] aligned */
    b_pages  = (b_pages  + 7) & ~7ull;
    uint64_t bytes = b_frame + b_pages + b_bucket;
    uint64_t frames = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
    uint64_t base = pmm_alloc_contig((size_t)frames);
    if (!base) {
        /* Correct degradation, and loud: with no frame table there is no O(1)
         * way to tell reclaim that a frame it is about to take is ours, so the
         * cache stays OFF entirely rather than running without the one hook
         * that keeps its entries from dangling. The kernel then behaves exactly
         * as it did before this line: file mappings are refused, reads go
         * straight to the backend.
         *
         * NOT retried at a smaller size. A cache that quietly came up with a
         * tenth of the slots it asked for would reintroduce the ceiling the
         * sizing note exists to remove, at a number nobody chose and nothing
         * prints -- which is the exact failure being fixed here. Off and loud
         * is the honest outcome; the numbers below say what was asked for. */
        kprintf("[pcache] init FAILED: no %d contiguous frames (%d KiB) for "
                "%d page slots + a %d-entry frame table "
                "-- the page cache is DISABLED for this boot\n",
                (int)frames, (int)(bytes / 1024), (int)pc_npage, (int)total_frames);
        pc_npage = 0;
        return;
    }
    uint8_t *mem = (uint8_t *)mm_p2v(base);
    pc_of_frame = (uint32_t *)mem;
    pg          = (struct pentry *)(mem + b_frame);
    bucket      = (int32_t *)(mem + b_frame + b_pages);
    pc_frames = total_frames;
    memset(pc_of_frame, 0, (size_t)(total_frames * 4));

    for (uint32_t i = 0; i < pc_nbucket; i++) bucket[i] = -1;
    for (uint32_t i = 0; i < pc_npage; i++) {
        pg[i].fh = -1;
        pg[i].phys = 0;
        pg[i].index = 0;
        pg[i].hnext = (i + 1 < pc_npage) ? (int32_t)(i + 1) : -1;
    }
    pc_free = 0;
    pc_ready = 1;

    kprintf("[pcache] up: %d page slots (%d KiB ceiling), %d file slots, "
            "%d buckets; tables %d KiB from the PMM (%d B/frame of RAM)%s\n",
            (int)pc_npage, (int)(pc_npage * 4), PCACHE_MAXFILE, (int)pc_nbucket,
            (int)(frames * FRAME_SIZE / 1024),
            (int)(frames * FRAME_SIZE / (total_frames ? total_frames : 1)),
#ifdef PCACHE_PER_OPEN
            "  [NEGATIVE CONTROL: keyed per-open, not per-inode]"
#elif defined(PCACHE_NO_READAHEAD)
            /* Said at boot, because a control kernel's log is otherwise
             * indistinguishable from the real one until somebody reads the
             * right counter -- and a control that cannot be told apart from
             * the thing it controls is how a measurement gets attributed to
             * the wrong build. */
            "  [NEGATIVE CONTROL: readahead compiled out]"
#elif defined(PCACHE_RA_ALWAYS)
            "  [NEGATIVE CONTROL: readahead WITHOUT the sequential test]"
#else
            ""
#endif
            );
}

/* --------------------------------------------------------------- lookup -- */

static inline uint32_t hash(int fh, uint64_t index)
{
    uint64_t h = ((uint64_t)fh * 0x9E3779B97F4A7C15ull) ^ (index * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)((h >> 32) & pc_bmask);
}

/* Caller holds pc_lock. */
static int32_t find(int fh, uint64_t index)
{
    for (int32_t e = bucket[hash(fh, index)]; e >= 0; e = pg[e].hnext)
        if (pg[e].fh == fh && pg[e].index == (uint32_t)index) return e;
    return -1;
}

/* Caller holds pc_lock. Unlinks the entry from its bucket and from
 * pc_of_frame, and returns the frame WITHOUT dropping the cache's reference --
 * every caller has a different thing to do with it. */
static uint64_t unlink_entry(int32_t e)
{
    uint32_t b = hash(pg[e].fh, pg[e].index);
    int32_t *pp = &bucket[b];
    while (*pp >= 0 && *pp != e) pp = &pg[*pp].hnext;
    if (*pp != e) { c_bug++; kprintf("[pcache] BUG: entry %d not in its bucket\n", (int)e); }
    else *pp = pg[e].hnext;

    uint64_t phys = pg[e].phys;
    uint64_t f = phys / FRAME_SIZE;
    if (pc_of_frame && f < pc_frames) pc_of_frame[f] = 0;

    pg[e].fh = -1;
    pg[e].phys = 0;
    pg[e].hnext = pc_free;
    pc_free = e;
    if (c_resident) c_resident--;
    return phys;
}

int pcache_holds(uint64_t phys)
{
    if (!pc_ready) return 0;
    uint64_t f = phys / FRAME_SIZE;
    if (f >= pc_frames) return 0;
    return pc_of_frame[f] != 0;      /* one aligned load; see pcache.h */
}

void pcache_forget_frame(uint64_t phys)
{
    if (!pc_ready) return;
    uint64_t f = phys / FRAME_SIZE;
    if (f >= pc_frames) return;

    uint64_t fl = spin_lock_irqsave(&pc_lock);
    uint32_t slot = pc_of_frame[f];
    if (slot) {
        unlink_entry((int32_t)(slot - 1));
        c_drop++;
        spin_unlock_irqrestore(&pc_lock, fl);
        pmm_free(phys);              /* the cache's own reference, and only it */
        return;
    }
    spin_unlock_irqrestore(&pc_lock, fl);
}

/* ---------------------------------------------------------------- files -- */

int pcache_file_open(const char *path)
{
    if (!pc_ready || !pc_ops || !pc_ops->stat || !path) return -1;

    uint64_t dev = 0, ino = 0, size = 0;
    if (pc_ops->stat(path, &dev, &ino, &size) < 0) return -1;

    uint64_t fl = spin_lock_irqsave(&pc_lock);
    int ret = -1;

#ifndef PCACHE_PER_OPEN
    /* THE KEY IS THE INODE. Two paths hard-linked to one file, and two
     * processes opening the same name, all arrive here and all get the same
     * handle -- which is the only reason a page can be shared at all. The
     * negative control below removes exactly this loop. */
    for (int i = 0; i < PCACHE_MAXFILE; i++)
        if (pf[i].used && pf[i].dev == dev && pf[i].ino == ino) {
            pf[i].refs++;               /* revives a CACHED-IDLE entry too --
                                         * that re-hit is what idle exists for */
            /* The size can have moved under us since the last open (logitfs
             * rewrites a whole file per write); the invalidation on write has
             * already emptied the pages, so refreshing the length here is all
             * that is left to do. The PATH is refreshed with it: the identity
             * is the inode, but pcv_read re-reads BY PATH, and this open may
             * arrive through a different hard link than the one the entry was
             * created under -- both name the same inode, so the caller's
             * spelling is the one guaranteed to still resolve. */
            pf[i].size = size;
            {
                size_t n = pc_slen(path);
                if (n < PCACHE_PATHMAX) memcpy(pf[i].path, path, n + 1);
            }
            ret = i;
            goto out;
        }
#else
    /* NEGATIVE CONTROL (-DPCACHE_PER_OPEN): never match an existing entry, so
     * every open() gets its own pages for the same file offset. Everything a
     * single process can observe about itself is unchanged. */
#endif

    for (int i = 0; i < PCACHE_MAXFILE; i++)
        if (!pf[i].used) {
            pf[i].used = 1;
            pf[i].refs = 1;
            pf[i].dev = dev;
            pf[i].ino = ino;
            pf[i].size = size;
            pf[i].hiwater = 0;      /* a fresh identity has installed nothing */
            pf[i].ra_next = 0;      /* ...and nobody has read it in any order */
            pf[i].ra_win  = 0;
            size_t n = pc_slen(path);
            if (n >= PCACHE_PATHMAX) { pf[i].used = 0; goto out; }
            memcpy(pf[i].path, path, n + 1);
            ret = i;
            goto out;
        }
    /* Table full of ENTRIES -- but an entry with refs == 0 is CACHED-IDLE,
     * holding pages purely on the bet that someone re-reads them. A new file
     * that needs the slot wins that bet's collateral: evict the first idle
     * entry, take its slot. The claim (identity + refs=1) happens under the
     * lock so nothing can revive the victim mid-swap; the purge of its pages
     * happens OUTSIDE, like every purge here, because pmm_free under a
     * spinlock with interrupts off is not a critical section, it is a stall.
     * The purge throws away only frames tagged with this fh -- all of them the
     * OLD identity's, since the caller cannot install new pages until we
     * return. First-idle, not LRU, on purpose: 32 slots, and an LRU stamp
     * would be bookkeeping the workload cannot yet justify -- revisit when
     * pcache_report says eviction is hot. */
    for (int i = 0; i < PCACHE_MAXFILE; i++)
        if (pf[i].used && pf[i].refs <= 0) {
            size_t n = pc_slen(path);
            if (n >= PCACHE_PATHMAX) goto out;
            pf[i].refs = 1;
            pf[i].dev = dev; pf[i].ino = ino; pf[i].size = size;
            memcpy(pf[i].path, path, n + 1);
            ret = i;
            spin_unlock_irqrestore(&pc_lock, fl);
            purge(ret, &c_evict);       /* the OLD identity's pages, under the
                                         * OLD hiwater -- which is why the reset
                                         * below comes after, not before */
            fl = spin_lock_irqsave(&pc_lock);
            if (pf[ret].used && pf[ret].dev == dev && pf[ret].ino == ino) {
                pf[ret].hiwater = 0;    /* still ours: the new identity has
                                         * installed nothing yet */
                /* The sequence state goes with the identity for the same
                 * reason: the previous file's trail would make this file's
                 * first request look like a resumption of a walk through a
                 * file it has nothing to do with, and prefetch pages of it. */
                pf[ret].ra_next = 0;
                pf[ret].ra_win  = 0;
            }
            spin_unlock_irqrestore(&pc_lock, fl);
            return ret;
        }
    /* Full of LIVE entries. Not fatal and not silent: the mapping is refused,
     * the caller falls back to reading the file, and the number says the table
     * is too small for the workload rather than that the workload is wrong. */
    kprintf("[pcache] no file slot for %s (%d in use) -- not cached\n",
            path, PCACHE_MAXFILE);
out:
    spin_unlock_irqrestore(&pc_lock, fl);
    return ret;
}

void pcache_file_ref(int fh)
{
    if (fh < 0 || fh >= PCACHE_MAXFILE) return;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    if (pf[fh].used) pf[fh].refs++;
    spin_unlock_irqrestore(&pc_lock, fl);
}

uint64_t pcache_file_size(int fh)
{
    if (fh < 0 || fh >= PCACHE_MAXFILE) return 0;
    return pf[fh].used ? pf[fh].size : 0;
}

/* Caller holds pc_lock. Up to `max` resident pages of `fh`, unlinked; the
 * frames come back on `out` so the reference drops can happen with the lock
 * released. `*cursor` is the page index to resume from and is advanced.
 *
 * THIS WALKS THE FILE'S INDEX SPACE, NOT THE POOL, and that is the one change
 * the pool's new size forced. The old loop was `for i in 0..pc_npage: if
 * pg[i].fh == fh`, which is O(pool) per batch of 64 -- fine at 4096 slots, and
 * 65,536 iterations per batch at the sized-from-RAM pool, on the path every
 * vfs_write takes for a cached file. Probing find(fh, 0..hiwater) instead costs
 * O(the file's pages), which is the right bound: a purge cannot remove more
 * pages than the file has, and for the small files that dominate (a two-page
 * config, a one-page script) it is two probes instead of tens of thousands.
 *
 * hiwater and not size/4096: a rewrite can SHRINK a file between the pages
 * being installed and the invalidation arriving, and pages past the new end
 * are exactly the ones a reader must not still see. */
static int purge_locked(int fh, uint64_t *out, int max, uint64_t *cursor)
{
    int n = 0;
    uint64_t hi = pf[fh].used ? pf[fh].hiwater : 0;
    while (*cursor < hi && n < max) {
        int32_t e = find(fh, *cursor);
        (*cursor)++;
        if (e >= 0) out[n++] = unlink_entry(e);
    }
    return n;
}

#define PURGE_BATCH 64

static void purge(int fh, uint64_t *counter)
{
    for (;;) {
        uint64_t frames[PURGE_BATCH];
        /* THE CURSOR RESTARTS AT 0 EVERY BATCH, deliberately, and carrying it
         * across batches was the first version. pc_lock is dropped between
         * batches, so a page of this same file can be installed in the gap --
         * at an index BELOW where the cursor got to -- and a carried cursor
         * walks straight past it and leaves a stale page no invalidation can
         * ever reach. The old pool-scan loop restarted from slot 0 each batch
         * and did not have that hole; this keeps the property. Re-probing is
         * idempotent (an unlinked entry simply is not found), and each batch
         * removes up to 64, so it still terminates. Cost is O(hiwater) per
         * batch rather than O(pool) per batch -- for an 800-page file, ~10k
         * hash probes instead of ~850k pool slots. */
        uint64_t cursor = 0;
        uint64_t fl = spin_lock_irqsave(&pc_lock);
        int n = purge_locked(fh, frames, PURGE_BATCH, &cursor);
        /* hiwater is NOT reset here, and the first version of this function did
         * reset it. The bug: pc_lock is dropped between batches (and inside
         * pcache_get's miss, which reads a disk), so a page of this same file
         * can be installed in the gap and raise hiwater; clearing it at the end
         * of the walk would leave that entry resident with an index the NEXT
         * purge's bound excludes -- a stale page that no invalidation can ever
         * reach, which is the one thing this cache must not have.
         *
         * Only a slot taking a NEW IDENTITY resets it (pcache_file_open, both
         * places), because that is the only moment the old bound stops meaning
         * anything. Monotone is the safe direction: an over-large hiwater costs
         * a few extra hash probes on a later purge and can never miss a page. */
        int done = (n < PURGE_BATCH);
        spin_unlock_irqrestore(&pc_lock, fl);
        for (int i = 0; i < n; i++) { pmm_free(frames[i]); if (counter) (*counter)++; }
        if (done) return;
    }
}

void pcache_file_put(int fh)
{
    if (fh < 0 || fh >= PCACHE_MAXFILE) return;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    if (pf[fh].used && pf[fh].refs > 0) pf[fh].refs--;
    spin_unlock_irqrestore(&pc_lock, fl);
    /* refs == 0 is CACHED-IDLE, not gone -- and that distinction is the whole
     * point of a page cache. The first version of this function purged here,
     * which quietly defeated the design's headline claim: a program that read
     * a file once dropped every page of it on the way out, so a standalone
     * pcache_pread() missed EVERYTHING on every call (open, read, put, purge --
     * found by the adversarial pass over mm_pcache_test, not by inspection).
     * The read-once pages are precisely what reclaim's tier 1 exists to take
     * back cheaply UNDER PRESSURE (rmap 0 + our one pmm ref: the cheapest
     * frames on the machine); purging them eagerly here spends that for free
     * memory nobody asked for.
     *
     * An idle entry's pages now leave exactly three ways, all deliberate:
     * invalidation (a write/delete/rename made them stale -- and for an idle
     * entry the SLOT goes too, see pcache_invalidate_path, because a dead
     * file's inode number can be reused and a lingering identity would alias
     * the next file to wear it); reclaim taking frames one at a time under
     * pressure (pcache_forget_frame); or the slot being evicted for a new
     * file when the table is full (pcache_file_open).
     *
     * Nothing is unmapped here, same as before: a process that still has the
     * file mapped holds its own reference (its VMA, taken by
     * vma_space_clone), so a mapped file can never be idle. */
}

/* After an invalidation, an entry that nobody holds is retired outright --
 * pages AND slot. With CACHED-IDLE in the design this is a correctness rule,
 * not tidiness: the file behind an invalidation may have been DELETED, and
 * logitfs reuses inode numbers, so an idle entry left wearing a dead (dev,ino)
 * would be matched by the NEXT file to receive that inode -- and would serve it
 * pages re-read through a path that no longer means it. An entry with holders
 * keeps its slot (they re-fault and re-read, which is the invalidation
 * contract); only the unowned identity is dangerous to keep. */
static void retire_if_idle(int fh)
{
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    if (pf[fh].used && pf[fh].refs <= 0) pf[fh].used = 0;
    spin_unlock_irqrestore(&pc_lock, fl);
}

void pcache_invalidate_file(int fh)
{
    if (fh < 0 || fh >= PCACHE_MAXFILE) return;
    if (pc_ops && pc_ops->forget) pc_ops->forget(pf[fh].used ? pf[fh].path : 0);
    purge(fh, &c_inval);
    retire_if_idle(fh);
}

void pcache_invalidate_path(const char *path)
{
#ifdef PCACHE_NO_INVALIDATE
    /* NEGATIVE CONTROL: the write path stops telling the cache. A program that
     * writes a file and reads it back then gets what used to be in it. */
    (void)path;
    return;
#else
    if (!pc_ready || !path) return;
    /* The backend first, and unconditionally: it may be holding a copy of this
     * file's bytes whether or not any PAGE of it is resident here, so making
     * this conditional on a hit below would leave the stale copy behind in
     * exactly the case where nothing else would notice. */
    if (pc_ops && pc_ops->forget) pc_ops->forget(path);
    /* By PATH and not by (dev, ino): the caller is a mutation that may have
     * just made the name mean a different inode (rename, delete-and-recreate),
     * so the identity on the medium is not necessarily the one to look up. Both
     * are matched -- the name, and any entry whose inode the name resolves to
     * right now -- because either alone leaves a case uncovered. */
    int hits[PCACHE_MAXFILE];
    int nh = 0;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    for (int i = 0; i < PCACHE_MAXFILE; i++)
        if (pf[i].used && pc_sneq(pf[i].path, path, PCACHE_PATHMAX) == 0)
            hits[nh++] = i;
    spin_unlock_irqrestore(&pc_lock, fl);

    if (pc_ops && pc_ops->stat) {
        uint64_t dev = 0, ino = 0, size = 0;
        if (pc_ops->stat(path, &dev, &ino, &size) == 0) {
            fl = spin_lock_irqsave(&pc_lock);
            for (int i = 0; i < PCACHE_MAXFILE; i++) {
                if (!pf[i].used || pf[i].dev != dev || pf[i].ino != ino) continue;
                pf[i].size = size;
                int seen = 0;
                for (int j = 0; j < nh; j++) if (hits[j] == i) seen = 1;
                if (!seen) hits[nh++] = i;
            }
            spin_unlock_irqrestore(&pc_lock, fl);
        }
    }
    for (int j = 0; j < nh; j++) { purge(hits[j], &c_inval); retire_if_idle(hits[j]); }
#endif
}

/* ---------------------------------------------------------------- pages -- */

/* Make room when the entry pool is full.
 *
 * PASS 1 takes a page NOTHING maps, and "nothing maps it" is read off the
 * refcount: the cache's own reference is one, so a refcount of exactly one
 * means no PTE anywhere points at this frame. Taking a mapped PAGE would mean
 * tearing down PTEs, which is reclaim's job and needs reclaim's machinery (the
 * reverse map, the busy-elsewhere check, the TLB); doing a second, weaker copy
 * of it here is how the two would drift apart. So the division is: the cache
 * takes back what is free to take, and reclaim -- which can unmap -- takes the
 * rest, through the drop tier.
 *
 * PASS 2 IS NEW, AND IT IS NOT THAT. It does not unmap anything and does not
 * free a frame: it takes back the ENTRY while the PAGE stays exactly where it
 * is, mapped, valid, and holding its bytes. The victim frame loses one pmm
 * reference (the cache's) and keeps one per PTE, so rmap_count == pmm_refcount
 * again with pcache_holds() now 0 -- three independently maintained numbers
 * that still agree, which is the only invariant reclaim.h asks of anyone.
 *
 * WHY IT HAD TO EXIST. Without it, a pool full of mapped pages made
 * pcache_get() hand its caller a page with NO ENTRY BEHIND IT, and fault.c's
 * do_file() then took a second reference on it and installed one PTE. That
 * frame has rmap_count 1, pcache_holds 0 and pmm_refcount 2: it fails
 * reclaim's eligibility test forever, and on process exit the PTE's reference
 * is dropped and the allocation reference is not. ONE LEAKED FRAME PER FAULT
 * PAST THE CEILING, counted by nothing. The alternative fixes were worse:
 * returning 0 declines the fault and kills the process (a memory shortage that
 * presents as a segfault), and changing what pcache_get() returns means
 * changing fault.c, which is another line's file this run.
 *
 * WHAT IT COSTS, SAID PLAINLY: the victim page stops being shareable (a later
 * mapping of the same file page misses and reads a second copy) and stops
 * being tier-1 droppable (VMM_PTE_FILE is not VMM_PTE_ANON, so try_drop
 * declines it and it can only leave through swap). That is the same loss the
 * uncached hand-back had, minus the leak, and moved onto the COLDEST page
 * instead of the one just faulted. It is counted separately from an ordinary
 * eviction because it is a different event: `evicted` is the cache working,
 * `orphaned` is the pool being too small for the workload.
 *
 * It does NOT widen the coherence hole. pcache_invalidate_*() has never torn
 * down a PTE -- it removes entries so the NEXT fault re-reads -- so a process
 * that already has a file page mapped already keeps its old bytes across a
 * write. An orphaned page is in exactly that pre-existing state.
 *
 * Caller holds pc_lock. Returns the victim frame (the caller drops the cache's
 * reference on it), or 0. */
static uint64_t evict_one_locked(void)
{
    for (uint32_t tries = 0; tries < pc_npage; tries++) {
        uint32_t i = pc_hand;
        pc_hand = (pc_hand + 1 >= pc_npage) ? 0 : pc_hand + 1;
        if (pg[i].fh < 0) continue;
        if (pmm_refcount(pg[i].phys) != 1) continue;     /* somebody maps it */
        c_evict++;
        return unlink_entry((int32_t)i);
    }
#ifndef PCACHE_NO_ORPHAN
    for (uint32_t tries = 0; tries < pc_npage; tries++) {
        uint32_t i = pc_hand;
        pc_hand = (pc_hand + 1 >= pc_npage) ? 0 : pc_hand + 1;
        if (pg[i].fh < 0) continue;
        c_orphan++;
        return unlink_entry((int32_t)i);
    }
#else
    /* NEGATIVE CONTROL (-DPCACHE_NO_ORPHAN): pass 2 removed, which is the code
     * exactly as it stood before 2026-08-20. It is the SHIPPED WRONG VERSION,
     * not a fault injected to be easy to catch, and that is the point -- every
     * functional assertion in this tree still passes against it, because the
     * fault still succeeds and the process still runs and reads the right
     * bytes. The only thing it does is lose one 4 KiB frame per fault, forever,
     * with no counter and no console line. tests/unit/mm_pcache_test.c's
     * t_pool_full_of_mapped is REQUIRED to fail here. */
#endif
    return 0;                    /* pc_npage == 0: the cache never came up */
}

/* Caller holds pc_lock, and has already established that (fh, index) is NOT in
 * the table. Takes an entry FROM THE FREE LIST ONLY and returns 0, or -1 if the
 * pool has none.
 *
 * It never evicts, and that is the point of it being a function. The
 * single-page path below calls evict_one_locked() first and then this;
 * readahead calls only this. So "a speculative page never displaces a page
 * somebody actually asked for" is enforced by which function is reachable from
 * where, rather than by a rule a later reader has to remember to keep. */
static int install_locked(int fh, uint64_t index, uint64_t frame)
{
    if (pc_free < 0) return -1;
    int32_t e = pc_free;
    pc_free = pg[e].hnext;
    pg[e].fh = (int16_t)fh;
    pg[e].index = (uint32_t)index;
    pg[e].phys = frame;
    uint32_t b = hash(fh, index);
    pg[e].hnext = bucket[b];
    bucket[b] = e;
    pc_of_frame[frame / FRAME_SIZE] = (uint32_t)e + 1;
    if (index + 1 > pf[fh].hiwater) pf[fh].hiwater = index + 1;   /* purge's bound */
    c_resident++;
    if (c_resident > c_peak) c_peak = c_resident;
    return 0;
}

/* ------------------------------------------------------------ readahead --
 * pcache.h's READAHEAD block is the design and the three bounds on K; this is
 * the mechanism. */

/* How many pages this request should bring in, INCLUDING the one being asked
 * for. 1 means no prefetch and is the old behaviour exactly.
 *
 * Caller holds pc_lock. This is the ONE place the sequence state moves, and it
 * moves on every request, HIT OR MISS. A window that only misses could advance
 * would collapse on its own success: once N..N+K are resident the faults for
 * N+1..N+K are hits, so the next miss at N+K+1 would look like a fresh random
 * touch and the window would restart from nothing on every batch. */
static unsigned ra_advance(int fh, uint64_t index, int hit)
{
    struct pfile *f = &pf[fh];
    int seq = (f->ra_next != 0 && (uint64_t)f->ra_next == index);
#ifdef PCACHE_RA_ALWAYS
    /* NEGATIVE CONTROL: readahead without the sequential test -- prefetch on
     * every miss. This is the PLAUSIBLE wrong version, not the feature broken:
     * it makes the sequential case look exactly as good (which is why somebody
     * would ship it), and pays K pages of device time and K frames for every
     * single page of a random walk. Only a test that measures the RANDOM case
     * can tell the two apart. */
    seq = 1;
#endif
    f->ra_next = (uint32_t)(index + 1);
    if (!seq) { f->ra_win = 0; return 1; }
    if (hit) return 1;              /* the trail was the point; the page is here */
#ifdef PCACHE_NO_READAHEAD
    /* NEGATIVE CONTROL: never prefetch. The trail above is still kept so the
     * control differs from the shipped code in exactly one thing -- whether
     * anything is fetched -- and in nothing else. */
    return 1;
#else
    unsigned k = f->ra_win ? f->ra_win * 2 : PCACHE_RA_MIN;
    if (k > PCACHE_RA_MAX) k = PCACHE_RA_MAX;
    f->ra_win = k;
    return k + 1;
#endif
}

/* Caller holds pc_lock. How many entries the free list has, counted up to
 * `max` and NOT ONE FURTHER: the list is tens of thousands of links long on a
 * 512 MiB machine and the only question ever asked of it is "at least this
 * many?". Walking it beats keeping a count that a second structure would have
 * to maintain correctly on every install, evict, purge and forget. */
static unsigned free_slots_locked(unsigned max)
{
    unsigned n = 0;
    for (int32_t e = pc_free; e >= 0 && n < max; e = pg[e].hnext) n++;
    return n;
}

/* Fetch pages `first` .. `first + want - 1` in as few backend calls as the
 * frames allow, install them, and return the frame holding page `first`. A
 * return of 0 means "this did nothing at all", and sends the caller to the
 * ordinary single-page path -- which is why every bound below can SHRINK the
 * batch to nothing without a special case: the demand page is served either
 * way, by one path or the other.
 *
 * THE ORDER IS ALLOCATE, THEN READ, THEN INSTALL, and it is not the obvious
 * one. Installing each page as it arrives is wrong here, for a reason worth
 * writing down because nothing about it is visible at the call site:
 * pmm_alloc() calls reclaim_on_alloc(), and a page THIS BATCH has already
 * installed is rmap_count 0, pcache_holds 1, refcount 1 -- reclaim's cheapest
 * and most eligible candidate on the whole machine (pcache.h says so in as
 * many words). Reclaim would take back the page we are in the middle of
 * serving and hand its frame to the very next prefetch allocation, and this
 * function would return a frame that by then holds a DIFFERENT page of the
 * file. The fault maps it, the process reads the wrong 4 KiB, and nothing logs
 * anything. Doing every allocation first closes the window rather than
 * covering it: after the last pmm_alloc() there is no allocation left on this
 * path, so nothing installed below can be reclaimed before the caller has its
 * own reference. (pmm_pin() across the batch was the other candidate. It also
 * works, costs a pin/unpin pair, and leaves the hazard present-but-guarded
 * instead of absent.) */
static uint64_t ra_batch(int fh, uint64_t first, unsigned want)
{
    uint64_t frames[PCACHE_RA_MAX + 1];
    unsigned n = want > PCACHE_RA_MAX + 1 ? PCACHE_RA_MAX + 1 : want;

    /* (1) The file's end. */
    uint64_t size = pf[fh].size;
    uint64_t npages = (size + FRAME_SIZE - 1) / FRAME_SIZE;
    if (first >= npages) return 0;
    if ((uint64_t)n > npages - first) n = (unsigned)(npages - first);

    /* (2) What is resident already, and what the pool can hold. Trimming at the
     * FIRST page that is already here keeps the run contiguous, which is the
     * only shape the block layer can merge -- a batch with a hole in it would
     * cost two device commands to save one. */
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    unsigned m = 0;
    while (m < n && find(fh, first + m) < 0) m++;
    if (m < n) n = m;
    unsigned slots = free_slots_locked(n);
    spin_unlock_irqrestore(&pc_lock, fl);
    if (slots < n) n = slots;               /* the pool is full: prefetch less */

    /* (3) Memory. A speculative read may not be the thing that pushes the
     * machine into reclaim: below the low watermark every pmm_alloc() runs a
     * pass, and adding pages nobody asked for to that is making work for the
     * hand. reclaim_low() is 0 until reclaim_init() runs, which reads as "all
     * the free frames are available" -- correct on a machine that has no
     * reclaim yet, and the same expression. */
    uint64_t freef = pmm_free_frames(), lowmark = reclaim_low();
    uint64_t headroom = freef > lowmark ? freef - lowmark : 0;
    if ((uint64_t)n > headroom) n = (unsigned)headroom;

    if (n < 2) return 0;                    /* one page IS the ordinary path */

    /* (4) Every frame, before any of them is installed. See the note above. */
    unsigned got = 0;
    while (got < n) {
        uint64_t f = pmm_alloc();
        if (!f) break;
        frames[got++] = f;
    }
    if (got < 2) {
        for (unsigned i = 0; i < got; i++) pmm_free(frames[i]);
        return 0;
    }
    n = got;
    c_ra_run++;

    /* (5) Zero, then read over it. Zeroing every page and not only the file's
     * tail is what makes a short read SAFE TO DETECT rather than dangerous to
     * miss: an uncovered page holds zeroes, never the previous owner's bytes.
     * The disclosure rule is do_anon()'s and pcache_get()'s, unchanged. */
    for (unsigned i = 0; i < n; i++) memset(mm_p2v(frames[i]), 0, FRAME_SIZE);

    unsigned covered = 0;       /* pages from `first` that hold real file bytes */
    int shortread = 0;
    for (unsigned i = 0; i < n; ) {
        /* The maximal run of frames that is CONTIGUOUS IN PHYSICAL MEMORY.
         * mm_p2v is the identity map, so one call fills all of them and the
         * filesystem sees one byte range it can coalesce (logitfs's inode_pread
         * MIDDLE loop -> bread_run). pmm's allocator hands out ascending frames
         * from a rotating hint, so on a cold sequential load this is usually
         * ONE run for the whole batch; it is measured (pcache_ra_reads) rather
         * than assumed, because on a fragmented machine it is not. */
        unsigned j = i + 1;
        while (j < n && frames[j] == frames[j - 1] + FRAME_SIZE) j++;

        uint64_t off = (first + i) * (uint64_t)FRAME_SIZE;
        uint64_t len = (uint64_t)(j - i) * FRAME_SIZE;
        if (off + len > size) len = size - off;      /* off < size: bounded at (1) */
        long r = pc_ops->read(pf[fh].path, off, mm_p2v(frames[i]), len);
        c_ra_reads++;
        if (r < 0) break;

        uint64_t nb = (uint64_t)r;
        /* CLAMPED TO WHAT WE ASKED FOR, and this is not defensive decoration:
         * `covered` indexes frames[], which is on the stack, so a backend that
         * reported more bytes than it was given room for would walk this batch
         * off the end of that array and install frames nobody allocated. The
         * ops table is a seam a filesystem implements (pcache.h), so "no
         * backend would do that" is a promise made by code this file does not
         * own. We own `len` bytes of buffer; nothing above that is real. */
        if (nb > len) nb = len;
        unsigned whole = (unsigned)(nb / FRAME_SIZE);
        /* Rounding UP is legal for exactly one page in the file -- the tail,
         * which is genuinely shorter than 4096 and complete anyway. Rounding up
         * on any other short return would install a page with a hole of zeroes
         * in the middle of it and call it file data. */
        if ((nb % FRAME_SIZE) && off + nb == size) whole++;
        covered = i + whole;
        if (nb < len) { shortread = 1; break; }
        i = j;
    }

    /* (6) Install. Free list only -- never evict for a page nobody asked for. */
    unsigned installed = 0;
    fl = spin_lock_irqsave(&pc_lock);
    for (unsigned i = 0; i < covered; i++) {
        if (find(fh, first + i) >= 0) break;        /* not reachable under the BKL */
        if (install_locked(fh, first + i, frames[i]) < 0) break;   /* pool full */
        installed++;
    }
    if (installed) c_ra_pages += installed - 1;     /* the demand page is not
                                                     * ahead of anyone */
    if (shortread) {
        /* The backend could not serve the run. Cold the window rather than
         * doubling it: the next sequential miss starts again at PCACHE_RA_MIN,
         * so a backend that can only ever hand back one page wastes four frames
         * per miss instead of thirty-two. */
        c_ra_short++;
        pf[fh].ra_win = 0;
    }
    spin_unlock_irqrestore(&pc_lock, fl);

    for (unsigned i = installed; i < n; i++) pmm_free(frames[i]);
    return installed ? frames[0] : 0;
}

uint64_t pcache_get(int fh, uint64_t index)
{
    if (!pc_ready || !pc_ops || !pc_ops->read) return 0;
    if (fh < 0 || fh >= PCACHE_MAXFILE || !pf[fh].used) return 0;
    if (index > 0xFFFFFFFFull) return 0;

    uint64_t fl = spin_lock_irqsave(&pc_lock);
    int32_t e = find(fh, index);
    unsigned batch = ra_advance(fh, index, e >= 0);
    if (e >= 0) {
        uint64_t phys = pg[e].phys;
        c_hit++;
        spin_unlock_irqrestore(&pc_lock, fl);
        return phys;
    }
    spin_unlock_irqrestore(&pc_lock, fl);

    /* A SEQUENTIAL MISS. One batch, one backend call per contiguous run of
     * frames, and the page asked for is the first of them. A 0 back means the
     * batch declined for one of its bounds and nothing was installed, so the
     * single-page path below runs exactly as it always did -- which is also
     * what makes -DPCACHE_NO_READAHEAD the old code and not an approximation
     * of it. */
    if (batch > 1) {
        uint64_t phys = ra_batch(fh, index, batch);
        if (phys) { c_miss++; return phys; }
    }

    /* MISS. Off the lock, because this reads a disk. */
    uint64_t off = index * (uint64_t)FRAME_SIZE;
    if (off >= pf[fh].size) return 0;              /* past EOF: not a page of this file */

    uint64_t frame = pmm_alloc();
    if (!frame) return 0;

    /* Zero first, then read over it. The tail page of a file is shorter than a
     * page and the bytes past the end must read as zero, not as whatever the
     * previous owner of the frame left -- the same disclosure rule do_anon()
     * follows, and the same reason. */
    memset(mm_p2v(frame), 0, FRAME_SIZE);
    uint64_t want = pf[fh].size - off;
    if (want > FRAME_SIZE) want = FRAME_SIZE;
    long got = pc_ops->read(pf[fh].path, off, mm_p2v(frame), want);
    if (got < 0) { pmm_free(frame); return 0; }
    c_miss++;

    fl = spin_lock_irqsave(&pc_lock);
    e = find(fh, index);
    if (e >= 0) {                                   /* somebody beat us to it */
        uint64_t phys = pg[e].phys;
        spin_unlock_irqrestore(&pc_lock, fl);
        pmm_free(frame);
        return phys;
    }
    if (pc_free < 0) {
        uint64_t victim = evict_one_locked();
        if (victim) {
            spin_unlock_irqrestore(&pc_lock, fl);
            pmm_free(victim);
            fl = spin_lock_irqsave(&pc_lock);
        }
    }
    if (install_locked(fh, index, frame) == 0) {
        spin_unlock_irqrestore(&pc_lock, fl);
        return frame;
    }
    /* UNREACHABLE, and counted rather than trusted.
     *
     * evict_one_locked()'s second pass takes a slot from ANY entry, so it can
     * only come back empty when the pool holds no entries at all -- and if it
     * holds none then pc_free cannot be empty either. So getting here means the
     * free list and the entry array disagree, which is a bug in this file and
     * nowhere else. (Readahead cannot reach this: it installs through
     * install_locked() alone and simply stops when the free list runs out.)
     *
     * The old code got here whenever the pool was full of mapped pages,
     * returned the frame UNCACHED, and leaked it (see evict_one_locked).
     * Handing it back is still the least-bad thing to do -- the fault
     * succeeds and the process runs -- so that is kept, but it is now
     * SAID: this counter is a leak counter, one 4 KiB frame per tick, and
     * a nonzero value is a defect report, not a capacity report. */
    c_uncached++;
    spin_unlock_irqrestore(&pc_lock, fl);
    kprintf("[pcache] BUG: pool of %d slots has no free entry and nothing "
            "to evict; page %d of %s handed back UNCACHED (frame %p LEAKED, "
            "%d so far)\n", (int)pc_npage, (int)index, pf[fh].path,
            (void *)frame, (int)c_uncached);
    return frame;
}

long pcache_pread(const char *path, void *buf, uint64_t off, uint64_t len)
{
    if (!pc_ready || !pc_ops || !pc_ops->stat) return -1;
    if (!path || !buf) return -1;

    uint64_t dev, ino, size;
    if (pc_ops->stat(path, &dev, &ino, &size) < 0) return -1;
    if (size > PCACHE_STREAM_BYTES) { c_bypass++; return -1; }   /* see pcache.h */
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    if (!len) return 0;

    int fh = pcache_file_open(path);
    if (fh < 0) return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint64_t done = 0;
    while (done < len) {
        uint64_t p = (off + done) / FRAME_SIZE;
        uint64_t in_page = (off + done) % FRAME_SIZE;
        uint64_t n = FRAME_SIZE - in_page;
        if (n > len - done) n = len - done;
        uint64_t frame = pcache_get(fh, p);
        if (!frame) { pcache_file_put(fh); return done ? (long)done : -1; }
        /* THE IDENTITY, in one line: these are the bytes of the very frame an
         * mmap of this page would install in a page table. read() copies out of
         * it; mmap() maps it. There is one copy of the file in RAM. */
        memcpy(dst + done, (const uint8_t *)mm_p2v(frame) + in_page, (size_t)n);
        done += n;
    }
    pcache_file_put(fh);
    return (long)done;
}

/* ---------------------------------------------------------------- audit -- */

int pcache_audit(void)
{
    int errs = 0;
    if (!pc_ready) return 0;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    uint64_t resident = 0;
    for (uint32_t i = 0; i < pc_npage; i++) {
        if (pg[i].fh < 0) continue;
        resident++;
        uint64_t f = pg[i].phys / FRAME_SIZE;
        if (pg[i].phys == 0 || f >= pc_frames) {
            kprintf("[pcache] AUDIT: entry %d holds bad frame %p\n", (int)i, (void *)pg[i].phys);
            errs++; continue;
        }
        if (pc_of_frame[f] != i + 1) {
            kprintf("[pcache] AUDIT: frame %p maps to slot %d, entry %d claims it\n",
                    (void *)pg[i].phys, (int)pc_of_frame[f] - 1, (int)i);
            errs++;
        }
        if (pmm_refcount(pg[i].phys) == 0) {
            /* The dangling entry this design's one hook exists to prevent. If
             * this ever fires, reclaim freed a cached frame without calling
             * pcache_forget_frame(), and the next hit hands out somebody
             * else's memory. */
            kprintf("[pcache] AUDIT: entry %d holds FREE frame %p -- "
                    "a reclaim path is not calling pcache_forget_frame()\n",
                    (int)i, (void *)pg[i].phys);
            errs++;
        }
        if (pf[pg[i].fh].used == 0) {
            kprintf("[pcache] AUDIT: entry %d belongs to dead file slot %d\n",
                    (int)i, (int)pg[i].fh);
            errs++;
        }
    }
    if (resident != c_resident) {
        kprintf("[pcache] AUDIT: %d entries resident, counter says %d\n",
                (int)resident, (int)c_resident);
        errs++;
    }
    spin_unlock_irqrestore(&pc_lock, fl);
    return errs + (int)c_bug;
}

void pcache_report(const char *tag)
{
    if (!pc_ready) { kprintf("[pcache] %s: not up\n", tag ? tag : "-"); return; }
    kprintf("[pcache] %s: %d pages resident (peak %d of %d slots), %d files; "
            "%d hits, %d misses, %d shared\n",
            tag ? tag : "-", (int)c_resident, (int)c_peak, (int)pc_npage,
            (int)pcache_files(), (int)c_hit, (int)c_miss, (int)pcache_shared());
    kprintf("[pcache] %s: %d dropped by reclaim, %d evicted here, "
            "%d invalidated by a write, %d whole-file reads bypassed\n",
            tag ? tag : "-", (int)c_drop, (int)c_evict, (int)c_inval, (int)c_bypass);
    /* THE CEILING LINE. Everything above is the cache working; these two
     * numbers are the pool being too small for what is asked of it, and
     * neither existed before 2026-08-20 -- the pool has never had a consumer
     * that could reach its top, so its failure mode had never been printed.
     * `orphaned` is a capacity report and `uncached` is a defect report; they
     * are on the same line because a reader chasing "where did my sharing go"
     * needs to see which of the two happened. */
    kprintf("[pcache] %s: pool %d/%d slots used at peak (%d%%); "
            "%d entries orphaned (pool full of mapped pages), "
            "%d pages handed back UNCACHED AND LEAKED\n",
            tag ? tag : "-", (int)c_peak, (int)pc_npage,
            (int)(pc_npage ? (c_peak * 100) / pc_npage : 0),
            (int)c_orphan, (int)c_uncached);
    /* READAHEAD, and every number here is half of a ratio on purpose. `pages
     * per read` is the coalescing factor -- 1 means the frames came back
     * scattered and the block layer had nothing to merge, which is a real
     * outcome on a fragmented machine and is not distinguishable from "the
     * feature is off" without this line. `short` is a backend that stopped
     * early; on logitfs it should be 0, and a nonzero value there means
     * vfs_pread is refusing multi-page reads, not that the cache is wrong. */
    kprintf("[pcache] %s: readahead %d batches, %d pages ahead, %d backend "
            "reads (%d pages/read), %d short; K %d..%d\n",
            tag ? tag : "-", (int)c_ra_run, (int)c_ra_pages, (int)c_ra_reads,
            (int)(c_ra_reads ? (c_ra_pages + c_ra_run) / c_ra_reads : 0),
            (int)c_ra_short, (int)PCACHE_RA_MIN, (int)PCACHE_RA_MAX);
}
