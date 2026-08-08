#ifndef LOGIT_PROT_H
#define LOGIT_PROT_H

/* The ring-3 protection bits: what is actually enabled on this machine.
 *
 * The bits themselves are set in assembly (c/boot/long.asm for the BSP,
 * c/boot/ap_trampoline.asm for every AP) because EFER.NXE has to be on before
 * a page table can carry bit 63 -- see the comments there. This file is the
 * read side, and it deliberately READS THE REGISTERS BACK rather than
 * re-deriving the answer from CPUID.
 *
 * That distinction is the whole point of the file. "The CPU supports NX" and
 * "NX is enabled on the core you are running on" are different claims, and the
 * gap between them is exactly the failure this work exists to close: SMEP and
 * SMAP were detected by c/kernel/cpu/cpufeat.c for a long time and set in CR4
 * never, which reads from outside as handled. A predicate that answers from
 * CPUID would have reported both as present that entire time. These answer
 * from EFER and CR4.
 */

/* Bit 63 of a leaf page-table entry: no-execute.
 *
 * Defined here, beside the predicate that says whether it is legal to set it,
 * rather than in c/kernel/mm/vmm.h with the other VMM_* flags. Two reasons, one
 * practical and one not: c/kernel/mm is owned by another line right now, and --
 * more to the point -- this bit is meaningless without EFER.NXE. Anywhere it is
 * used, cpu_prot_nx() has to be used too, so the two live together and a caller
 * cannot reach for one without seeing the other.
 *
 * vmm_map_page()'s `flags` argument is a uint64_t OR'd straight into the entry,
 * so passing this needs no change to the mapping code. Only LEAF entries ever
 * get it: effective execute permission is the OR of the NX bits along the whole
 * walk, so an NX bit on a page-directory entry would make an entire 2 MiB (or
 * 1 GiB) subtree non-executable, including whatever else shares it. */
#define PTE_NX  (1ull << 63)

/* 1 if EFER.NXE is set on the current core -- i.e. bit 63 of a page-table
 * entry means no-execute and not "reserved bit, fault the process". Every
 * caller that is about to set bit 63 must ask this first. */
int cpu_prot_nx(void);

/* 1 if it is SAFE to put PTE_NX in a leaf entry of a USER page table.
 *
 * This is NOT the same question as cpu_prot_nx(), and the difference is the
 * whole reason the function exists. EFER.NXE being on says the CPU will honour
 * bit 63. It says nothing about whether the KERNEL can survive its own page
 * tables carrying it -- and for a long time it could not.
 *
 * WHAT WAS WRONG, kept because it is the best available description of how a
 * protection bit can be fatal three subsystems away. Every place c/kernel/mm
 * converted a page-table entry back into a physical frame did it with
 *
 *     frame = e & ~(uint64_t)0xFFF;
 *
 * which clears the low 12 flag bits and KEEPS bit 63. The architectural
 * address field is bits 12..51, so the correct mask is 0x000FFFFFFFFFF000.
 * With NX set, that expression yields 0x8000000000nnn000 -- a physical address
 * eight exabytes up -- and handed it to pmm_ref() (vmm.c, the fork path),
 * pmm_free() (vmm.c, unmap and teardown) and pmm_refcount() (fault.c, the
 * copy-on-write path). The frame refcount table is indexed by frame number, so
 * that was an out-of-bounds access on every fork, every COW resolution and
 * every process exit.
 *
 * It was not theoretical. Setting NX with those masks in place booted the
 * desktop, launched Finder, Clock and /bin/sh, and then killed the machine
 * with a kernel #GP inside memcpy the first time the shell fork+exec'd a
 * command -- the corrupted refcount table having produced a non-canonical
 * pointer several operations later.
 *
 * WHAT FIXED IT: MM_PTE_ADDR in c/kernel/mm/mm.h, at all 48 sites, each read
 * and classified rather than swept (about fifteen other `& ~0xFFF` in those
 * files page-align a VIRTUAL address, where the old mask is right). Two
 * further sites did the opposite -- the fork clone and the copy-on-write copy
 * rebuilt an entry's permissions from `e & 0xFFF`, which drops bit 63, so NX
 * would have quietly come undone on the first write to any forked page rather
 * than corrupting anything. Those carry MM_PTE_FLAGS now. And the swap entry,
 * which keeps NX in bit 63 by design, read its slot number as `e >> 12` with
 * no mask; the slot field is now stated to be 40 bits wide and masked at both
 * ends.
 *
 * So this now returns cpu_prot_nx(). Deliberately still a function and not an
 * #ifdef: the condition is a property of another subsystem, and it should be
 * possible to flip it in one place and have the whole kernel agree -- which is
 * also the fastest way to bisect a machine that misbehaves after this. */
int cpu_prot_nx_usable(void);

/* 1 if CR4.SMEP is set on the current core: ring 0 cannot execute a page whose
 * effective U/S is user. */
int cpu_prot_smep(void);

/* 1 if CR4.SMAP is set. Expected to be 0 -- see the audit in prot.c for what
 * would have to change first, and why turning it on today would convert a
 * large number of silent user-pointer dereferences into faults on rare paths
 * rather than into a diagnosis. */
int cpu_prot_smap(void);

/* Print one line naming, for THIS core, which protections are supported by the
 * CPU and which are actually on. Called once on the BSP after serial is up,
 * and once per AP, because CR4 is per-core state and an AP that missed the
 * trampoline's CR4 write would otherwise be indistinguishable from one that
 * did not. */
void cpu_prot_report(const char *who);

#endif /* LOGIT_PROT_H */
