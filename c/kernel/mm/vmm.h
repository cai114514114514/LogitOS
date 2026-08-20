#ifndef LOGIT_VMM_H
#define LOGIT_VMM_H

#include <stdint.h>

#define VMM_WRITABLE 0x2
#define VMM_USER     0x4
#define VMM_NOCACHE  (0x8 | 0x10)   /* PWT | PCD */

/* ------------------------------------------------------------- PTE BITS --
 * Bits 9-11 of a PTE are ignored by the CPU and reserved for the OS. This is
 * the whole map of what c/kernel/mm/ uses, written in one place because a page
 * lifetime line and an NX/permissions line editing the same 64 bits from
 * different files is how a PTE flag gets used twice:
 *
 *   bit  1   W        hardware: writable
 *   bit  2   U        hardware: user-accessible
 *   bit  5   A        hardware: set by the CPU on any reference   (reclaim reads)
 *   bit  6   D        hardware: set by the CPU on a write         (reclaim reads)
 *   bit  9   COW      OS: writable-but-shared, resolve on write   (mm owns)
 *   bit 10   ANON     OS: this page was zero-filled on demand and can be
 *                     re-derived by faulting it again             (mm owns)
 *   bit 11   FILE     OS: this page is a PAGE-CACHE page: its contents came
 *                     from a file and can be re-derived by re-reading it
 *   bit 52   SHM      OS: this page belongs to a SHARED segment and must NOT
 *                     be privatised by fork                       (mm owns)
 *   bit 53   NOACCESS OS: not present, but the frame in it is still LIVE and
 *                     still referenced -- mprotect(PROT_NONE) over a resident
 *                     page                                      (mm owns)
 *   bits 54-62        OS: FREE
 *   bit 63   NX       hardware: no-execute (the user/kernel-separation line)
 *
 * mm claims 9, 10, 11, 52 and 53, and nothing else. 52 and 53 rather than more
 * bits down in 9-11 because there are no more bits down there; 52-62 are
 * ignored by the CPU in a leaf entry exactly as 9-11 are, and MM_PTE_FLAGS
 * (mm.h) already carries everything above 51 across a fork and a copy-on-write
 * copy, which is the one property these bits need from the rest of the tree.
 *
 * THE TWO COLLIDED, and it is worth recording because the collision was CAUSED
 * BY A DATA LOSS rather than by anybody choosing badly. NOACCESS took 52
 * first; an agent's `git stash` then wiped every tracked modification in the
 * tree (recovered on branch rescue-1845); SHM was written afterwards against a
 * header where 52 read as free, and claimed it too. They are NOT mutually
 * exclusive -- a shared page can be mprotect(PROT_NONE)'d -- so aliasing them
 * would be silently wrong rather than merely tight. Syscall numbers were
 * pre-assigned across the parallel work that produced these two; PTE BITS WERE
 * NOT, and that is the lesson. */
#define VMM_PTE_COW  (1ull << 9)

/* Zero-fill-on-demand origin. Set by the anonymous fault, carried through fork
 * (the clone copies PTE flags wholesale) and through a copy-on-write copy.
 * Reclaim's cheapest tier needs it: a page may only be dropped and re-derived
 * for free if the code that re-derives it produces the same bytes, and only an
 * anonymous page's re-derivation (zero fill) is known to. An ELF text page's
 * contents came from a file that nothing re-reads, so it must be swapped. */
#define VMM_PTE_ANON (1ull << 10)

/* File-backed origin: this page belongs to c/kernel/mm/pcache.h and its bytes
 * are on a filesystem. The SECOND thing reclaim's cheap tier can re-derive, and
 * the one the design note in reclaim.h says had no producer here -- see the top
 * of pcache.h.
 *
 * It is not, on its own, permission to drop the page. The authority is the
 * cache: pcache_holds(frame) says the entry is still there to re-read from,
 * and it is the cache's reference on the frame that reclaim has to account for.
 * The bit exists because a PTE has to be able to SAY what it is without a hash
 * lookup -- fork copies PTE flags wholesale, so the child's mapping arrives
 * already marked, and the audit can tell a file page from an anonymous one in
 * the tables themselves.
 *
 * ANON and FILE are mutually exclusive by construction: do_anon() sets one,
 * do_file() sets the other, and nothing sets both. A page with neither came
 * from elf_load and can only be swapped, which is the pre-existing behaviour
 * and stays. */
