#include <stdint.h>
#include <stddef.h>
#include "pcache.h"
#include "pmm.h"
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
    char     path[PCACHE_PATHMAX];
};

struct pentry {
    uint64_t phys;
    uint32_t index;         /* page index within the file */
    int16_t  fh;            /* -1 = free */
    int32_t  hnext;         /* next in this hash bucket, or -1 */
};

#define PC_NBUCKET 1024     /* power of two; PCACHE_MAXPAGE / 4 */

static struct pfile  pf[PCACHE_MAXFILE];
static struct pentry pg[PCACHE_MAXPAGE];
static int32_t       bucket[PC_NBUCKET];
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

static uint64_t c_hit, c_miss, c_drop, c_evict, c_inval, c_bypass, c_peak, c_resident;
static uint64_t c_bug;

uint64_t pcache_hits(void)        { return c_hit; }
uint64_t pcache_misses(void)      { return c_miss; }
uint64_t pcache_resident(void)    { return c_resident; }
uint64_t pcache_peak(void)        { return c_peak; }
uint64_t pcache_dropped(void)     { return c_drop; }
uint64_t pcache_evicted(void)     { return c_evict; }
uint64_t pcache_invalidated(void) { return c_inval; }
uint64_t pcache_bypassed(void)    { return c_bypass; }
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
    for (uint32_t i = 0; i < pc_npage; i++)
        if (pg[i].fh >= 0 && pmm_refcount(pg[i].phys) > 2) n++;
    return n;
}

void pcache_set_ops(const struct pcache_ops *ops) { pc_ops = ops; }

/* ------------------------------------------------------------------ init -- */

void pcache_init(uint64_t total_frames)
{
    if (pc_ready || total_frames == 0) return;

    /* The pool is a CEILING, not a target: reclaim takes these pages back long
     * before it is reached. Clamped to a sixteenth of RAM so that the small
     * machines the pressure harnesses boot (192 MiB, sometimes 128) do not give
     * a third of themselves to page cache before reclaim has had a word. */
    uint64_t n = PCACHE_MAXPAGE;
    if (n > total_frames / 16) n = total_frames / 16;
    if (n < 32) n = 32;
    if (n > PCACHE_MAXPAGE) n = PCACHE_MAXPAGE;
    pc_npage = (uint32_t)n;

    uint64_t bytes = total_frames * 4;
    uint64_t frames = (bytes + FRAME_SIZE - 1) / FRAME_SIZE;
    uint64_t base = pmm_alloc_contig((size_t)frames);
    if (!base) {
        /* Correct degradation, and loud: with no frame table there is no O(1)
         * way to tell reclaim that a frame it is about to take is ours, so the
         * cache stays OFF entirely rather than running without the one hook
         * that keeps its entries from dangling. The kernel then behaves exactly
         * as it did before this line: file mappings are refused, reads go
         * straight to the backend. */
        kprintf("[pcache] init FAILED: no %d contiguous frames for the frame table "
                "-- the page cache is DISABLED for this boot\n", (int)frames);
        return;
    }
    pc_of_frame = (uint32_t *)mm_p2v(base);
    pc_frames = total_frames;
    memset(pc_of_frame, 0, (size_t)(total_frames * 4));

    for (int i = 0; i < PC_NBUCKET; i++) bucket[i] = -1;
    for (uint32_t i = 0; i < pc_npage; i++) {
        pg[i].fh = -1;
        pg[i].phys = 0;
        pg[i].hnext = (i + 1 < pc_npage) ? (int32_t)(i + 1) : -1;
    }
    pc_free = 0;
    pc_ready = 1;

    kprintf("[pcache] up: %d page slots (%d KiB ceiling), %d file slots, "
            "frame table %d KiB%s\n",
            (int)pc_npage, (int)(pc_npage * 4), PCACHE_MAXFILE,
            (int)(total_frames * 4 / 1024),
#ifdef PCACHE_PER_OPEN
            "  [NEGATIVE CONTROL: keyed per-open, not per-inode]"
#else
            ""
#endif
            );
}

/* --------------------------------------------------------------- lookup -- */

static inline uint32_t hash(int fh, uint64_t index)
{
    uint64_t h = ((uint64_t)fh * 0x9E3779B97F4A7C15ull) ^ (index * 0xC2B2AE3D27D4EB4Full);
    return (uint32_t)((h >> 32) & (PC_NBUCKET - 1));
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
            pf[i].refs++;
            /* The size can have moved under us since the last open (logitfs
             * rewrites a whole file per write); the invalidation on write has
             * already emptied the pages, so refreshing the length here is all
             * that is left to do. */
            pf[i].size = size;
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
            size_t n = pc_slen(path);
            if (n >= PCACHE_PATHMAX) { pf[i].used = 0; goto out; }
            memcpy(pf[i].path, path, n + 1);
            ret = i;
            goto out;
        }
    /* Table full. Not fatal and not silent: the mapping is refused, the caller
     * falls back to reading the file, and the number says the table is too
     * small for the workload rather than that the workload is wrong. */
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

