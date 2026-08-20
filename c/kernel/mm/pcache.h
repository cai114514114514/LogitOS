#ifndef LOGIT_PCACHE_H
#define LOGIT_PCACHE_H

#include <stdint.h>

/* THE PAGE CACHE: file pages, keyed by (file, page index), and the frame IS
 * the thing that gets mapped.
 *
 * ===========================================================================
 * WHY THIS IS NOT bcache.c
 *
 * c/fs/bcache.c already caches 4 KiB units read off a disk, and it would be
 * easy to read this file as a second copy of it. It is not, and the difference
 * is the entire reason this exists:
 *
 *   a BUFFER cache holds DISK BLOCKS, keyed by (device, block number). Its
 *   contents are private to the filesystem; the only way anything above it
 *   sees a byte is by COPYING out of a buffer.
 *
 *   a PAGE cache holds FILE PAGES, keyed by (file, page index), and the page
 *   it holds is a physical frame that can be MAPPED. The cached page and the
 *   page in the process's address space are the same frame.
 *
 * That identity is the whole point, and everything this line claims follows
 * from it and from nothing else:
 *
 *   - read() and mmap() return the same memory, because there is one copy of
 *     the file's bytes in RAM rather than one per mechanism;
 *   - two processes that open the same file share it, because they find the
 *     same entry under the same key -- which is why the key is the FILE and
 *     not the open file description (see the negative control below);
 *   - a clean file page is DROPPABLE. It can be handed back to the frame
 *     allocator with no device write at all, because the file still holds the
 *     bytes and the next fault re-reads them.
 *
 * That last one is what this was built for. c/kernel/mm/reclaim.h says, in the
 * long comment at the top, that TIER 1 -- drop a clean page and re-derive it --
 * had NO PRODUCER on this machine, because nothing here was file-backed, and
 * that the all-zero anonymous page was standing in for one. This file is the
 * real producer. reclaim.c's try_drop() now has a second, larger population to
 * work on, and the split between the tiers is a measured number
 * (tests/boot/run-swap-test.sh already prints it).
 *
 * ===========================================================================
 * THE REFCOUNT DECISION, WRITTEN DOWN BECAUSE IT IS THE ONE THAT MATTERS
 *
 * Reclaim's whole safety argument is one line (rmap.h):
 *
 *     evict only if   rmap_count(f) == pmm_refcount(f),
 *
 * the same number arrived at from two structures that are maintained
 * independently, so that a bug in either costs reclaimability and never costs
 * correctness. A cache entry is a reference to a frame with NO leaf PTE behind
 * it -- structurally the same shape as a page-table page, a kheap arena or a
 * DMA ring, all of which are excluded from eviction *by that very test*,
 * because their rmap count is 0 and their refcount is not.
 *
 * So the question has to be answered explicitly, and there are only two
 * answers:
 *
 *   (a) THE CACHE ENTRY IS A PIN. Structurally excluded, like a page table.
 *       REJECTED, and it is worth being precise about why, because it is the
 *       answer that "falls out" of the existing machinery and it is wrong.
 *       Every page this cache ever holds would become permanently unevictable.
 *       A file page mapped into one process would have refcount 2 (one PTE,
 *       one cache entry) against an rmap count of 1, fail the test, and be
 *       counted as skip_partial forever. The cache would be a monotonically
 *       growing region of memory that reclaim is forbidden to touch -- which
 *       is not a page cache, it is a leak with a hash table in front of it.
 *       And it would deliver the exact opposite of the thing this line was
 *       asked for: instead of GIVING tier 1 its producer, it would take the
 *       pages away from tier 1 permanently.
 *
 *   (b) THE CACHE ENTRY IS AN ORDINARY REFERENCE AND THE PAGE STAYS
 *       EVICTABLE. Chosen. The cache holds exactly one pmm reference per
 *       resident page and takes no pin, and reclaim's eligibility test gains
 *       ONE TERM:
 *
 *           rmap_count(f) + pcache_holds(f) == pmm_refcount(f)
 *
 *       where pcache_holds(f) is 0 or 1 and comes from a THIRD structure
 *       maintained independently of the other two (pc_of_frame[], below). The
 *       property that made the original test worth having is preserved
 *       exactly: three independently maintained numbers have to agree, and a
 *       disagreement stops the eviction rather than permitting a wrong one.
 *
 * A CACHED-BUT-UNMAPPED PAGE IS THEREFORE EVICTABLE, NOT PINNED, and it is the
 * CHEAPEST thing on the machine to reclaim: rmap_count 0, refcount 1, no PTE to
 * tear down, no device write, one entry removed and the frame is back. Pinning
 * it would mean a program that read a file once had permanently spent that
 * memory. That is the whole answer to the question the design note asks.
 *
 * The cost of (b) is one hook: when reclaim takes a frame this cache holds, the
 * cache has to be told, or the entry dangles onto a frame the allocator has
 * handed to somebody else. pcache_forget_frame() is that hook, it is O(1)
 * through pc_of_frame[], and reclaim's drop tier calls it while it still holds
 * the big kernel lock that made the decision.
 *
 * ===========================================================================
 * WHAT IS NOT HERE, DELIBERATELY
 *
 * READ-ONLY MAPPINGS ONLY. There is no dirty page, no writeback, no msync and
 * no writable MAP_SHARED. A writable file mapping is REFUSED OUT LOUD
 * (mmsys.c) rather than silently downgraded to a private copy, because a
 * silent downgrade is a program whose writes go nowhere and whose author has
 * no way to find out.
 *
 * The reason is not that writeback is hard in the abstract; it is that a dirty
 * file page reaching the device has to be ordered against the three barriers
 * above log_commit() in c/fs/logitfs.c, and that argument is a separate piece
 * of work with its own crash sweep. Everything in this file is a pure reader:
 * the device is always the truth, a cached page is always either equal to what
 * is on the device or removed, and there is no third state.
 *
 * WHICH IS ALSO THE COHERENCE ANSWER. The only way a file's bytes change is
 * vfs_write / vfs_delete / vfs_rename, and each of those calls
 * pcache_invalidate_path() (c/fs/vfs.c). Read-then-write-then-read therefore
 * cannot see stale bytes. `-DPCACHE_NO_INVALIDATE` removes exactly that call
 * and the coherence case is REQUIRED to fail against it.
 *
 * ===========================================================================
 * THE NEGATIVE CONTROL: -DPCACHE_PER_OPEN
 *
 * Not "remove the cache" -- removing it fails everything and proves nothing
 * about the design. The control keys the cache on the OPEN rather than on the
 * FILE, which is the single most plausible wrong version of this file. Under
 * it every single-process test still passes: read() and mmap() still agree,
 * the pages are still cached, the performance still looks fine. What silently
 * disappears is both things the cache exists for -- two processes stop sharing
 * a page, and a page dropped by reclaim comes back in a DIFFERENT frame while
 * another mapping still points at the old one. tests/unit/mm_pcache_test.c is
 * required to fail against that build.
 * ======================================================================== */

