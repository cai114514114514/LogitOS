#ifndef LOGIT_COREDUMP_H
#define LOGIT_COREDUMP_H

#include <stdint.h>

/* ===========================================================================
 * CORE DUMPS -- what a ring-3 program leaves behind when it dies.
 *
 * WHAT WAS HERE BEFORE. Nothing. A faulting app printed one line on the serial
 * port ("[fault] app exception: page fault (vector 14) rip=... err=... cr2=...")
 * and was gone; `make debug` attaches gdb to QEMU, which debugs the KERNEL, so
 * the only program on this machine that could ever be examined after a crash
 * was the one that is not the one that crashed.
 *
 * -------------------------------------------------------------------- FORMAT
 * ELF64 ET_CORE. NOT an own format, and the reason is not tidiness:
 *
 *   - the host has gdb, readelf and objdump (`which gdb readelf objdump` ->
 *     /usr/bin/{gdb,readelf,objdump}). An ELF core is therefore checkable by
 *     three tools that did not come from this tree, which is the strongest
 *     oracle available for a file format and is exactly what the differential
 *     discipline everywhere else in this repository asks for. An own format
 *     would be checkable only by the reader shipped beside it -- i.e. by
 *     something that agrees with the writer by construction.
 *   - c/kernel/exec/elf.h already carries the ELF64 structures, so the header
 *     and program-header emission is arithmetic, not a port.
 *   - PT_LOAD's p_filesz < p_memsz is ALREADY the format's own way of saying
 *     "this region is larger than what is in the file". The size cap below is
 *     the central problem of dumping on a 64 MiB filesystem, and ELF core
 *     represents its outcome natively. An own format would have to invent that
 *     representation and then teach a reader to believe it.
 *
 * The one thing that would have made it half-done is NT_PRSTATUS: a register
 * note whose layout is nearly right produces a file gdb opens and then lies
 * about. So the layout is not remembered, it is DIFFED -- tests/unit/
 * coredump_test.c asserts every field offset of `struct core_prstatus` below
 * against glibc's own <sys/procfs.h> `struct elf_prstatus`, and then hands the
 * finished file to gdb and requires gdb to read back the registers this kernel
 * put in. If that check ever fails the format claim fails with it.
 *
 * ---------------------------------------------------------------- WHAT FITS
 * THE CONSTRAINT IS REAL AND IT IS NOT DISK SPACE. c/fs's image is 64 MiB with
 * about 6,000 free blocks, which would hold a 24 MiB dump; tools/mkfs.py's
 * INODE_COUNT is 256 with ~230 used, which would not hold many files. And
 * LogitFS rewrites a WHOLE FILE per write (vfs_write takes one contiguous
 * buffer), so a dump must exist in kernel memory in one piece before any of it
 * reaches the disk. Three separate ceilings, and the buffer is the lowest.
 *
 * So: CORE_BUF_MAX bytes, in .bss, not kmalloc. Allocating in the death path
 * is the same trap c/kernel/mm/swap.c argues about its own write-out path -- a
 * process that died BECAUSE memory ran out is the one whose dump would then
 * fail to allocate. The buffer is serialised by the BKL, which the ring-3
 * fault path holds at the one call site (c/kernel/cpu/interrupts.c).
 *
 * WHAT A CALLER SEES WHEN IT DOES NOT FIT -- never silence, in three places:
 *   1. every PT_LOAD carries p_memsz = the region's real size and p_filesz =
 *      what was actually written, so the shortfall is in the file's own
 *      structure and `readelf -l` prints it;
 *   2. the LOGIT note carries want_bytes/got_bytes, want_regions/got_regions
 *      and CORE_F_TRUNCATED, and /bin/readcore prints TRUNCATED on its first
 *      line;
 *   3. the kernel prints the same numbers on the serial port at dump time.
 * A dump that quietly held less than it claimed would be worse than no dump,
 * because it would be believed.
 *
 * FILE-BACKED REGIONS ARE NOT DUMPED (header only, p_filesz = 0). Their bytes
 * are already on the disk in the executable that was mapped, and copying a
 * program's text into a dump on a filesystem that holds the program is paying
 * the scarcest resource here for a second copy. Linux's default
 * coredump_filter makes the same choice for the same reason. The LOGIT region
 * table records each one with CORE_RGN_FILE so the reader can say "this
 * existed and was deliberately not dumped" rather than leaving a hole that
 * reads as damage.
 *
 * KNOW THIS BEFORE BELIEVING gdb ABOUT SUCH A REGION, because it is exactly
 * the "plausible small number where there should have been none" shape: ELF
 * defines p_memsz > p_filesz as ZERO FILL, so `x/1xb` on an undumped region
 * prints 00 rather than refusing. Measured, not assumed -- gdb reads 0x00 at
 * the start of a p_filesz = 0 PT_LOAD in this tree's own fixture. That is the
 * format's semantics and Linux's dumps have the same property; it is not
 * something this writer can fix from inside the format. It is why the LOGIT
 * note carries a `kind` per region and why /bin/readcore prints that column:
 * "not dumped, backed by a file" and "dumped, and it was zeroes" are the same
 * bytes to gdb and different findings to a person.
 *
 * WHAT SURVIVES THE CAP IS THE STACK. Segments are described in ascending
 * address order, but the run holding rsp is WRITTEN first, so if the cap is
 * reached it is the heap that is short and never the backtrace. Measured on
 * the real machine, 2026-08-20: /bin/as faulting with mini-libc's 24 MiB arena
 * reserved wrote "258048 of 528384 bytes, TRUNCATED" -- the cap is reached by
 * an ordinary program, so this is a rule and not a precaution. Only the file
 * offsets move; the program-header table stays in address order, because a
 * reader is entitled to that and nothing in the file would explain the
 * reordering.
 *
 * NOT-PRESENT PAGES ARE NOT DUMPED EITHER, and cannot be: reading an unmapped
 * or swapped-out user page from the fault handler would fault in ring 0. Each
 * region is therefore emitted as one PT_LOAD per contiguous run of pages that
 * are mapped RIGHT NOW. An absent page is described by no PT_LOAD, which is
 * what gdb already means by "Cannot access memory at address 0x...".
 * =========================================================================== */