/* Caller holds pc_lock. Every resident page of `fh`, unlinked; the frames come
 * back on `out` so the reference drops can happen with the lock released. */
static int purge_locked(int fh, uint64_t *out, int max)
{
    int n = 0;
    for (uint32_t i = 0; i < pc_npage && n < max; i++)
        if (pg[i].fh == fh) out[n++] = unlink_entry((int32_t)i);
    return n;
}

#define PURGE_BATCH 64

static void purge(int fh, uint64_t *counter)
{
    for (;;) {
        uint64_t frames[PURGE_BATCH];
        uint64_t fl = spin_lock_irqsave(&pc_lock);
        int n = purge_locked(fh, frames, PURGE_BATCH);
        spin_unlock_irqrestore(&pc_lock, fl);
        for (int i = 0; i < n; i++) { pmm_free(frames[i]); if (counter) (*counter)++; }
        if (n < PURGE_BATCH) return;
    }
}

void pcache_file_put(int fh)
{
    if (fh < 0 || fh >= PCACHE_MAXFILE) return;
    uint64_t fl = spin_lock_irqsave(&pc_lock);
    int last = 0;
    if (pf[fh].used && --pf[fh].refs <= 0) { pf[fh].used = 0; last = 1; }
    spin_unlock_irqrestore(&pc_lock, fl);
    /* The pages go when the last reference does. Note what does NOT happen
     * here: nothing is unmapped. A process that still has the file mapped holds
     * its own reference (its VMA), so `last` cannot be true while a mapping
     * exists -- which is exactly why the reference lives on the VMA and is
     * taken by vma_space_clone(). */
    if (last) purge(fh, 0);
}

void pcache_invalidate_file(int fh)
{
    if (fh < 0 || fh >= PCACHE_MAXFILE) return;
    purge(fh, &c_inval);
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
    for (int j = 0; j < nh; j++) purge(hits[j], &c_inval);
#endif
}

/* ---------------------------------------------------------------- pages -- */

/* Make room when the entry pool is full.
 *
 * Only a page NOTHING maps may be taken here, and "nothing maps it" is read off
 * the refcount: the cache's own reference is one, so a refcount of exactly one
 * means no PTE anywhere points at this frame. Taking a mapped page would mean
 * tearing down PTEs, which is reclaim's job and needs reclaim's machinery (the
 * reverse map, the busy-elsewhere check, the TLB); doing a second, weaker copy
 * of it here is how the two would drift apart. So the division is: the cache
 * takes back what is free to take, and reclaim -- which can unmap -- takes the
 * rest, through the drop tier.
 *
 * Caller holds pc_lock. Returns the freed frame, or 0. */
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
    return 0;
}

uint64_t pcache_get(int fh, uint64_t index)
{
    if (!pc_ready || !pc_ops || !pc_ops->read) return 0;
    if (fh < 0 || fh >= PCACHE_MAXFILE || !pf[fh].used) return 0;
    if (index > 0xFFFFFFFFull) return 0;

    uint64_t fl = spin_lock_irqsave(&pc_lock);
    int32_t e = find(fh, index);
    if (e >= 0) {
        uint64_t phys = pg[e].phys;
        c_hit++;
        spin_unlock_irqrestore(&pc_lock, fl);
        return phys;
    }
    spin_unlock_irqrestore(&pc_lock, fl);

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
    if (pc_free < 0) {
        /* Every slot is taken and every page in them is mapped by somebody. The
         * page is still handed back -- UNCACHED, with its allocation reference
         * intact -- so the fault succeeds and the process runs; it simply does
         * not get to share. Counted as an eviction failure through the pool
         * being too small, not hidden. */
        spin_unlock_irqrestore(&pc_lock, fl);
        return frame;
    }
    e = pc_free;
    pc_free = pg[e].hnext;
    pg[e].fh = (int16_t)fh;
    pg[e].index = (uint32_t)index;
    pg[e].phys = frame;
    uint32_t b = hash(fh, index);
    pg[e].hnext = bucket[b];
    bucket[b] = e;
    pc_of_frame[frame / FRAME_SIZE] = (uint32_t)e + 1;
    c_resident++;
    if (c_resident > c_peak) c_peak = c_resident;
    spin_unlock_irqrestore(&pc_lock, fl);
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
}
