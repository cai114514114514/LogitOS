#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"
#include "proc.h"
#include "sched.h"
#include "file.h"
#include "vmm.h"
#include "pmm.h"
#include "vma.h"         /* the CLI stack is a RESERVATION, faulted in on touch */
#include "pcache.h"      /* the file handle a program's text is mapped from */
#include "aex.h"
#include "elf.h"      /* struct elf_image + the AT_* auxv tags */
#include "vfs.h"
#include "kheap.h"
#include "usercopy.h"
#include "logit_abi.h"
#include "kprintf.h"
#include "prot.h"        /* PTE_NX + cpu_prot_nx(): the user stack is data */

void *memcpy(void *, const void *, size_t);

#define MAXARG          48
#define ARGBUFSZ        4096          /* total bytes for all argv + envp strings */
#define CLI_STACK_PAGES 256           /* 1 MiB user stack for a CLI program */

/* WHAT AN EXEC COSTS, split so the answer is actionable.
 *
 * /bin/sh fork+execs for every command, so exec is on the interactive path, and
 * "exec is slow" is not a finding -- "exec spends N% of itself allocating a
 * megabyte of stack the program will never touch" is. Same instrument as
 * proc.c's fork accounting (rdtsc, reported as cycles) for the same reason: a
 * whole exec rounds to 0 or 1 of the 100 Hz ticks. */
static uint64_t g_execs, g_exec_cyc, g_exec_stack_cyc, g_exec_load_cyc;

/* WHAT THE LOADER DID WITH THE PAGES, across every load this boot -- execve,
 * proc_spawn, cap_spawn and wm_launch alike, so the number covers the desktop
 * and not only the shell.
 *
 * It is here and not in elf.c because elf.c is compiled into the host loader
 * tests, where a boot-lifetime counter would be a global that survives between
 * cases. exec_note_load() is called by every site that loads an image, which
 * is also what makes "a load that took the eager path" visible: file_runs 0
 * with copied nonzero is the eager loader, and that is what a v1 image, an
 * unstattable path and a full pcache file table all look like from here. */
static uint64_t g_ld_loads, g_ld_filepg, g_ld_copypg, g_ld_runs, g_ld_eager;

/* The per-load line is BOUNDED and printed from the first load onward, rather
 * than folded into exec_report()'s every-eighth-exec cadence. Two reasons, both
 * learned the hard way on this change: a boot that never reaches a shell does
 * two loads, so an every-8 line never prints at all and the feature reads as
 * dead; and the interesting comparison is between NAMED programs -- 870 pages
 * for the browser and 5 for /bin/login are the same feature working, and an
 * average over both says nothing about either. */
#define LOAD_REPORT_MAX 24

void exec_note_load(const char *what, const struct elf_image *ei)
{
    if (!ei) return;
    g_ld_loads++;
    g_ld_filepg += ei->file_pages;
    g_ld_copypg += ei->copied_pages;
    g_ld_runs   += ei->file_runs;
    if (!ei->file_runs) g_ld_eager++;
    if (g_ld_loads <= LOAD_REPORT_MAX)
        kprintf("[exec] load %s: %d pages file-backed in %d areas, %d copied\n",
                what ? what : "?", (int)ei->file_pages, (int)ei->file_runs,
                (int)ei->copied_pages);
}

static inline uint64_t exec_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define EXEC_REPORT_EVERY 8

static void exec_report(void)
{
    if (!g_execs || (g_execs % EXEC_REPORT_EVERY)) return;
    kprintf("[exec] %d execs, %d kcycles each: %d in aex_load, %d in the "
            "%d-page user stack\n",
            (int)g_execs, (int)(g_exec_cyc / g_execs / 1000),
            (int)(g_exec_load_cyc / g_execs / 1000),
            (int)(g_exec_stack_cyc / g_execs / 1000), CLI_STACK_PAGES);
    /* Beside it, because the two answer one question together: the cycles say
     * what a load cost and this says why. A gate reads `file` going up and
     * `copied` coming down; `eager` going up instead is the whole feature
     * being declined, quietly, which is the failure mode worth naming. */
    kprintf("[exec] loader: %d loads (%d eager), %d pages from the page cache "
            "in %d areas, %d pages copied\n",
            (int)g_ld_loads, (int)g_ld_eager, (int)g_ld_filepg,
            (int)g_ld_runs, (int)g_ld_copypg);
}

