#ifndef LOGIT_VMA_H
#define LOGIT_VMA_H

#include <stdint.h>

/* Virtual memory areas: what a range of user address space MEANS, as opposed
 * to what is currently mapped into it.
 *
 * Before this existed, "mapped" and "meant to exist" were the same thing --
 * memory was created by mapping it, so a process's only memory was the fixed
 * arena it linked with, and mini-libc's allocator could not grow because there
 * was nothing to grow into. A VMA is the record that lets a page be absent and
 * still be legitimate, which is what both mmap() and demand paging need.
 *
 * Deliberately a fixed table, not a kmalloc'd list: it is consulted from the
 * page-fault path, and an allocator call there would put kheap_lock underneath
 * the fault handler on a path that kmalloc itself can reach. 64 spaces x 16
 * areas x 32 bytes = 32 KiB of BSS, which buys "no allocation in the fault
 * path" outright. */

/* VMA_MAXAREA WAS 16 AND IS 32, AND THE NUMBER IS DERIVED, NOT ROUNDED UP.
 *
 * A thread stack used to cost ONE area. It now costs TWO: pthread_create mmaps
 * the region and then mprotects its lowest page to PROT_NONE, which splits the
 * area in two -- one guard, one stack. include/abi/logit_abi.h states the
 * ceiling that follows from this table ("around THIRTEEN concurrent threads per
 * process") as the number a caller will size its worker pool by, so holding
 * that ceiling while doubling the per-thread cost is what sets this:
 *
 *      13 threads x 2 areas          26
 *      the program's own stack        1
 *      libc's malloc arenas           2   (segregated free list; it grows)
 *      the loader's file mappings     2   (text and rodata, elf.c)
 *                                  ----
 *                                    31   -> 32
 *
 * Cost, measured rather than estimated -- `nm -S build/kernel.elf | grep spaces`
 * reads 0x14200 = 82,432 B of BSS against 41,472 B at VMA_MAXAREA 16, so
 * exactly +40,960 B on a 512 MiB machine (sizeof(struct vma) is 40, and
 * 64 x (8 + 32 x 40) = 82,432 checks out against the symbol). Paid because the
 * alternative is a guard page that halves how many threads a process may have,
 * which is a worse trade than 40 KiB by a wide margin. */
#define VMA_MAXSPACE 64
#define VMA_MAXAREA  32

#define VMA_READ   0x1
#define VMA_WRITE  0x2
#define VMA_EXEC   0x4

/* FILE BACKING. `file` is a c/kernel/mm/pcache.h handle -- a reference to the
 * INODE, not to the open file description the mmap came through, which is why
 * the fd may be closed the instant after mmap() returns and why two processes
 * mapping the same file land on the same pages. `foff` is the byte offset in
 * that file of `start`, so page (va - start)/4096 + foff/4096 is the file page
 * index and the fault path needs nothing else.
 *
 * The reference is owned by the AREA, which is what makes the lifetime rules
 * fall out rather than having to be remembered:
 *   fork    vma_space_clone copies the areas, so it takes a reference each;
 *   execve  vma_space_clear drops every area, so it puts each one;
 *   exit    vma_space_free the same;
 *   munmap  vma_release puts the areas it removes, and an area that is SPLIT
 *           becomes two areas and therefore takes one more.
 *
 * `file` is -1 on an anonymous area, and every place a slot is created or
 * cleared sets it, because a stale handle in an unused slot would be put twice
 * the next time that slot is used. */
/* SHARED-SEGMENT BACKING. `shm` is a c/kernel/mm/shm.h handle, and it is the
 * SECOND kind of backing an area can have. It obeys the identical lifetime
 * table above -- clone takes a reference, clear/free/release put one, a split
 * takes one more -- because it is the identical shape of problem, and the two
 * are maintained side by side in every one of those five places for exactly
 * that reason.
 *
 * `foff` IS SHARED BETWEEN THE TWO and means "byte offset of `start` in
 * whatever backs this area". That is not an economy, it is what makes the split
 * and trim arithmetic in vma_release() correct for segments WITHOUT A LINE
 * CHANGED: front-trimming an area has to advance the backing offset by the same
 * amount whether the bytes come from a file or from a segment, and having
 * written that once there is no second copy of it to get wrong.
 *
 * `file` and `shm` are MUTUALLY EXCLUSIVE -- at most one is >= 0. An area is
 * anonymous (both -1), file-backed, or segment-backed, and nothing constructs
 * one that is two of those. Both are set to -1 by slot_clear(), for the reason
 * `file` alone already needed it: a stale handle in an unused slot is a
 * reference that gets put a second time the next time that slot is handed
 * out. */