/* A file this cache knows about: the identity, and how to re-read it.
 *
 * `dev`/`ino` are the identity -- two paths hard-linked to one inode are ONE
 * entry, which is the whole reason the key is not the path. The path is kept
 * only because the VFS backends are path-addressed (c/fs/vfs.h), so it is how
 * a page is re-read, not what it is filed under. */
#define PCACHE_MAXFILE 32
#define PCACHE_PATHMAX 192

/* ===========================================================================
 * HOW BIG THE POOL IS, AND WHY IT IS NO LONGER A CONSTANT
 *
 * This used to be `#define PCACHE_MAXPAGE 4096`, clamped at init to
 * total_frames/16, and the entries were a static array in kernel .bss. Three
 * measurements on 2026-08-20 said that all three of those decisions were
 * wrong, and none of them could have been seen before elf_load started
 * producing file-backed VMAs, because until then the cache had no consumer:
 *
 *   1. THE CONSTANT MEANS A DIFFERENT THING ON EVERY MACHINE. 4096 slots is
 *      16 MiB: 3.1% of the 512 MiB desktop boot, and on the 192 MiB machine
 *      the swap harness boots the clamp bites instead and gives 3072 slots =
 *      6.25%. One number, two policies, neither chosen.
 *
 *   2. 16 MiB IS NOT ENOUGH FOR THE ONE WORKLOAD THIS LINE EXISTS FOR. A
 *      284 MiB model mapped read-only off the disk is 72,704 pages. It cannot
 *      fit in 4096 slots, and what happened when it did not fit is (3).
 *
 *   3. THE FULL-POOL PATH LEAKED A FRAME PER FAULT. pcache_get() handed the
 *      page back UNCACHED with its allocation reference intact; do_file()
 *      (fault.c) then took its own reference and installed one PTE. So
 *      rmap_count 1 + pcache_holds 0 = 1 against a pmm refcount of 2 --
 *      reclaim's eligibility test (below) fails, forever, and when the
 *      process exits the PTE's reference goes and the allocation reference
 *      does not. One 4 KiB frame lost per page fault past the ceiling, with
 *      nothing counting it. On the target workload that is the machine.
 *
 * THE SIZE IS NOW DERIVED, from the one thing that bounds it: A FRAME CAN
 * HOLD AT MOST ONE CACHED FILE PAGE, so the largest pool that can ever be
 * needed is one entry per frame. Half of that is chosen, and the half is
 * derived too rather than picked: the cache must never be able to drive the
 * allocator to failure BY ITSELF, and "at most half the frames" is the
 * strongest form of that -- for every frame the cache holds there is one it
 * does not. That is the property the old 4096 was providing by accident of
 * scale; this states it.
 *
 *      slots = total_frames / PCACHE_FRAME_SHARE
 *
 * COST, MEASURED, not estimated -- `size -A build/c/kernel/mm/pcache.o` and
 * the kernel's own boot line, both on 2026-08-20, 512 MiB / 131,037 frames:
 *
 *   .bss          109,716 B  ->  7,588 B      -102,128 B, permanent, every boot
 *                                             (pg[] -98,304, bucket[] -4,096,
 *                                              pf[] +256 for the purge bound)
 *   allocated     511 KiB    ->  2,112 KiB    +1,600 KiB at init, from the PMM
 *   per frame     4 B        ->  16.5 B       +12.5 B = +0.305% of RAM
 *   ceiling       16 MiB     ->  256 MiB
 *
 * An entry is 24 bytes and the bucket array is one int32 per four entries, so
 * 12.5 B per frame is arithmetic and not a fit. It comes from
 * pmm_alloc_contig() at init like the reverse map's node pool, which is the
 * point: the static array cost its 102,400 B on every machine whether or not
 * anything ever opened a file, and a machine with 64 MiB paid the same as one
 * with 512.
 *
 * WHAT DID NOT CHANGE: the pool is still a CEILING and not a target, reclaim
 * still takes these pages back under pressure long before it is reached, and
 * the full-pool path still exists and is still reachable. It no longer leaks;
 * see pcache.c's evict_one_locked().
 *
 * OCCUPANCY, MEASURED, so that "the desktop does not come near it" is a number
 * rather than a hope (each is `pcache_report`'s peak, one QEMU boot each):
 *
 *   boot + desktop, nothing launched            45-46 pages
 *   ...+ the browser exec'd                       178 pages
 *   ...+ 25 large files mapped and every page
 *        touched at once (run-pcachefill.sh)    4,782 pages
 *
 * The last one is a workload built specifically to load this pool, and it
 * reaches 7% of it. It is also 117% of the OLD pool, which is exactly why that
 * harness exists: 4,775 mapped pages against 4,096 slots is where the old code
 * leaked 685 frames, measured. */