#define VMM_PTE_FILE (1ull << 11)

/* SHARED-SEGMENT origin: this page belongs to a c/kernel/mm/shm.h segment, and
 * the frame under it is shared with every other process mapping that segment.
 *
 * IT IS THE ONE PTE BIT THAT CHANGES WHAT FORK DOES, and that is its entire
 * reason to exist. vmm_clone_user walks PTEs, not VMAs -- so without a mark in
 * the entry itself, telling a shared page from a private one would mean a VMA
 * table scan per page across a 512x512 walk, under vma_lock, in the middle of
 * fork. With the mark, the clone's shared branch is three lines and the
 * property is local to the entry it is about.
 *
 * A shared page is the ONLY page in this kernel that a fork must hand to the
 * child STILL WRITABLE and NOT marked copy-on-write. Every other writable page
 * goes read-only + COW so the first write privatises it; doing that to a shared
 * page is precisely the bug -- the two processes each get their own copy on
 * their first write and stop communicating, with no error anywhere and nothing
 * in either address space that looks wrong. `-DSHM_FORK_COPY` is that mistake
 * on a switch (vmm.c) and the gate is required to fail against it.
 *
 * MUTUALLY EXCLUSIVE WITH ANON AND FILE, by construction and of necessity, not
 * merely by tidiness: both of those bits are a PROMISE TO RECLAIM that the
 * page's contents can be re-derived by faulting it again, and for a shared page
 * both re-derivations produce a PRIVATE frame (do_anon allocates a fresh one;
 * a swap-in reads into a fresh one). A shared page marked ANON would be
 * eligible for the drop tier the moment it happened to be all zeroes, which is
 * exactly what a freshly created segment looks like. shm.h argues this at
 * length; do_shm() sets this bit and neither of the others. */
#define VMM_PTE_SHM  (1ull << 52)

/* ------------------------------------------------------- PROT_NONE -------
 * A page that is RESIDENT and that nothing may touch: mprotect(PROT_NONE) over
 * a range whose pages have already been faulted in. The frame is still ours,
 * its contents are still the process's, and a later mprotect back to readable
 * must give those bytes back -- so the frame cannot be freed and cannot be
 * forgotten, but no user access to it may succeed.
 *
 * THE HARDWARE OFFERS NO ENCODING FOR THAT, which is why this bit exists.
 * With P=1 there is no combination of W and U that refuses a READ from ring 3
 * (U=0 refuses it, but then the entry is indistinguishable from a kernel page
 * mapped inside the user subtree, and every teardown loop in vmm.c uses
 * exactly `(PRESENT|USER)` to decide what to free). So the entry is made NOT
 * PRESENT -- which is what makes the access fault -- and this bit says the
 * frame address in it is still live and still referenced.
 *
 * Bit 52 rather than 9/10/11: those three are taken (COW/ANON/FILE) and the
 * map above records 52-62 as free. It sits ABOVE the frame field, so
 * MM_PTE_ADDR still extracts the frame and vmm_clone_user's "install the entry
 * whole" carries it across a fork untouched.
 *
 * IT IS NOT A SWAP ENTRY AND CANNOT BE MISTAKEN FOR ONE: vmm_pte_is_swap()
 * requires bit 1, and a PROT_NONE page is never writable, so bit 1 is always
 * clear here. The classifier sees P=0, no swap marker, and a VMA whose prot is
 * 0, and returns MM_FAULT_NONE -- a fault at exactly the faulting address,
 * which is the whole point of a guard page.
 *
 * THE COST, stated rather than discovered: a PROT_NONE resident page has no
 * rmap entry (rmap tracks PRESENT|USER PTEs only), so rmap_count is 0 while
 * pmm_refcount is 1, and reclaim's "evict only if they are equal" rule makes
 * it structurally un-evictable -- the same way a page-table frame is. That is
 * the safe direction (rmap_audit permits FEWER mappings than references and
 * says so) and it is bounded by how much a process chooses to make PROT_NONE,
 * which for the only caller today is one page per thread. */