struct vma {
    uint64_t start, end;      /* [start, end), page aligned */
    uint64_t foff;            /* byte offset of `start` in the backing object */
    int32_t  file;            /* pcache file handle, or -1 = not file-backed */
    int32_t  shm;             /* shm segment handle, or -1 = not segment-backed */
    uint32_t prot;            /* VMA_* */
    uint32_t used;
};

/* Address-space lifetime. Mirrors vmm_new_space / vmm_free_space, which are
 * the only two places an address space is born and dies. */
void vma_space_new(uint64_t cr3);
void vma_space_free(uint64_t cr3);
void vma_space_clear(uint64_t cr3);              /* execve: drop every area, keep the space */
int  vma_space_clone(uint64_t dst_cr3, uint64_t src_cr3);   /* fork */

/* Look up the area covering `va`. Returns its prot, or 0 if none. */
uint32_t vma_prot_at(uint64_t cr3, uint64_t va);

/* The file backing of the area covering `va`, if it has any. Returns 1 and
 * fills *file / *index (the FILE PAGE INDEX, not a byte offset) / *prot, or 0
 * for an anonymous area or no area at all. One call rather than three, because
 * the page-fault path asks all of it at once and a second scan of the table
 * under the same lock is the only other way to get it consistently. */
int vma_file_at(uint64_t cr3, uint64_t va, int *file, uint64_t *index, uint32_t *prot);

/* The same question for a SHARED SEGMENT: returns 1 and fills *shm / *index
 * (the PAGE INDEX within the segment) / *prot, or 0 for any other kind of area.
 * A separate call rather than an out-parameter on vma_file_at(), because a
 * caller that asks "is this file-backed" and gets back "no, but it is segment-
 * backed" through a field it did not read is a caller that faults the page the
 * wrong way. The two are asked one after the other in fault.c and at most one
 * of them can answer. */
int vma_shm_at(uint64_t cr3, uint64_t va, int *shm, uint64_t *index, uint32_t *prot);

/* Reserve `len` bytes backed by shm segment handle `sh` from byte offset
 * `off`. Takes its own reference on `sh`, so the caller still owns the one it
 * came in with -- the identical contract vma_reserve_file() states, and for the
 * identical reason. Returns the base address, or 0. */
uint64_t vma_reserve_shm(uint64_t cr3, uint64_t hint, uint64_t len, uint32_t prot,
                         int sh, uint64_t off);

/* Reserve `len` bytes backed by pcache file handle `fh` from byte offset
 * `foff`. Takes its own reference on `fh`, so the caller still owns the one it
 * came in with. Returns the base address, or 0. */
uint64_t vma_reserve_file(uint64_t cr3, uint64_t hint, uint64_t len, uint32_t prot,
                          int fh, uint64_t foff);

/* Reserve `len` bytes. `hint` is a preferred base (0 = anywhere). Returns the
 * base address, or 0 on failure. Nothing is mapped: the pages materialise on
 * first touch. */
uint64_t vma_reserve(uint64_t cr3, uint64_t hint, uint64_t len, uint32_t prot);

/* Reserve an EXACT range, anywhere in the private user region rather than only
 * inside the mmap window. For the kernel's own placements -- a program's
 * initial stack, which exec puts at an address derived from the image's link
 * base and which is nowhere near MM_MMAP_BASE. Refuses to overlap an existing
 * area, so it still cannot be used to take memory away from anything; that
 * refusal, and not the address window, is what makes reservation safe. Returns
 * 0, or -1 (bad range / no free slot / already occupied). */
int vma_reserve_fixed(uint64_t cr3, uint64_t start, uint64_t len, uint32_t prot);