#define PCACHE_FRAME_SHARE 2u

/* The floor. A machine (or a host simulation) small enough that half its
 * frames is a handful still gets a usable cache rather than a degenerate one. */
#define PCACHE_MINPAGE 32u

/* ---------------------------------------------------------------------------
 * THE NEGATIVE CONTROL: -DPCACHE_LEGACY_POOL
 *
 * Not "no page cache" -- that fails everything and proves nothing about the
 * SIZE. This is the pool exactly as it shipped: `min(4096, total_frames/16)`
 * entries, and evict_one_locked() without its second pass, which together are
 * the two lines that produced the leak. Everything a program can observe about
 * itself is unchanged under it: the mappings work, the bytes are right, the
 * processes run to completion.
 *
 * tests/boot/run-pcachefill.sh is REQUIRED to fail against it, on exactly two
 * assertions -- `orphaned == 0` and `uncached == 0`. That it fails on the
 * SECOND one and not only the first is the part that matters: an orphan is the
 * pool being too small, an uncached hand-back is a frame gone.
 *
 * The Makefile rule this needs (two tokens, in the toggle block beside CHURN=1
 * and NOSHAPE=1 at Makefile:106; not added here because the Makefile is owned
 * by another line this run):
 *
 *     ifeq ($(PCLEGACY),1)
 *     CFLAGS += -DPCACHE_LEGACY_POOL
 *     endif
 *
 * and then
 *
 *     make BUILD=build/pclegacy PCLEGACY=1 build/pclegacy/logit.iso
 *     bash tests/boot/run-pcachefill.sh build/pclegacy/logit.iso build/disk.img legacy
 *
 * -- the disk is shared on purpose: the apps are identical, only the kernel
 * differs, so a rebuilt disk would be a second variable. */