#define VMM_PTE_NOACCESS (1ull << 53)

static inline int vmm_pte_is_noaccess(uint64_t e)
{ return !(e & 1) && (e & VMM_PTE_NOACCESS) != 0; }

/* The hardware reference bits, named. Reclaim samples A to find pages nobody is
 * using, and reads D to know whether a page has been modified. */
#define VMM_PTE_ACCESSED (1ull << 5)
#define VMM_PTE_DIRTY    (1ull << 6)

/* ------------------------------------------------------------ SWAP ENTRY --
 * A page that has been written to the swap device leaves its PTE NOT PRESENT,
 * with the slot number in it. With P=0 the CPU ignores every other bit, so the
 * whole entry is ours -- but bit 0 must stay clear and the entry must be
 * distinguishable from a plain zero PTE (an address that was never mapped), so
 * bit 1 is the marker and slot numbers start at 1.
 *
 *   bit  0    0        not present
 *   bit  1    1        SWAP marker
 *   bit  2    saved W        )  the permissions the page had before it left,
 *   bit  3    saved COW      )  so faulting it back in restores them. They
 *   bit  4    saved ANON     )  cannot stay in their normal positions: bit 1
 *                              (W) is the marker and bit 2 (U) would collide.
 *   bits 12-51 slot number   (40 bits: the same span a real PTE's address
 *                             field occupies, which is what makes bit 63 free
 *                             to keep meaning NX)
 *   bit  63   NX       left exactly where it is, so it survives untouched
 *
 * THE SLOT FIELD IS BOUNDED, and it has to be. This used to read the slot as
 * `e >> 12` with no mask, which is only correct while every bit above the slot
 * is zero. It is not: the line directly above deliberately keeps NX in bit 63,
 * so the moment a no-execute page was swapped out, its slot number came back
 * with bit 51 set -- 2^51 -- and swap.c indexed its slot table with it. Found
 * by tests/unit/mm_reclaim_test.c the first time an anonymous page was mapped
 * NX, as an out-of-bounds pointer inside the simulated device. Two bits of one
 * PTE meaning two things is fine; reading one of them without saying where it
 * ends is not. */
#define VMM_PTE_SWAP     (1ull << 1)
#define VMM_SWAP_W       (1ull << 2)
#define VMM_SWAP_COW     (1ull << 3)
#define VMM_SWAP_ANON    (1ull << 4)
#define VMM_SWAP_SHIFT   12
#define VMM_SWAP_BITS    40
#define VMM_SWAP_MAX     ((1ull << VMM_SWAP_BITS) - 1)

static inline int vmm_pte_is_swap(uint64_t e)
{ return !(e & 1) && (e & VMM_PTE_SWAP) != 0; }
static inline uint64_t vmm_pte_swap_slot(uint64_t e)
{ return (e >> VMM_SWAP_SHIFT) & VMM_SWAP_MAX; }

/* Map one 4 KiB page (`virt` -> `phys`) into the active address space,
 * allocating intermediate page tables from the PMM as needed. PRESENT is
 * always set; pass extra flags such as VMM_WRITABLE. */
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Map a contiguous range (rounded out to page boundaries). */
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

/* --- per-process address spaces --- */

/* Physical address of the kernel's boot PML4 (the shared address space). */
uint64_t vmm_kernel_cr3(void);

/* Build a new address space: a fresh PML4 that shares the kernel's top-level
 * entries (identity-mapped low memory, framebuffer MMIO) but gets its own
 * private user region. Returns the new PML4 physical address, or 0 on failure. */
uint64_t vmm_new_space(void);

/* Map one 4 KiB page into the address space rooted at PML4 physical `cr3`
 * (rather than the active one), so an app's space can be populated before the
 * switch. The private user PDPT is copied-on-first-use off the shared kernel
 * PDPT so kernel/framebuffer stay mapped. */
