#ifndef EXECHOST_SPACE_H
#define EXECHOST_SPACE_H

#include <stdint.h>

/* A HOST ADDRESS SPACE FOR THE KERNEL LOADER.
 *
 * c/kernel/exec/elf.c is compiled unmodified into this test. It allocates
 * frames from pmm_alloc(), maps them with vmm_map_page(), reads them back with
 * vmm_pte(), and then -- this is the part a table of integers cannot stand in
 * for -- memcpy()s the file bytes to the USER VIRTUAL ADDRESS, because in the
 * kernel the target space is the one that is active.
 *
 * So this harness does not simulate a page table. It uses the real one:
 *   pmm_alloc()   -> mmap() one anonymous page, return its address as a frame
 *   vmm_map_page()-> mremap(MREMAP_FIXED) that page to the virtual address, then
 *                    mprotect() it to what the flags ask for
 *   vmm_pte()     -> a real entry out of a hash table this file keeps
 *
 * The user region (0x40000000..0x80000000) is free in a host process, so the
 * loader writes to exactly the addresses it would write to in the kernel and
 * the test can read them back and diff against the file. It also means the
 * PERMISSIONS are enforced by the same hardware: space_writable() asks the
 * kernel's own /proc-free way -- a write through a probe with the fault caught
 * -- so "text is not writable" is measured, not asserted from a flag word we
 * ourselves stored. */

void     space_reset(void);          /* unmap everything, reset the frame budget */
uint64_t space_frames_used(void);
void     space_set_budget(uint64_t frames);   /* pmm_alloc fails past this */
int      space_nx_enabled(void);
void     space_set_nx(int on);       /* what cpu_prot_nx_usable() answers      */

/* Pages mapped outside [lo, hi). The loader's central promise is that nothing
 * it maps ever lands outside the private user region, and the fuzz run asserts
 * it after every single iteration rather than only when something crashed. */
uint64_t space_pages_outside(uint64_t lo, uint64_t hi);
uint64_t space_pages_mapped(void);

/* The recorded PTE for `va`, or 0 if nothing is mapped there. */
uint64_t space_pte(uint64_t va);

/* Measured, not recorded: does a store to this address fault? 1 = writable. */
int      space_writable(uint64_t va);
/* Recorded: is the page marked no-execute (PTE bit 63)? */
int      space_nx(uint64_t va);

/* How many kprintf lines the loader emitted, and the last one. The loader's
 * contract is that a refusal always names the field, so a test that expects a
 * refusal also expects a line. */
int         space_msgs(void);
const char *space_last_msg(void);
void        space_msgs_reset(void);
void        space_quiet(int on);     /* stop echoing loader output to stdout   */

#endif
