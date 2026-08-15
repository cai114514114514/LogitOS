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

/* Resident pages. 4096 entries is a 16 MiB ceiling on cached file data, which
 * is a bound and not a target: reclaim takes these back under pressure long
 * before the ceiling matters, and the ceiling exists so that a machine with no
 * pressure at all still has a number. Clamped further at init to one sixteenth
 * of RAM, so the 192 MiB machine the swap harness boots does not give a third
 * of its memory to page cache before reclaim has said anything. */
#define PCACHE_MAXPAGE 4096

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
 *            Always page aligned and at most 4096 bytes from here. */
struct pcache_ops {
    int  (*stat)(const char *path, uint64_t *dev, uint64_t *ino, uint64_t *size);
    long (*read)(const char *path, uint64_t off, void *dst, uint64_t len);
};
void pcache_set_ops(const struct pcache_ops *ops);

/* --- accounting; a cache nobody has watched work is not a cache ---------- */
uint64_t pcache_hits(void);
uint64_t pcache_misses(void);       /* a page read off the device */
uint64_t pcache_resident(void);     /* pages held right now */
uint64_t pcache_peak(void);
uint64_t pcache_dropped(void);      /* pages reclaim took */
uint64_t pcache_evicted(void);      /* pages the cache took back itself (pool full) */
uint64_t pcache_invalidated(void);  /* pages a write threw away */
uint64_t pcache_shared(void);       /* pages currently mapped more than once */
uint64_t pcache_bypassed(void);     /* whole-file reads too big to install */
uint64_t pcache_files(void);        /* live file entries */
int      pcache_audit(void);        /* every entry: frame allocated, refcount sane */
void     pcache_report(const char *tag);

#endif /* LOGIT_PCACHE_H */