void vmm_map_page_in(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);

/* Install a complete, NOT-present PTE (i.e. a swap entry) in `cr3`. mm-internal:
 * the fork clone uses it to give a child its parent's swapped-out pages. */
void vmm_map_raw_in(uint64_t cr3, uint64_t virt, uint64_t entry);

/* Validate that a user buffer is mapped in `cr3` for read/write access.
 * PURE CHECK: it never changes a mapping, so a copy-on-write page reads as
 * NOT writable. Callers that are about to write must use
 * vmm_user_range_fault_in() instead -- see usercopy.c. */
int vmm_user_range_ok(uint64_t cr3, const void *ptr, uint64_t len, int write);

/* Same range check, but RESOLVES anything that is legitimately missing first:
 * a copy-on-write page is unshared, an untouched anonymous (mmap) page is
 * filled. After this returns 1 the whole range is present, and writable if
 * `write`, so the caller's plain memcpy cannot fault. Returns 0 if the range is
 * not valid for the process, in which case nothing was changed. */
int vmm_user_range_fault_in(uint64_t cr3, const void *ptr, uint64_t len, int write);

/* Locate the leaf PTE for `virt` in `cr3` WITHOUT allocating page tables.
 * Returns a pointer to the entry, or NULL if any level is absent or is a large
 * page. mm-internal (fault.c), exported only because it lives in vmm.c. */
uint64_t *vmm_pte(uint64_t cr3, uint64_t virt);

/* fork(): clone the private user subtree of `src_cr3` into `dst_cr3`. With
 * copy-on-write enabled (mm.h) no data page is copied: both spaces are pointed
 * at the same frames, read-only + VMM_PTE_COW, and the frames' refcounts go up.
 * Returns 0 on success, -1 on OOM (caller must vmm_free_space(dst) + fail). */
int vmm_clone_user(uint64_t dst_cr3, uint64_t src_cr3);

/* Pages shared / copied by the most recent vmm_clone_user, for the fork
 * accounting. Reset at the start of each clone. */
void vmm_clone_stats(uint64_t *shared, uint64_t *copied);

/* Unmap [virt, virt+len) in `cr3`, dropping a reference on each frame that was
 * present. Returns the number of pages actually unmapped. */
uint64_t vmm_unmap_range_in(uint64_t cr3, uint64_t virt, uint64_t len);

/* Re-apply VMA_* protection `prot` to the pages of [virt, virt+len) that are
 * ALREADY RESIDENT in `cr3`. The other half of mprotect: vma_protect() changes
 * what the range MEANS (and so what a future fault does), this changes what
 * the pages already there permit. Absent, swapped and PROT_NONE pages are all
 * handled; nothing is allocated and no frame is freed, so a partial range
 * cannot leave the space inconsistent. Returns the number of PTEs changed.
 *
 * COPY-ON-WRITE IS RE-DERIVED HERE RATHER THAN CARRIED, and that is the one
 * subtle thing in it -- see the comment on the function. */
uint64_t vmm_protect_range_in(uint64_t cr3, uint64_t virt, uint64_t len, uint32_t prot);

/* execve(): free the user subtree but keep the (empty) address space alive. */
void vmm_free_user(uint64_t cr3);

/* exit(): tear down an entire address space made by vmm_new_space (not the active CR3). */
void vmm_free_space(uint64_t cr3);

/* Load CR3 (switch the active address space). Also records which space this
 * core is running, for vmm_space_busy_elsewhere() below. */
void vmm_switch(uint64_t cr3);

/* Is `cr3` loaded on some OTHER core right now (or was, until a CR3 write that
 * has not completed)? Reclaim must not unmap a page belonging to a space
 * another core is executing: it cannot shoot down that core's TLB (tlb.h --
 * doing so from under the BKL deadlocks), so it leaves those spaces alone
 * instead. See the long comment above vmm_switch() for why the answer is
 * maintained here and why the update order is what it is. */
int vmm_space_busy_elsewhere(uint64_t cr3);

#endif /* LOGIT_VMM_H */