#ifdef PCACHE_LEGACY_POOL
#define PCACHE_LEGACY_MAXPAGE 4096u
#define PCACHE_LEGACY_SHARE   16u
#define PCACHE_NO_ORPHAN 1
#endif

/* Files above this are read STRAIGHT THROUGH, not installed. The argument is
 * bcache.c's, one layer up and for the same reason: a 2.2 MiB font or a 1 MiB
 * .aex read once, sequentially, and never read again would evict everything
 * that is read repeatedly, so the cache would be helping small files and being
 * destroyed by big ones. A big file MAPPED is still cached page by page -- it
 * is the whole-file read that bypasses, because that is the streaming access. */
#define PCACHE_STREAM_BYTES (4u * 1024u * 1024u)

/* Bring the cache up. Takes its per-frame table from the PMM, like rmap_init
 * does and for the same reason: it must not allocate later, on the paths that
 * run when memory is short. Safe to call twice; everything below is safe to
 * call before it and answers "nothing is cached". */
void pcache_init(uint64_t total_frames);
int  pcache_ready(void);

/* --- file handles ------------------------------------------------------- */

/* Look `path` up, resolve it to (dev, ino), and return a handle to the ONE
 * entry for that inode, taking a reference. Returns -1 if the path cannot be
 * resolved, is not a regular file, or the table is full. */
int  pcache_file_open(const char *path);
void pcache_file_ref(int fh);      /* fork: the child's VMA references it too */
void pcache_file_put(int fh);      /* execve / exit / munmap */
uint64_t pcache_file_size(int fh);

/* --- pages -------------------------------------------------------------- */

/* The frame holding page `index` of file `fh`, reading it in on a miss.
 * Returns 0 if it could not be had (no frame, unreadable file, past EOF).
 *
 * The CACHE's reference is held on the returned frame; a caller that is about
 * to install a PTE must take its own with pmm_ref() first, exactly as
 * fault.c's copy-on-write path does. */
uint64_t pcache_get(int fh, uint64_t index);

