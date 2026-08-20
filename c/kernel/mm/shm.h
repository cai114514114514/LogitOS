#ifndef LOGIT_SHM_H
#define LOGIT_SHM_H

#include <stdint.h>

/* SHARED ANONYMOUS MEMORY: one frame, several address spaces, and writes that
 * are visible to all of them.
 *
 * ===========================================================================
 * WHAT WAS ALREADY HERE, AND WHAT WAS NOT
 *
 * Half of "shared memory" existed before this file and it is worth being exact
 * about which half, because the two are easy to conflate and only one of them
 * was missing.
 *
 *   FILE-BACKED SHARING: DONE, and done properly. c/kernel/mm/pcache.c keys a
 *   cached page on (dev, ino), so two processes that map the same file land on
 *   the same physical frame -- not a copy of it. fault.c's do_file() takes a
 *   second pmm reference on the cache's frame and maps it. Two unrelated
 *   programs execing the same binary already share its text this way, and
 *   tests/boot/run-execshare-test.sh already gates it. Nothing here changes or
 *   duplicates that.
 *
 *   ANONYMOUS SHARING: ABSENT. There was no way to share memory that is not a
 *   file. `MAP_SHARED` appeared nowhere in the ABI or in c/kernel/mm; libc's
 *   mman.c refused MAP_SHARED|MAP_ANONYMOUS with ENOTSUP and argued why (fork
 *   maps every anonymous page copy-on-write, so a "shared" anonymous mapping
 *   would silently become two private ones on the first write). Every non-file
 *   IPC on this machine was therefore a COPY: a pipe.
 *
 * So this file builds the second one only, and it deliberately builds it in
 * the SHAPE of the first. A segment is "a file that never had a file behind
 * it": an object with an identity, holding one frame per page, that a fault
 * takes a second reference on. do_shm() in fault.c is do_file() with the
 * backing store swapped, and that is not a coincidence -- it is the reason the
 * refcount argument below is one already-audited argument rather than a new
 * one.
 *
 * ===========================================================================
 * THE INVARIANT, WHICH IS THE ONLY GENUINELY DANGEROUS PART
 *
 * reclaim.c evicts a frame only if
 *
 *      rmap_count(f) + pcache_holds(f) == pmm_refcount(f)
 *
 * -- the same number arrived at from three structures maintained
 * independently, so a bug in any one of them costs reclaimability and never
 * costs correctness (rmap.h states the original two-term form at length).
 *
 * A shared page is precisely the case that arithmetic was built for: N
 * processes, N leaf PTEs, N rmap entries, N references. It would BALANCE. And
 * balancing is exactly wrong here, because both of reclaim's tiers destroy the
 * sharing while preserving the bytes:
 *
 *   TIER 1 (drop) demands VMM_PTE_ANON and an all-zero page, then throws the
 *   frame away and lets do_anon() re-derive it. do_anon() allocates a FRESH
 *   FRAME PER FAULT. Two processes sharing a page that happened to be all
 *   zeroes -- which is what a segment looks like the instant before the first
 *   write -- would come back holding two different frames. No crash, no log
 *   line: process A writes 42, process B reads 0, forever.
 *
 *   TIER 2 (swap) writes the page to a slot and leaves a swap entry in every
 *   PTE. swap.h says in as many words that whichever side faults first reads
 *   it back into a private frame and "the sharing is not restored". Same
 *   silent decoupling, one encoding along.
 *
 * A MAP_SHARED FRAME MUST THEREFORE BE UNEVICTABLE. The question is how, and
 * there are two answers:
 *
 *   (a) pmm_pin() it. REJECTED. rmap.h draws the line between the two
 *       mechanisms sharply: a pin means "the kernel is busy with this frame
 *       RIGHT NOW" -- dynamic, temporary, nested, released. Structural
 *       exclusion is the other mechanism, and rmap.h's whole point about it is
 *       that it needs NO list of exceptions to forget to update. Using a pin
 *       here would put a permanent entry in a temporary mechanism and add this
 *       file to a list of things pmm_pins_live() no longer means what it says.
 *
 *   (b) THE SEGMENT HOLDS ONE ORDINARY pmm REFERENCE PER PAGE. Chosen, and it
 *       required NO CHANGE TO reclaim.c AT ALL -- which is the strongest thing
 *       that can be said for it. Walk the two states:
 *
 *         created, not yet mapped:  rmap_count 0, refcount 1
 *              -> candidate()'s first clause, `!rmap_mapped && !held`, skips it
 *                 as ordinary kernel memory. Which it is: the segment's.
 *
 *         mapped by N processes:    rmap_count N, refcount N+1
 *              -> `n + held != rc` (N != N+1), skip_partial. The comment there
 *                 already describes this exact case in the general: "somebody
 *                 holds a reference that is neither one of the PTEs we know nor
 *                 the cache's own -- the kernel is using this user page".
 *
 *       That is the SAME structural exclusion that already covers page-table
 *       frames, kheap arenas, DMA rings and the rmap's own tables, reached by
 *       the same test, with no term added and no exception listed. pcache.c
 *       rejected this very shape for ITSELF and was right to: a page cache that
 *       pins everything it touches is "a leak with a hash table in front of
 *       it". A segment is the opposite kind of object -- a fixed-size
 *       allocation a program asked for by name and will free by name, like an
 *       open file rather than like a cache of one. Making it unevictable is not
 *       a leak; it is what the program requested.
 *
 * The cost, stated rather than buried: shared memory is memory this kernel
 * cannot reclaim under pressure. 8 segments x 2 MiB is a 16 MiB ceiling on how
 * much of the machine a program can make unreclaimable, and that ceiling is
 * SHM_SEGMAX x SHM_PAGEMAX below rather than an accident.
 *
 * ===========================================================================
 * EAGER, NOT DEMAND-PAGED, AND THE FAULT PATH ALLOCATES NOTHING
 *
 * Every page's frame is allocated in shm_create(). Two reasons, in order of
 * importance:
 *
 *   1. vma.c's table is fixed and unallocated for a stated reason -- "it is
 *      consulted from the page-fault path, and an allocator call there would
 *      put kheap_lock underneath the fault handler on a path that kmalloc
 *      itself can reach". do_shm() is on that same path. With the frames
 *      already there, a shared fault is a table lookup, a pmm_ref and a PTE
 *      write, and it can no more fail for want of memory than a COW-reuse
 *      fault can.
 *
 *   2. A segment has an explicit size the caller named. Failing at
 *      shm_create() with "there is not that much memory" is a fact the caller
 *      can act on; failing three minutes later inside a fault kills the
 *      process at an address it has every right to touch. mmap()'s lazy
 *      reservation is right for a 64 MiB malloc arena that will be 200 KiB
 *      used, and wrong for a 2 MiB buffer that exists to be filled.
 *
 * The frames are zeroed at creation for the reason do_anon() zeroes: handing
 * over a frame with the previous owner's bytes in it is a disclosure bug.
 *
 * ===========================================================================
 * NAMED AND UNNAMED, ONE OBJECT
 *
 * A named segment (shm_open("/frames")) can be found by a process that is not
 * related to its creator, which is the case that has NO other answer on this
 * machine -- pipes need a common ancestor, and so would an anonymous shared
 * mapping inherited across fork. An unnamed segment (name == NULL) is the same
 * object with nothing to find it by, reachable only by inheriting the mapping
 * through fork; it is what libc's MAP_SHARED|MAP_ANONYMOUS is built on.
 *
 * They are one object because the only difference between them is whether a
 * string is in the table. Building the named one and synthesising the
 * anonymous one from it costs nothing; building them separately would be two
 * lifetimes to get right instead of one.
 *
 * PERMISSIONS ARE REAL AND ARE CHECKED HERE, not at the caller. shm_open()
 * takes the asking uid and enforces `mode` against it; a caller that already
 * knows the uid cannot forget the check, and the whole rule is testable on the
 * host without c/fs on the include path (mmsys.c supplies the uid from
 * vfs_cred_current(), which is the only kernel-only part). Root (uid 0)
 * bypasses, as it does in c/fs.
 *
 * WHAT IS NOT HERE: no resize (a segment's size is fixed at creation, so no
 * mapping can be outlived by a shrink), no msync (there is no backing store to
 * sync TO -- these pages ARE the storage), and no persistence across a reboot.
 *
 * ===========================================================================
 * LOCKING. shm_lock (irqsave) guards the table. Taken UNDER the BKL; it calls
 * only into pmm, so the order is BKL -> shm_lock -> pmm_lock and it never
 * nests with vma_lock (vma.c collects handles under its own lock and calls
 * shm_put() after releasing it, exactly as it already does for pcache). */