/* An EXACT range backed by pcache file handle `fh` from byte offset `foff`.
 * The kernel-internal counterpart of vma_reserve_file(), for c/kernel/exec's
 * ELF loader: a program's text is at the address it was LINKED at, which is
 * nowhere near the mmap window vma_reserve() places into. Takes its own
 * reference on `fh`; the caller keeps the one it came in with. Refuses
 * VMA_WRITE, an unaligned `foff`, a range that would be rounded, a range
 * outside the private user region, and any overlap. Returns 0 or -1; a -1 is
 * a reason to copy the pages, never a reason to fail a load. */
int vma_reserve_file_fixed(uint64_t cr3, uint64_t start, uint64_t len,
                           uint32_t prot, int fh, uint64_t foff);

/* Release [addr, addr+len). Returns 0, or -1 if the range is not a subset of
 * reserved space. Splits an area when the range is punched out of its middle. */
int vma_release(uint64_t cr3, uint64_t addr, uint64_t len);

/* mprotect: give [addr, addr+len) the protection `prot`, splitting areas at
 * the range's edges so the change lands on exactly that range and no more.
 *
 * `prot` MAY BE 0, and that is the point of the call -- see logit_abi.h's
 * SYS_MPROTECT. Everywhere else in this file a prot of 0 is floored to
 * VMA_READ, because a reservation nobody may touch is indistinguishable from
 * no reservation; here the two are exactly what have to be distinguished, and
 * `vma_prot_at` returning 0 for "PROT_NONE" and for "no area at all" is
 * harmless because the fault path's answer to both is the same word: refuse.
 *
 * ALL OR NOTHING. Returns:
 *    0   the whole range now has `prot`
 *   -1   VMA_E_RANGE  bad range (zero/wrapping/outside the user region)
 *   -2   VMA_E_NOMEM  part of the range is not reserved, or the splits it
 *                     needs have no free slot. NOTHING IS CHANGED -- both are
 *                     decided before a single area is touched, for the reason
 *                     vma_release states about its own split: a call that
 *                     fails halfway leaves an address space no caller can
 *                     reason about.
 *   -3   VMA_E_ACCES  VMA_WRITE over a file-backed area. Refused here and not
 *                     only in mmsys.c, so the "a file mapping is never
 *                     writable" invariant is a property of the mechanism
 *                     rather than of the callers that remember it -- exactly
 *                     the argument vma_reserve_file_fixed makes. */
#define VMA_E_RANGE (-1)
#define VMA_E_NOMEM (-2)
#define VMA_E_ACCES (-3)
int vma_protect(uint64_t cr3, uint64_t addr, uint64_t len, uint32_t prot);

/* THE WHOLE TABLE, copied out, ascending by `start`. Returns how many areas
 * exist, which may exceed `max` (in which case the `max` lowest are written).
 * The only enumerator in this header -- everything else answers about one
 * address -- and it exists because c/kernel/exec/coredump.c has to describe a
 * dying process's entire address space. See the definition in vma.c for why it
 * copies rather than lending a pointer into the locked table. */
int      vma_snapshot(uint64_t cr3, struct vma *out, int max);

/* Accounting. */
uint64_t vma_reserved_bytes(uint64_t cr3);
int      vma_count(uint64_t cr3);
int      vma_spaces_live(void);

/* Enumeration: the i'th LIVE area of `cr3`, copied out. 1 = filled, 0 = there
 * is no i'th area. `i` counts live areas, not slots -- see the long comment on
 * the definition for why that distinction is load-bearing, and why this
 * returns a copy rather than a pointer. Added for /proc/<pid>/maps. */
int      vma_nth(uint64_t cr3, int i, struct vma *out);

/* Page-aligned range arithmetic, shared with mmap and munmap. Returns 0 on
 * success and fills `*out_start` / `*out_end` with the rounded-out range;
 * returns -1 for a zero length, an overflowing length, or a range that leaves
 * the user region. Exported because getting this wrong is how an mmap ABI
 * turns into an arbitrary-write primitive. */
int vma_range(uint64_t addr, uint64_t len, uint64_t *out_start, uint64_t *out_end);

#endif /* LOGIT_VMA_H */