/* Does the cache hold this frame? 0 or 1 -- the extra term in reclaim's
 * eligibility test. O(1), no lock, one aligned load: it is called once per
 * frame per sweep of the clock, alongside rmap_mapped(), and for the same
 * reason it is allowed to be read without the lock (the answer is only ever
 * used to decide whether to look properly). */
int  pcache_holds(uint64_t phys);

/* Reclaim took this frame. Remove the entry and drop the cache's reference.
 * Called from reclaim.c's drop tier with the frame's last PTE already gone. */
void pcache_forget_frame(uint64_t phys);

/* The file's bytes changed (or the name did): every page of it must go. The
 * ONE thing standing between this cache and a program that writes a file and
 * reads back what used to be in it. */
void pcache_invalidate_path(const char *path);
void pcache_invalidate_file(int fh);

/* Serve `len` bytes at `off` of `path` out of the cache, into `buf`. Returns
 * bytes copied, or -1 to say "not cached-serviceable, go to the backend"
 * (a streaming-sized file, or the cache is not up). This is the read() half of
 * the identity claim: the bytes come out of the very frame an mmap of the same
 * page would be given. */
long pcache_pread(const char *path, void *buf, uint64_t off, uint64_t len);

/* --- the seam --------------------------------------------------------------
 * c/kernel/mm/ may not depend on c/fs/: it is compiled for the host tests with
 * mmhost.h as its only seam, and there is no VFS there. So the two things this
 * cache needs from a filesystem are function pointers, installed by
 * pcache_init() from c/fs/ in the kernel build and by the test on the host.
 *
 *   stat  -- path -> (dev, ino, size). The identity, and the size.
 *   read  -- path, byte offset, destination, length -> bytes read, or < 0.
 *            Always page aligned and at most 4096 bytes from here.
 *   forget -- OPTIONAL (may be NULL). "this file's bytes are no longer what
 *            you last read." It exists because a backend that cannot pread
 *            has to hold a copy of the file to serve one page of it (see
 *            pcache_vfs.c), and a second copy nobody invalidates is the very
 *            bug pcache_invalidate_path() exists to prevent, one layer down.
 *            Called from every invalidation path here, so the backend does not
 *            have to guess. A backend that reads straight off a device leaves
 *            it NULL and nothing changes. */
struct pcache_ops {
    int  (*stat)(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size);
    long (*read)(const char *path, uint64_t off, void *dst, uint64_t len);
    void (*forget)(const char *path);
};
void pcache_set_ops(const struct pcache_ops *ops);

/* --- accounting; a cache nobody has watched work is not a cache ---------- */
uint64_t pcache_hits(void);
uint64_t pcache_misses(void);       /* a page read off the device */
uint64_t pcache_resident(void);     /* pages held right now */
uint64_t pcache_peak(void);
uint64_t pcache_dropped(void);      /* pages reclaim took */
uint64_t pcache_evicted(void);      /* pages the cache took back itself (pool full) */
uint64_t pcache_orphaned(void);     /* entries dropped while the page stayed MAPPED --
                                     * the pool was full of pages nothing was willing
                                     * to give up, so the coldest one lost its cache
                                     * identity (not its mapping). See evict_one_locked. */
uint64_t pcache_uncached(void);     /* THE FAILURE THAT USED TO BE INVISIBLE: a page
                                     * handed back with no entry behind it. Must be 0;
                                     * see the pool-sizing note above for why it now
                                     * cannot happen without a bug, and pcache.c for
                                     * what it costs when it does. */
uint64_t pcache_slots(void);        /* the pool's size, decided at init from RAM */
uint64_t pcache_invalidated(void);  /* pages a write threw away */
uint64_t pcache_shared(void);       /* pages currently mapped more than once */
uint64_t pcache_bypassed(void);     /* whole-file reads too big to install */
uint64_t pcache_files(void);        /* live file entries */
int      pcache_audit(void);        /* every entry: frame allocated, refcount sane */
void     pcache_report(const char *tag);

#endif /* LOGIT_PCACHE_H */