/* SIZE, DERIVED. The table is flat and static for vma.c's reason above, so
 * both numbers are BSS paid on every boot:
 *
 *     SHM_SEGMAX (8) x SHM_PAGEMAX (512) x 8 bytes = 32 KiB of frame slots,
 *     plus 8 x 64 bytes of identity                =  0.5 KiB
 *
 * against vma.c's already-paid 82 KiB, on a 512 MiB machine. Measured with
 * `nm -S build/kernel.elf | grep g_seg` rather than computed -- see the report.
 *
 * WHAT 2 MiB PER SEGMENT FORBIDS, said out loud because a limit nobody states
 * is a limit somebody discovers: a 640x480 RGBA frame is 1.17 MiB and fits; a
 * 1280x720 one is 3.52 MiB and does NOT. A consumer that needs to hand a
 * 720p frame across therefore needs SHM_PAGEMAX raised (and the BSS with it),
 * or a segment holding a smaller tile at a time. Raising it is one constant
 * and nothing else; it is not raised speculatively because 8 x 4 MiB of
 * permanently unreclaimable memory is a real thing to spend. */
#define SHM_SEGMAX   8
#define SHM_PAGEMAX  512
#define SHM_NAMEMAX  32

/* Errors. Negative, distinct, and never folded into one "it failed": a caller
 * that cannot tell "no such segment" from "not yours" cannot report either. */