static int kstrlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max)
{ int i = 0; for (; s && i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0; }

/* Copy a user argv/envp vector into kernel storage. Returns the count, or -1.
 * Each string is packed into `store` (advancing *used); vec[] gets kernel ptrs. */
static int copy_uvec(char **uvec, char *vec[MAXARG], char *store, int *used, int storemax)
{
    if (!uvec) return 0;                          /* NULL vector -> empty */
    int n = 0;
    for (; n < MAXARG; n++) {
        if (!user_range_ok(&uvec[n], sizeof(char *), 0)) return -1;
        char *uptr = uvec[n];
        if (!uptr) break;                         /* NULL terminator */
        char *dst = store + *used;
        int wrote = user_copy_string(dst, storemax - *used, uptr);
        if (wrote < 0) return -1;
        vec[n] = dst;
        *used += wrote + 1;
    }
    return n;
}

/* How much of the stack is mapped up front. Everything the kernel itself writes
 * has to be there before the program starts, because those writes happen with
 * the target space active but no faulting thread to resolve them cheaply:
 * the argv/envp strings (bounded by ARGBUFSZ) plus the pointer vector
 * (1 + MAXARG + 1 + MAXARG + 1 slots) plus two 16-byte alignments. Everything
 * above that is the PROGRAM's stack, and the program faults its own pages in.
 *
 * Kept at the true minimum rather than padded, for a reason that is about
 * evidence and not about memory: with a generous eager window /bin/sh never
 * touches an unmapped page, the demand-paging path never executes, and the
 * first program with a deep call chain would be the one to discover whether it
 * works. At two pages the ordinary shell faults its own stack in on every boot,
 * so `[mm] ... anon` is nonzero in every log and the path is exercised
 * continuously instead of being trusted. */
/* The auxiliary vector costs 2 words a pair, and the executable's own path is
 * pushed as a string for AT_EXECFN. */
#define AUXV_PAIRS      14
#define CLI_STACK_HEAD  (ARGBUFSZ + 160 + (2 * MAXARG + 3 + 2 * AUXV_PAIRS) * 8 + 48)
#define CLI_STACK_EAGER 2
/* If the head ever outgrows the eager window, exec would write into an unmapped
 * page from kernel context with the wrong CR3 semantics -- so it is a build
 * error, not a runtime surprise. */
_Static_assert(CLI_STACK_HEAD <= CLI_STACK_EAGER * 4096,
               "the SysV initial stack no longer fits the eagerly mapped pages");

/* Map a CLI user stack above the program image and build the SysV initial stack
 * (argc, argv[], NULL, envp[], NULL, strings). The target space MUST be active,
 * and `cr3` must name it (the VMA table is indexed by address space, and
 * proc_spawn builds a stack in a space that is active but is not p->cr3 yet).
 * Returns the user rsp (points at argc, 16-aligned), or 0 on OOM.
 *
 * WHY THIS IS NOT 256 EAGER PAGES ANY MORE.
 * Measured, before: 708 kcycles per execve, of which 522 kcycles -- 74% -- was
 * this function allocating, zeroing-by-poison-check and mapping a megabyte of
 * stack. /bin/sh touches a few kilobytes of it. The megabyte is a RESERVATION,
 * which is exactly what a VMA is for, and demand paging has been able to
 * materialise one on first touch since the fault hook was wired: the pages that
 * are used cost a fault each and the pages that are not cost nothing at all.
 *
 * The reservation is not optional. Without it mm_fault_classify() sees an
 * address no VMA covers, returns MM_FAULT_NONE, and the process dies on its
 * first deep call -- so a failed reservation falls back to mapping eagerly
 * rather than handing out a stack that faults. */
static uint64_t setup_cli_stack(uint64_t cr3, const struct elf_image *img,
                                const char *execfn, char **argv, int argc,
                                char **envp, int envc)
{
    uint64_t entry = img->entry;
    uint64_t base = entry & ~(uint64_t)0xFFFFF;
    uint64_t top = base + 0x4000000;                 /* 64 MiB above base */
    uint64_t bottom = top - (uint64_t)CLI_STACK_PAGES * 0x1000;

#ifdef KBENCH_NEGCTL
    /* The negative control (tests/kbench.mk): map the whole megabyte up front,
     * the way this did before. tests/boot/run-kbench.sh must FAIL against it --
     * on the cost of exec, on the pages a fork then has to share, and on the
     * anonymous-fault count, which goes to zero because nothing is ever
     * missing. */
    int eager = CLI_STACK_PAGES;
    (void)bottom;
#else
    int reserved = (vma_reserve_fixed(cr3, bottom, (uint64_t)CLI_STACK_PAGES * 0x1000,
                                      VMA_READ | VMA_WRITE) == 0);
    int eager = reserved ? CLI_STACK_EAGER : CLI_STACK_PAGES;
#endif

    /* NX on the stack: it is data, and without it the classic shape -- overflow
     * a buffer, land the return address on the bytes you just pushed -- works
     * exactly as written. The stack is the likeliest place for a program to be
     * handed attacker-controlled input, so it is the page this most wants.
     *
     * WHERE THE ANSWER COMES FROM, which is the part that changed: it used to
     * be decided here, and independently again in c/kernel/gui/wm.c, with the
     * program file having no say. It is now PT_GNU_STACK -- the header the ELF
     * has always carried for exactly this and that this loader ignored. The
     * loader refuses an image that asks for PF_X outright (see elf.c), so by
     * the time we get here the only question left is whether NX can be
     * expressed at all.
     *
     * Which it currently cannot: cpu_prot_nx_usable() returns 0 because
     * c/kernel/mm's PTE->frame masks keep bit 63 (prot.h has the full account,
     * including the kernel #GP it produces). Left in this shape deliberately
     * rather than deleted -- when that mask changes, one function starts
     * returning 1 and the stack becomes no-execute with no edit here.
     *
     * Two further gaps that stay open even then, both outside this line:
     *   - only the EAGER pages pass through here. The rest of the reservation
     *     is faulted in by c/kernel/mm/fault.c's do_anon(), which maps from the
     *     VMA protection and ignores VMA_EXEC.
     *   - GUI apps get their stack from c/kernel/gui/wm.c, not from here. */
    int want_nx = !(img->stack_flags & PF_X) && cpu_prot_nx_usable();
    uint64_t stack_flags = VMM_WRITABLE | VMM_USER | (want_nx ? PTE_NX : 0);
    for (int i = 1; i <= eager; i++) {
        uint64_t frame = pmm_alloc();
        if (!frame) return 0;
        vmm_map_page(top - (uint64_t)i * 0x1000, frame, stack_flags);
    }
    uint64_t sp = top, uargv[MAXARG], uenvp[MAXARG], uexecfn = 0;
    for (int i = 0; i < argc; i++) { int l = kstrlen(argv[i]); sp -= l + 1; memcpy((void *)sp, argv[i], l + 1); uargv[i] = sp; }
    for (int i = 0; i < envc; i++) { int l = kstrlen(envp[i]); sp -= l + 1; memcpy((void *)sp, envp[i], l + 1); uenvp[i] = sp; }
    if (execfn) { int l = kstrlen(execfn); sp -= l + 1; memcpy((void *)sp, execfn, l + 1); uexecfn = sp; }
    sp &= ~(uint64_t)0xF;

    /* THE AUXILIARY VECTOR.
     *
     * The initial stack used to be argc / argv[] / NULL / envp[] / NULL and
     * then stop -- which is a truncated SysV stack, not a small one. Anything
     * ported expects the auxv after the environment: a static libc reads
     * AT_PAGESZ before it can round anything, AT_RANDOM to seed its stack
     * canary, AT_PHDR/AT_PHNUM to find its own PT_TLS, and AT_ENTRY to know
     * where it started. A program that goes looking for them found whatever
     * happened to be above the NULL, which on this stack is the environment
     * strings -- so the failure was not "no auxv", it was "auxv full of
     * filenames".
     *
     * AT_PHDR and AT_RANDOM are addresses in the read-only page the loader
     * places above the image (elf.c), so nothing here has to allocate for them.
     * AT_SECURE is 0 and AT_UID/GID are 0 because this system has one user;
     * they are emitted rather than omitted because a libc that does not find
     * AT_SECURE assumes the worst. */
    int nslots = 1 + (argc + 1) + (envc + 1) + 2 * AUXV_PAIRS;
    sp -= (uint64_t)nslots * 8;
    sp &= ~(uint64_t)0xF;                            /* 16-align argc */
    uint64_t *st = (uint64_t *)sp; int k = 0;
    st[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) st[k++] = uargv[i];
    st[k++] = 0;
    for (int i = 0; i < envc; i++) st[k++] = uenvp[i];
    st[k++] = 0;
    st[k++] = AT_PHDR;     st[k++] = img->phdr_va;
    st[k++] = AT_PHENT;    st[k++] = img->phentsize;
    st[k++] = AT_PHNUM;    st[k++] = img->phnum;
    st[k++] = AT_PAGESZ;   st[k++] = 0x1000;
    st[k++] = AT_BASE;     st[k++] = 0;              /* no interpreter */
    st[k++] = AT_FLAGS;    st[k++] = 0;
    st[k++] = AT_ENTRY;    st[k++] = img->entry;
    st[k++] = AT_UID;      st[k++] = 0;
    st[k++] = AT_EUID;     st[k++] = 0;
    st[k++] = AT_GID;      st[k++] = 0;
    st[k++] = AT_SECURE;   st[k++] = 0;
    st[k++] = AT_RANDOM;   st[k++] = img->random_va;
    st[k++] = AT_EXECFN;   st[k++] = uexecfn;
    st[k++] = AT_NULL;     st[k++] = 0;
    /* One pair short and the reservation is bigger than the vector (harmless);
     * one pair long and the vector overwrites an argv string (not). Counted,
     * not asserted in a comment, because AUXV_PAIRS and the block above are two
     * places that have to agree and nothing else makes them. */
    if (k != nslots)
        kprintf("[exec] auxv miscount: wrote %d slots, reserved %d\n", k, nslots);
    return sp;
}

/* execve(path, argv, envp): replace the current process's user address space with
 * a freshly loaded program. fds + cwd survive; on success this does not return to
 * the caller -- it rewrites the syscall frame `r` so the iretq enters the new
 * program with a SysV argc/argv/envp stack. */
long proc_execve(struct registers *r)
{
    struct proc *p = proc_current();
    if (!p) return -1;

    /* 1. Snapshot path + argv + envp into kernel memory BEFORE we tear down the
     *    user address space they live in. (exec runs with IF=0, so the static
     *    staging buffers are safe from preemption.) */
    char path[128];
    if (user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) return -1;
    char abs[128];
    proc_resolve(p, path, abs, sizeof abs);

    static char argstore[ARGBUFSZ];
    static char *argv[MAXARG], *envp[MAXARG];
    int used = 0;
    int argc = copy_uvec((char **)r->rsi, argv, argstore, &used, ARGBUFSZ);
    if (argc < 0) { kprintf("[execve] %s: bad argv\n", abs); return -1; }
    int envc = copy_uvec((char **)r->rdx, envp, argstore, &used, ARGBUFSZ);
    if (envc < 0) { kprintf("[execve] %s: bad envp\n", abs); return -1; }

    /* 1.5. The execute bit. vfs_read()/vfs_size() below check MAY_READ (a
     * loadable file must be readable) but nothing on this path has ever asked
     * MAY_EXEC -- the two real call sites in c/fs/vfs.c (search permission on
     * a directory being traversed, and the parent-write check for create/
     * remove/rename) are both about directories, never about the FILE being
     * run. So the execute bit stored on a regular file (vfs_meta.c, durable
     * across reboot -- see CLAUDE.md's Storage section) was recorded and never
     * consulted: any process that could read /bin/sh could execve() a data
     * file with no x bit at all. Checked here, before the old address space is
     * torn down, so a refusal leaves the caller exactly as vfs_read failing
     * already does two lines below. */
    if (vfs_access(abs, MAY_EXEC) < 0) {
        kprintf("[execve] %s: permission denied (not executable)\n", abs);
        return -1;
    }

    /* 2. Validate the container's fixed header before destroying the old space,
     *    so a bad path/exec leaves the caller intact and returns -1.
     *
     * THIS USED TO BE A kmalloc OF THE WHOLE FILE and one vfs_read into it, and
     * that was the ceiling on how big a program this machine could run: kmalloc
     * -> kheap grow() -> pmm_alloc_contig(), which DOUBLES an arena to cover the
     * request and then wants it in ONE contiguous physical run. Measured
     * 2026-08-20: a 128 MiB file took a 256 MiB arena, and a 256 MiB file was
     * refused with 456 MiB free, because the doubling asked for 512 MiB on a
     * 511 MiB machine. Now nothing here holds the image at all -- aex_load_path
     * reads the header, then each segment, straight out of the file.
     *
     * What is still checked HERE rather than later is the 64 bytes every AEX
     * version agrees on. That placement is the whole point of splitting it out:
     * everything after the vmm_free_user() below is fatal to the caller, so the
     * cheap "is this a program at all" answer has to come before it, exactly as
     * the old aex_info(img, ...) did. */
    int sz = vfs_size(abs);
    if (sz < AEX_HDR_SIZE) { kprintf("[execve] %s: missing/too small (%d)\n", abs, sz); return -1; }
    char nm[32], ext[8];
    if (aex_info_path(abs, nm, ext) != 0) { kprintf("[execve] %s: bad aex header\n", abs); return -1; }

    /* M28 D1: `p->caps` and `p->fs_prefix` are DELIBERATELY untouched anywhere
     * in this function, and that silence is the load-bearing part of the
     * whole capability design, not an oversight to fill in later.
     *
     * The design document this milestone starts from said a capability is
     * "granted by the kernel at execve from a per-process set" -- which has
     * no referent here, because every AetherScript program execve()s the
     * SAME binary (/bin/as); a grant keyed to WHICH IMAGE just loaded could
     * not tell two scripts apart, and would silently hand every script
     * whatever /bin/as itself holds (spec section 1). So the grant rides on
     * the PROCESS (this struct proc, same pid, same slot, before and after
     * this function runs) and never on the image replacing it. execve is a
     * content change, not an identity change, and the capability set is
     * part of the process's identity here -- exactly as pid and ppid are
     * left alone by this function too. If a future change ever makes execve
     * touch `caps`, that change re-opens the exact hole D1 exists to close. */

    /* 3. Point of no return: swap the user address space. */
    kprintf("[execve] pid %d: %s loading\n", p->pid, abs);   /* DIAG: reached = child alive */
    uint64_t t_exec = exec_rdtsc();
    uint64_t cr3 = p->cr3;
    vmm_free_user(cr3);
    struct elf_image ei;
    /* AFTER vmm_free_user, which drops the OLD image's areas and with them
     * their references to whatever this process was running a moment ago --
     * so re-exec'ing the same binary puts the old handle before taking the new
     * one, and the entry never briefly counts twice. A -1 (no slot, an
     * unstattable path, a backend with no real inode numbers) is not an error
     * here: the loader copies, exactly as it always did. */
    int fh = pcache_file_open(abs);
#ifdef EXEC_NEGCTL_SLURP
    /* THE NEGATIVE CONTROL (tests/exec.mk's test-bigexec-negctl): this function
     * exactly as it was before the streaming loader -- kmalloc the whole file,
     * one vfs_read into it, load from memory. It is not "the feature switched
     * off"; it is the PLAUSIBLE implementation, the one that shipped, and it
     * loads every ordinary program on this disk perfectly well. What it cannot
     * do is the thing being measured, and it cannot do it at a size the
     * measurement names rather than at any size at all -- which is why the gate
     * requires a SMALL pad to still pass against this build. A control that
     * fails everywhere is measuring "did I break the loader", not the ceiling.
     *
     * The refusal it produces is `[oom] kmalloc(N) refused` from kheap's grow()
     * -> pmm_alloc_contig(), with the frames-free count printed beside it, and
     * that free count is normally far LARGER than the request. */
    int lrc = -1;
    if (sz <= 0x7fffffff - 511) {
        int bytes = ((sz + 511) / 512) * 512;
        void *img = kmalloc((unsigned)bytes);
        if (img) {
            if (vfs_read(abs, img, bytes) > 0)
                lrc = aex_load_image_ex(img, (uint64_t)bytes, nm, ext, &ei, fh);
            kfree(img);
        } else {
            kprintf("[execve] %s: kmalloc %d failed\n", abs, bytes);
        }
    }
#else
    int lrc = aex_load_path(abs, (uint64_t)sz, nm, ext, &ei, fh);  /* maps into the active (p->cr3) space */
#endif
    if (fh >= 0) pcache_file_put(fh);          /* the VMAs hold their own */
    uint64_t t_load = exec_rdtsc();
    if (lrc != 0) { kprintf("[execve] %s: aex_load failed\n", abs); proc_exit(127); }
    exec_note_load(abs, &ei);   /* after the refusal: a load that failed part-way
                                 * left counts that describe no running program */
    uint64_t entry = ei.entry;

    /* PT_TLS: install the thread pointer the loader laid out. Through
     * sched_set_fsbase() and never with a bare WRMSR -- the scheduler keeps
     * `hardware IA32_FS_BASE == current->fsbase` as an invariant and decides
     * whether to reload the MSR by comparing the two threads' fields, so a
     * write that changed only the hardware would survive exactly until the next
     * switch back. Set unconditionally, including to 0: an execve REPLACES the
     * image, and a program with no thread-local storage must not inherit the
     * thread pointer of the program that was here a moment ago. */
    sched_set_fsbase(ei.tls_tp);

    /* 4+5. Fresh user stack with the SysV argc/argv/envp/auxv layout. */
    uint64_t sp = setup_cli_stack(cr3, &ei, abs, argv, argc, envp, envc);
    if (!sp) { kprintf("[execve] %s: stack setup failed\n", abs); proc_exit(127); }
    {
        uint64_t t_end = exec_rdtsc();
        g_execs++;
        g_exec_cyc       += t_end  - t_exec;
        g_exec_load_cyc  += t_load - t_exec;
        g_exec_stack_cyc += t_end  - t_load;
        exec_report();
    }

    /* 6. Rewrite the syscall-return frame to land in the new program. */
    scopy(p->name, nm, sizeof p->name);
    r->rip = entry; r->rsp = sp; r->rflags = 0x202; r->cs = 0x1B; r->ss = 0x23;
    r->rax = r->rbx = r->rcx = r->rdx = r->rsi = r->rdi = r->rbp = 0;
    r->r8 = r->r9 = r->r10 = r->r11 = r->r12 = r->r13 = r->r14 = r->r15 = 0;
    return 0;
}

/* init: spawn `path` as a fresh CLI process with fd 0/1/2 bound to the serial
 * console. Used by the kernel to launch /bin/sh after boot. Returns the pid. */
int proc_spawn(const char *path, char **argv)
{
    int sz = vfs_size(path);
    if (sz < AEX_HDR_SIZE) return -1;        /* aex_info reads the 64-byte header */
    char nm[32], ext[8];
    if (aex_info_path(path, nm, ext) != 0) return -1;

    uint64_t space = vmm_new_space();
    if (!space) return -1;

    int argc = 0; while (argv && argv[argc]) argc++;

    /* BEFORE the cli below, and that placement is the whole reason this is not
     * one line inside the block: pcache_file_open() stats the path, which is a
     * filesystem call, and this kernel's block drivers poll with interrupts ON
     * (c/kernel/cpu/interrupts.c's non-preemptible busy flags). Opening it
     * inside the interrupts-off window would be a device wait with no timer. */
    int fh = pcache_file_open(path);

    /* Load + build the stack with the new space active (it isn't current yet).
     *
     * THE LOADER NOW READS THE DISK FROM INSIDE THIS WINDOW, which the comment
     * above pcache_file_open() warns against, so here is why it is safe and
     * what would make it stop being safe.
     *
     * The worry that comment names is "a device wait with no timer" -- a poll
     * whose timeout comes from the PIT, which does not advance with IF=0. There
     * is exactly ONE place a synchronous block transfer waits, and it is not in
     * a driver: c/drivers/block/blkdev.c's blk_wait() saves RFLAGS, raises
     * g_ata_busy, does its OWN sti, polls, and restores the caller's IF. Every
     * backend goes through it. So the read gets interrupts whatever this
     * function's IF is, and its completion does not depend on the tick
     * (virtio.c polls a spin COUNT, 200,000,000 iterations).
     *
     * The non-preemption matters far more than the timer here, and it is the
     * part specific to this call site: g_ata_busy makes interrupts.c skip
     * schedule() (c/kernel/cpu/interrupts.c:288), and without that the sti
     * inside blk_wait could switch this thread out -- which would restore this
     * THREAD's cr3 on the way back, leaving the loader writing its segments
     * into the wrong address space, silently.
     *
     * So the requirement is precise and it is one function's: if blk_wait ever
     * stops holding the no-preemption flag across a transfer, this call site
     * breaks. Nothing about which driver is underneath matters. */
    uint64_t prev;
    __asm__ volatile ("cli");
    __asm__ volatile ("mov %%cr3, %0" : "=r"(prev));
    vmm_switch(space);
    struct elf_image ei;
    uint64_t entry = aex_load_path(path, (uint64_t)sz, nm, ext, &ei, fh) == 0 ? ei.entry : 0;
    uint64_t sp = entry ? setup_cli_stack(space, &ei, path, argv, argc, 0, 0) : 0;
    vmm_switch(prev);
    __asm__ volatile ("sti");
    if (fh >= 0) pcache_file_put(fh);
    if (entry) exec_note_load(path, &ei);
    if (!entry || !sp) { vmm_free_space(space); return -1; }

    struct proc *p = proc_create(space, NULL, nm, 0);
    if (!p) { vmm_free_space(space); return -1; }

    /* M28 D1: THE CHAIN'S ROOT. This is the one call in the whole tree where
     * the kernel itself launches a process -- init's shell, on the serial
     * console -- rather than a process launching another. alloc_proc() (see
     * proc.c) resets a fresh slot's capability set to DENY-ALL precisely so
     * that no creation site gets a grant "for free"; this is the ONE site
     * that is allowed to hand one out by construction instead of narrowing
     * one down, and it says so explicitly rather than relying on some
     * kernel-wide default. Every other process's set traces back to this
     * grant through SYS_FORK (unchanged) and SYS_CAP_SPAWN (ceiling-checked)
     * -- never invented anywhere else. */
    p->caps = CAP_ALL; p->fs_prefix[0] = 0;

    struct file *tty = file_open_tty();              /* fd 0/1/2 -> serial console */
    if (tty) { p->fd[0] = tty; file_dup(tty); p->fd[1] = tty; file_dup(tty); p->fd[2] = tty; }

    p->tid = thread_create_user(nm, entry, sp, p, space);
    if (p->tid < 0) {                    /* OOM: undo the spawn (same shape as proc_fork's
                                          * failure path) instead of leaking the PCB slot
                                          * + the whole address space under a live pid. */
        for (int i = 0; i < NFD; i++)
            if (p->fd[i]) { file_close(p->fd[i]); p->fd[i] = NULL; }
        vmm_free_space(space);
        p->state = PROC_FREE; p->pid = 0; p->cr3 = 0;
        return -1;
    }
    return p->pid;
}

/* SYS_CAP_SPAWN: fork()+load+execve(path) in ONE kernel call, except the
 * child's OWN struct proc does not inherit a copy of the caller's capability
 * set -- it gets exactly `req` (include/abi/logit_abi.h's struct
 * logit_capreq), and the call is refused OUTRIGHT -- no address space
 * allocated, no child created, nothing partially built -- unless
 * proc_cap_subset() (proc.c) accepts `req` against the CALLER's own current
 * (caps, fs_prefix). This is D1's ceiling
 * (docs/superpowers/specs/2026-08-14-m28-capabilities.md), enforced at the
 * one place a process's grant is allowed to shrink.
 *
 * WHY A FRESH NUMBER AND NOT SYS_SPAWN (63) BROUGHT BACK TO LIFE. SYS_SPAWN
 * already has a real, existing caller: fsroot/as/lib/abi.as's spawn()
 * (line ~374) does `syscall(SYS_SPAWN, addr(path), addr(argv))` -- exactly
 * two arguments, matching SYS_SPAWN's ORIGINAL M18 doc comment, and SYS_SPAWN
 * has never had a kernel dispatch case (grep c/kernel confirms it: every call
 * to it today falls through to wm_gui_syscall()'s default and returns -1).
 * Bringing it to life with a now-MANDATORY third argument -- a capability
 * request, which must never be "whatever happened to be in the register" --
 * would silently change what that existing symbolic name means the moment
 * abi.as's spawn() is exercised again, without abi.as ever having been asked
 * to pass one: `req` would be built from rdx as the AetherScript syscall()
 * native happens to leave it, and every existing caller of spawn() would be
 * handed either garbage capabilities or a hard refusal, depending on what
 * garbage decoded to. A fresh number costs one #define; silently redefining
 * an ABI symbol another line's code already calls costs a debugging session
 * nobody would think to have near this file. See the note above SYS_SPAWN's
 * own definition in include/abi/logit_abi.h.
 *
 * SHAPE: closer to SYS_FORK immediately followed by SYS_EXECVE than to
 * proc_spawn() above (which is the KERNEL launching init from NOTHING, with
 * no caller to inherit from) -- the child gets the CALLER's fd table (dup'd,
 * exactly like SYS_FORK) and cwd, not a bare tty. It does NOT get an envp:
 * there is no register left to carry one (path, argv, and the capability
 * request already claim all three int 0x80 slots), so the child's
 * environment is empty -- the same as SYS_SPAWN's original documented shape
 * and proc_spawn()'s own child, not a new gap this call introduces. */
long proc_cap_spawn(struct registers *r)
{
    struct proc *p = proc_current();
    if (!p) return LOGIT_CAP_E_ARG;

    char path[128];
    if (user_copy_string(path, sizeof path, (const char *)r->rdi) < 0) return LOGIT_CAP_E_ARG;
    char abs[128];
    proc_resolve(p, path, abs, sizeof abs);

    static char cs_argstore[ARGBUFSZ];
    static char *cs_argv[MAXARG];
    int used = 0;
    int argc = copy_uvec((char **)r->rsi, cs_argv, cs_argstore, &used, ARGBUFSZ);
    if (argc < 0) { kprintf("[cap_spawn] %s: bad argv\n", abs); return LOGIT_CAP_E_ARG; }

    struct logit_capreq req;
    if (!user_range_ok((const void *)r->rdx, sizeof req, 0)) return LOGIT_CAP_E_ARG;
    if (user_copy_from(&req, (const void *)r->rdx, sizeof req) < 0) return LOGIT_CAP_E_ARG;
    req.prefix[sizeof req.prefix - 1] = 0;   /* a short/hostile copy must not run the
                                              * containment test past this buffer */

    /* THE CEILING, checked before anything is touched. A refused spawn
     * leaves the caller's own process exactly as it was and creates nothing
     * -- not a half-built child whose set gets "corrected" after the fact. */
    if (!proc_cap_subset(req.caps, req.prefix, p->caps, p->fs_prefix)) {
        kprintf("[cap_spawn] pid %d: %s refused (requested caps exceed the caller's)\n",
                p->pid, abs);
        return LOGIT_CAP_E_CEIL;
    }

    /* From here down: load + validate the image before creating anything the
     * caller or anyone else can observe, same discipline as proc_execve() --
     * including the execute-bit check proc_execve() carries above (B3: this
     * call is the second, and until now only, unguarded loader in this file --
     * fixing proc_execve() alone would have left SYS_CAP_SPAWN as a standing
     * bypass of the very check just added). */
    if (vfs_access(abs, MAY_EXEC) < 0) {
        kprintf("[cap_spawn] %s: permission denied (not executable)\n", abs);
        return LOGIT_CAP_E_NOENT;
    }
    int sz = vfs_size(abs);
    if (sz < AEX_HDR_SIZE) return LOGIT_CAP_E_NOENT;
    char nm[32], ext[8];
    if (aex_info_path(abs, nm, ext) != 0) return LOGIT_CAP_E_NOENT;

    uint64_t space = vmm_new_space();
    if (!space) return LOGIT_CAP_E_NOMEM;

    /* Load + build the stack with the new space active, exactly like
     * proc_spawn() above (the target space is not current yet, so the
     * kernel's own writes into it -- argv/envp/auxv -- must happen with it
     * switched in, and switched back out before anything else can run). */
    int fh = pcache_file_open(abs);           /* outside the cli: see proc_spawn */
    uint64_t prev;
    __asm__ volatile ("cli");
    __asm__ volatile ("mov %%cr3, %0" : "=r"(prev));
    vmm_switch(space);
    struct elf_image ei;
    uint64_t entry = aex_load_path(abs, (uint64_t)sz, nm, ext, &ei, fh) == 0 ? ei.entry : 0;
    uint64_t sp = entry ? setup_cli_stack(space, &ei, abs, cs_argv, argc, 0, 0) : 0;
    vmm_switch(prev);
    __asm__ volatile ("sti");
    if (fh >= 0) pcache_file_put(fh);
    if (entry) exec_note_load(abs, &ei);
    if (!entry || !sp) { vmm_free_space(space); return LOGIT_CAP_E_NOENT; }

    struct proc *child = proc_create(space, NULL, nm, p->pid);
    if (!child) { vmm_free_space(space); return LOGIT_CAP_E_NOMEM; }
    scopy(child->cwd, p->cwd, sizeof child->cwd);
    /* The grant: exactly `req`, never a copy of the caller's own set -- the
     * whole point proven above is that `req` is already <= that set. */
    child->caps = req.caps;
    scopy(child->fs_prefix, req.prefix, sizeof child->fs_prefix);

    for (int i = 0; i < NFD; i++) {           /* inherit the caller's fds, like SYS_FORK */
        child->fd[i] = p->fd[i];
        if (child->fd[i]) file_dup(child->fd[i]);
    }

    child->tid = thread_create_user(nm, entry, sp, child, space);
    if (child->tid < 0) {                     /* OOM: same undo shape as proc_fork()/proc_spawn() */
        for (int i = 0; i < NFD; i++)
            if (child->fd[i]) { file_close(child->fd[i]); child->fd[i] = NULL; }
        vmm_free_space(space);
        child->state = PROC_FREE; child->pid = 0; child->cr3 = 0;
        return LOGIT_CAP_E_NOMEM;
    }
    return child->pid;
}
