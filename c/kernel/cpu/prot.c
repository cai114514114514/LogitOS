/* Ring-3 protection bits: the read side. See prot.h for why this reads EFER and
 * CR4 rather than CPUID.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS ON, WHAT IS OFF, AND WHY
 *
 * W^X, read-only half -- ON.
 *     c/kernel/exec/elf.c maps each PT_LOAD with the permissions the ELF
 *     actually asks for, instead of VMM_WRITABLE|VMM_USER for every segment.
 *     A program's text and rodata are now read-only to ring 3; before this,
 *     every executable page in every process was also writable, and a program
 *     could rewrite its own code. A PT_LOAD that asks for W and X at once is
 *     refused outright.
 *
 * NX, the no-execute half -- CPU ENABLED, KERNEL NOT USING IT. Be precise
 *     about which of those is which.
 *     EFER.NXE is set in c/boot/long.asm before any page table can carry bit
 *     63, and in c/boot/ap_trampoline.asm before an AP even enables paging, so
 *     every core would honour the bit today. Nothing sets it, because
 *     c/kernel/mm masks page-table entries back to frames with ~0xFFF, which
 *     keeps bit 63 and feeds the frame allocator an address 8 EiB up. See
 *     cpu_prot_nx_usable() in prot.h for the measurement and the fix. Until
 *     then a program's data, stack and heap remain executable.
 *
 * SMEP      -- ON (when the CPU has it).
 *     CR4.SMEP, set in the same two places. The kernel never intentionally
 *     executes a user page, so nothing had to be audited to turn it on: the
 *     set of code it constrains is empty by design, and the bit's value is
 *     that it stays empty. It was DETECTED by cpufeat.c and never set, which
 *     is the state this file exists to make visible.
 *
 * SMAP      -- OFF, deliberately. This is the honest part.
 *
 *     SMAP faults ring 0 on any read or write of a page whose effective U/S is
 *     user, unless the access is bracketed by STAC/CLAC. It is the right
 *     protection and it is not one that can be switched on: this kernel
 *     dereferences user pointers DIRECTLY -- not through user_copy_from() /
 *     user_copy_to() -- in a large number of places, and most of them are in
 *     files this line does not own:
 *
 *       c/kernel/gui/wm.c     the GUI syscall backend. Validates its pointers
 *                             with user_range_ok() and then memcpy()s through
 *                             them, or hands the raw pointer to fb/text code
 *                             that writes it. ~25 sites.
 *       c/net/core/sock.c     sock_send/sock_recv/sock_alpn take the user
 *                             buffer straight from the syscall frame.
 *       c/kernel/audio/snd.c  snd_syscall's PCM buffer, likewise.
 *       c/fs/vfs.c            vfs_read/vfs_write are handed the user buffer by
 *                             SYS_READ_FILE / SYS_WRITE_FILE and pass it down
 *                             to the filesystem, which memcpy()s into it.
 *       c/kernel/exec/       the ones this line does own are already routed
 *                             through usercopy.c, plus exec.c's setup_cli_stack
 *                             which writes the SysV stack directly.
 *
 *     Every one of those is CORRECT today -- the pointer is validated before
 *     it is used (see the audit in the report). SMAP would not find a bug in
 *     them; it would fault every one of them. Turning it on without first
 *     rewriting all of them to go through STAC/CLAC-bracketed accessors would
 *     not harden this kernel, it would stop it booting, and the version of
 *     that failure that actually matters is the one where the common paths
 *     were fixed and a rare one -- an error branch, a codec, a fault handler --
 *     was not, so the machine boots and dies later on a path no test covers.
 *
 *     The prerequisite is therefore not "set the bit". It is: make user_copy_*
 *     the ONLY way the kernel touches user memory, in every subsystem, and
 *     then set the bit so that the rule is enforced instead of maintained by
 *     hand. That is a cross-cutting change to five subsystems owned by four
 *     lines, and it is the next piece of work here, not this one.
 *
 *     The bit is left clear and this report says so out loud, on every boot,
 *     which is strictly better than the state it replaces: detected by
 *     cpufeat.c, never set, and nothing anywhere saying which.
 * ------------------------------------------------------------------------ */

#include <stdint.h>
#include "prot.h"
#include "cpufeat.h"
#include "kprintf.h"

#define EFER_MSR   0xC0000080u
#define EFER_NXE   (1ull << 11)
#define CR4_SMEP   (1ull << 20)
#define CR4_SMAP   (1ull << 21)

static inline uint64_t rd_efer(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(EFER_MSR));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rd_cr4(void)
{
    uint64_t v;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(v));
    return v;
}

int cpu_prot_nx(void)   { return (rd_efer() & EFER_NXE) ? 1 : 0; }

/* See the long comment in prot.h. The CPU is ready; c/kernel/mm is not, because
 * its PTE->frame masks keep bit 63. One symbol, so that when those masks are
 * fixed this becomes `return cpu_prot_nx();` and every user of NX in the kernel
 * turns on at once. */
int cpu_prot_nx_usable(void) { return 0; }
int cpu_prot_smep(void) { return (rd_cr4()  & CR4_SMEP) ? 1 : 0; }
int cpu_prot_smap(void) { return (rd_cr4()  & CR4_SMAP) ? 1 : 0; }

/* Printed as "supported/enabled" pairs rather than a single verdict, because
 * the two disagreeing is the interesting case in both directions: supported
 * but not enabled is the bug this work fixes, and enabled without support is
 * impossible and therefore worth being able to see. */
void cpu_prot_report(const char *who)
{
    /* nx is printed as three states, not two, because "the CPU will honour bit
     * 63" and "the kernel puts bit 63 in page tables" are different facts and
     * this machine is currently in between them. A single ON would be a lie in
     * the direction that matters. */
    kprintf("[prot] %s: nx %s/%s  wxsplit ON  smep %s/%s  smap %s/%s\n",
            who ? who : "cpu",
            cpu_has(CPU_NX) ? "sup" : "-",
            !cpu_prot_nx() ? "off" : (cpu_prot_nx_usable() ? "ON" : "efer-only"),
            cpu_has(CPU_SMEP) ? "sup" : "-", cpu_prot_smep() ? "ON"  : "off",
            cpu_has(CPU_SMAP) ? "sup" : "-", cpu_prot_smap() ? "ON"  : "off");
}