/* 256 KiB. Derived, not rounded: the stack of every program in this tree is
 * 64 KiB or less (c/apps/crt0*.asm and exec.c's initial stack), so a dump that
 * holds the whole stack plus the notes plus a few pages of touched heap fits
 * with room over, and 256 KiB is 64 blocks of a 4 KiB filesystem with ~6,000
 * free -- about 1% of the disk per dump, four dumps kept. Raising it costs
 * .bss on a 512 MiB machine and disk on a 64 MiB one; lowering it starts
 * cutting stacks in half, which is the one thing a dump exists to hold. */
#define CORE_BUF_MAX  (256 * 1024)

/* Program headers: 1 PT_NOTE + up to 63 PT_LOAD. A run of mapped pages costs
 * one; a program with more than 63 separate runs of resident memory has a
 * region map worth reading in the LOGIT note, which is bounded separately. */
#define CORE_PHDR_MAX 64
/* Regions recorded in the LOGIT note. c/kernel/mm/vma.h's VMA_MAXAREA is 32,
 * so 32 is the whole table and never a truncation of it on this machine. */
#define CORE_RGN_MAX  32

/* Rotation. Four slots, /core.1 .. /core.4, reused round-robin.
 *
 * THIS IS THE INODE ANSWER. A name derived from the pid (/core.<pid>) grows a
 * new inode per crash out of the ~26 this image has spare, so a crash loop
 * would exhaust the inode table and the NEXT thing to fail would be something
 * unrelated writing a file. Four fixed names cost at most four inodes for the
 * lifetime of the machine, because a rewrite of an existing path reuses its
 * inode. The pid is not lost -- it is in the dump, in NT_PRSTATUS and in the
 * LOGIT note, which is where a reader looks anyway.
 *
 * Root-level names, not a /core directory: a directory is one more inode and
 * one more thing to create from the fault path, for no gain. */
#define CORE_SLOTS 4

/* Per-boot budget. A crash loop must not be able to grind the disk from inside
 * a page-fault handler; when the budget is spent the kernel says so once and
 * stops writing. Eight = two full rotations, enough to see a pattern. */
#define CORE_MAX_PER_BOOT 8

/* --- region kinds, as recorded in the LOGIT note ------------------------- */
#define CORE_RGN_ANON 0   /* anonymous: dumped, subject to the cap            */
#define CORE_RGN_FILE 1   /* file-backed: header only, bytes are on disk      */
#define CORE_RGN_SHM  2   /* shared segment: header only, not this proc's     */