#define SHM_E_INVAL  (-1)    /* bad name, zero/oversized length, bad handle */
#define SHM_E_NOENT  (-2)    /* no segment by that name, and SHM_CREAT not asked */
#define SHM_E_EXIST  (-3)    /* SHM_EXCL and it is already there */
#define SHM_E_ACCES  (-4)    /* the mode forbids this uid */
#define SHM_E_NOMEM  (-5)    /* no table slot, or not enough frames */

#define SHM_CREAT  0x1
#define SHM_EXCL   0x2
#define SHM_WRITE  0x4       /* the caller intends to write; needs the w bit */

/* Bring the table up. Idempotent; call before anything can map. */
void shm_init(void);

/* Open, and optionally create, a NAMED segment. `uid` is the asking identity
 * (mmsys.c passes vfs_cred_current()'s). `pages` and `mode` are used only when
 * creating. Returns a handle >= 0 with ONE reference taken, or a SHM_E_*.
 *
 * The reference is the caller's to put. mmsys.c holds it across the syscall
 * and hands it to the VMA (which takes its own), exactly as it does for a
 * pcache file handle. */
int shm_open(const char *name, unsigned pages, unsigned mode, unsigned flags,
             unsigned uid);

/* An UNNAMED segment: nothing can find it, so it is reachable only through the
 * mapping and whatever inherits that mapping across fork. One reference. */
int shm_create_anon(unsigned pages, unsigned uid);

/* Remove the NAME. The segment itself lives until its last reference goes, so
 * a mapping already made keeps working -- POSIX's rule, and the one that makes
 * "create, map, unlink immediately" a safe idiom. */
int shm_unlink(const char *name, unsigned uid);

/* Reference counting. The segment (and every frame it holds) is freed when the
 * last reference goes AND the name is gone. */
int  shm_ref(int sh);
void shm_put(int sh);

/* The frame behind page `index`, or 0. The caller must take its own pmm
 * reference before installing a PTE -- fault.c's do_shm() does, and the
 * comment there is the same one do_file() carries, for the same reason. */
uint64_t shm_frame(int sh, uint64_t index);

/* How many pages this segment has. 0 for a handle that is not live. */
uint64_t shm_pages(int sh);

/* --- accounting ------------------------------------------------------- */
int      shm_segs_live(void);
uint64_t shm_frames_held(void);      /* total pages across every live segment */
uint64_t shm_bugs(void);
void     shm_report(const char *tag);

#endif /* LOGIT_SHM_H */
