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

#define VMA_MAXSPACE 64
#define VMA_MAXAREA  16

#define VMA_READ   0x1
#define VMA_WRITE  0x2
#define VMA_EXEC   0x4

struct vma {
    uint64_t start, end;      /* [start, end), page aligned */
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

/* Release [addr, addr+len). Returns 0, or -1 if the range is not a subset of
 * reserved space. Splits an area when the range is punched out of its middle. */
int vma_release(uint64_t cr3, uint64_t addr, uint64_t len);

/* Accounting. */
uint64_t vma_reserved_bytes(uint64_t cr3);
int      vma_count(uint64_t cr3);
int      vma_spaces_live(void);

/* Page-aligned range arithmetic, shared with mmap and munmap. Returns 0 on
 * success and fills `*out_start` / `*out_end` with the rounded-out range;
 * returns -1 for a zero length, an overflowing length, or a range that leaves
 * the user region. Exported because getting this wrong is how an mmap ABI
 * turns into an arbitrary-write primitive. */
int vma_range(uint64_t addr, uint64_t len, uint64_t *out_start, uint64_t *out_end);

#endif /* LOGIT_VMA_H */