/* --- LOGIT note flags ---------------------------------------------------- */
#define CORE_F_TRUNCATED 0x1   /* the buffer filled before the regions did     */
#define CORE_F_PHDRFULL  0x2   /* CORE_PHDR_MAX runs reached; more existed     */
#define CORE_F_RGNFULL   0x4   /* CORE_RGN_MAX regions reached; more existed   */
#define CORE_F_SIGCTX    0x8   /* built by the NEGATIVE CONTROL -- see below   */

/* ------------------------------------------------------------------ NOTES */
/* ELF note header. */
struct core_nhdr { uint32_t n_namesz, n_descsz, n_type; };

#define CORE_NT_PRSTATUS 1
#define CORE_NT_FPREGSET 2
#define CORE_NT_PRPSINFO 3
#define CORE_NT_SIGINFO  0x53494749   /* "SIGI" -- Linux's NT_SIGINFO         */
#define CORE_NT_LOGIT    0x4C4F4749   /* "LOGI", under the owner name "LOGIT".
                                       * NOT type 1: readelf and gdb both key
                                       * note types by NUMBER and print an
                                       * unknown owner's type 1 as NT_PRSTATUS
                                       * -- measured, gdb registered a second
                                       * bogus LWP from it. A private note has
                                       * to be numbered out of their way. */

/* NT_PRSTATUS's payload. EVERY OFFSET HERE IS DIFFED AGAINST GLIBC'S
 * `struct elf_prstatus` in tests/unit/coredump_test.c -- this comment is not
 * the authority, that check is. The two int triples and the four timevals are
 * present because they carry the offset of pr_reg, which is the field that
 * matters; they are zero here because this kernel does not account per-process
 * CPU time in a form gdb reads. */
struct core_timeval { int64_t tv_sec, tv_usec; };
struct core_siginfo_s { int32_t si_signo, si_code, si_errno; };
struct core_prstatus {
    struct core_siginfo_s pr_info;
    int16_t  pr_cursig;
    uint16_t pr_pad0;      /* glibc pads pr_cursig to pr_sigpend's alignment;
                            * measured: pr_sigpend is at 16, not 24 */
    uint64_t pr_sigpend, pr_sighold;
    int32_t  pr_pid, pr_ppid, pr_pgrp, pr_sid;
    struct core_timeval pr_utime, pr_stime, pr_cutime, pr_cstime;
    /* x86_64 elf_gregset_t, in <sys/user.h>'s struct user_regs_struct order.
     * NOT this kernel's struct registers order -- the two differ, which is
     * precisely the kind of thing gdb reads silently wrong. */
    uint64_t pr_reg[27];
    int32_t  pr_fpvalid;
    int32_t  pr_pad1;
};

/* Indices into pr_reg[], named so the copy that fills them can be read. */
enum {
    CORE_R15 = 0, CORE_R14, CORE_R13, CORE_R12, CORE_RBP, CORE_RBX,
    CORE_R11, CORE_R10, CORE_R9, CORE_R8, CORE_RAX, CORE_RCX, CORE_RDX,
    CORE_RSI, CORE_RDI, CORE_ORIG_RAX, CORE_RIP, CORE_CS, CORE_EFLAGS,
    CORE_RSP, CORE_SS, CORE_FS_BASE, CORE_GS_BASE, CORE_DS, CORE_ES,
    CORE_FS, CORE_GS
};

/* NT_PRPSINFO's payload, glibc `struct elf_prpsinfo`. Offsets diffed the same
 * way. gdb prints pr_fname/pr_psargs as the program name in `info proc`. */
struct core_prpsinfo {
    int8_t   pr_state, pr_sname, pr_zomb, pr_nice;
    uint8_t  pr_pad0[4];
    uint64_t pr_flag;
    uint32_t pr_uid, pr_gid;
    int32_t  pr_pid, pr_ppid, pr_pgrp, pr_sid;
    char     pr_fname[16];
    char     pr_psargs[80];
};

/* The LOGIT note: everything ELF core has no field for. THIS is where the
 * fault address and the CPU error code live in a form our own reader trusts;
 * si_addr in NT_SIGINFO carries cr2 as well, for gdb's benefit, and the two
 * are written from the same variable so they cannot disagree. */
struct core_region_note {
    uint64_t start, end;      /* VA range                                     */
    uint64_t dumped;          /* bytes of it actually in this file            */
    uint32_t prot;            /* VMA_READ/WRITE/EXEC                          */
    uint32_t kind;            /* CORE_RGN_*                                   */
};
#define CORE_LOGIT_MAGIC 0x4C434F52u   /* "LCOR", little-endian in the file    */
#define CORE_LOGIT_VER   1
struct core_logit_note {
    uint32_t magic, version;
    uint32_t flags;           /* CORE_F_*                                     */
    uint32_t signo;
    uint64_t trapno;          /* the CPU vector: 14 = page fault              */
    uint64_t err;             /* the page-fault error code                    */
    uint64_t cr2;             /* the faulting address                         */
    uint64_t cr3;             /* the address space it faulted in              */
    uint64_t want_bytes, got_bytes;
    uint32_t want_regions, got_regions;
    uint32_t nregion;         /* entries in region[] below                    */
    uint32_t pad;
    struct core_region_note region[CORE_RGN_MAX];
};

/* ------------------------------------------------------------- THE BUILDER */
/* Deliberately a pure function over callbacks, with no kernel type in its
 * signature beyond `struct registers`: that is what lets tests/unit/
 * coredump_test.c drive the REAL builder with a modelled address space and
 * then hand the bytes to gdb. A builder reachable only from a page fault could
 * be checked only by crashing, and only on the machine. */
struct core_region {
    uint64_t start, end;
    uint32_t prot;
    uint32_t kind;            /* CORE_RGN_* */
};

struct core_src {
    /* Fill up to `max` regions, ascending by start. Returns how many EXIST
     * (which may exceed `max` -- that difference is CORE_F_RGNFULL). */
    int  (*regions)(void *ctx, struct core_region *out, int max);
    /* Is [va, va+4096) readable in the dying process RIGHT NOW? */
    int  (*mapped)(void *ctx, uint64_t va);
    /* Copy that page into `dst`. Only ever called when mapped() said yes. */
    void (*read_page)(void *ctx, uint64_t va, void *dst);
    void *ctx;
};

struct core_meta {
    int      pid, ppid;
    uint32_t uid, gid;
    int      signo;
    uint64_t trapno, err, cr2, cr3;
    const char *name;
};

struct registers;   /* c/kernel/cpu/interrupts.h */

/* Build the whole file into `buf` (`cap` bytes). Returns the number of bytes
 * written, or a negative CORE_E_*. `fx` is the 512-byte FXSAVE area c/boot/
 * isr.asm filled on kernel entry -- the ONLY correct source for the program's
 * FP/SSE state, since kernel C has clobbered the live registers by now. It may
 * be NULL, in which case pr_fpvalid is 0 and no NT_FPREGSET is emitted; a
 * zeroed FP note would claim the program's FP state was zero, which is a
 * different and false statement. `out` may be NULL. */
#define CORE_E_SMALL (-1)   /* `cap` cannot even hold the headers and notes    */
#define CORE_E_ARG   (-2)
int coredump_build(void *buf, int cap, const struct core_meta *m,
                   const struct registers *r, const void *fx,
                   const struct core_src *src, struct core_logit_note *out);

/* Walk a finished dump and hand back NT_PRSTATUS's 27 registers (indices are
 * the CORE_R15..CORE_GS enum above). Returns 0, or -1 if there is no readable
 * register note. Used by the kernel to quote the FILE on its [core] line
 * instead of quoting the trap frame a second time -- see the long comment at
 * the definition for why the reader shipped as /bin/readcore deliberately does
 * NOT share this code. */
int coredump_read_gregs(const void *buf, int n, uint64_t *greg27);

/* ------------------------------------------------------------- THE KERNEL */
/* Called from the ring-3 fault path in c/kernel/cpu/interrupts.c, in the
 * faulting process's own address space, immediately before proc_exit(). Prints
 * one [core] line and returns; never fails the caller, because a dump that
 * could not be written must not turn a dead app into a dead kernel. */
void coredump_take(const struct registers *r, const void *fx,
                   int signo, uint64_t cr2);

/* The path a dump was last written to, and how many have been written this
 * boot. For the on-device harness and for /bin/readcore's default argument. */
const char *coredump_last_path(void);
int         coredump_count(void);

#endif /* LOGIT_COREDUMP_H */
