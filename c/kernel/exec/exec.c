#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"
#include "proc.h"
#include "sched.h"
#include "file.h"
#include "vmm.h"
#include "pmm.h"
#include "vma.h"         /* the CLI stack is a RESERVATION, faulted in on touch */
#include "aex.h"
#include "vfs.h"
#include "kheap.h"
#include "usercopy.h"
#include "logit_abi.h"
#include "kprintf.h"

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
#define CLI_STACK_HEAD  (ARGBUFSZ + (2 * MAXARG + 3) * 8 + 32)
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
static uint64_t setup_cli_stack(uint64_t cr3, uint64_t entry, char **argv, int argc,
                                char **envp, int envc)
{
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

    for (int i = 1; i <= eager; i++) {
        uint64_t frame = pmm_alloc();
        if (!frame) return 0;
        vmm_map_page(top - (uint64_t)i * 0x1000, frame, VMM_WRITABLE | VMM_USER);
    }
    uint64_t sp = top, uargv[MAXARG], uenvp[MAXARG];
    for (int i = 0; i < argc; i++) { int l = kstrlen(argv[i]); sp -= l + 1; memcpy((void *)sp, argv[i], l + 1); uargv[i] = sp; }
    for (int i = 0; i < envc; i++) { int l = kstrlen(envp[i]); sp -= l + 1; memcpy((void *)sp, envp[i], l + 1); uenvp[i] = sp; }
    sp &= ~(uint64_t)0xF;
    int nslots = 1 + (argc + 1) + (envc + 1);
    sp -= (uint64_t)nslots * 8;
    sp &= ~(uint64_t)0xF;                            /* 16-align argc */
    uint64_t *st = (uint64_t *)sp; int k = 0;
    st[k++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) st[k++] = uargv[i];
    st[k++] = 0;
    for (int i = 0; i < envc; i++) st[k++] = uenvp[i];
    st[k++] = 0;
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

    /* 2. Load + validate the program image (kernel buffer) before destroying the
     *    old space, so a bad path/exec leaves the caller intact and returns -1. */
    int sz = vfs_size(abs);
    if (sz < AEX_HDR_SIZE) { kprintf("[execve] %s: missing/too small (%d)\n", abs, sz); return -1; }
    if (sz > 0x7fffffff - 511) return -1;    /* sz + 511 would overflow int */
    int bytes = ((sz + 511) / 512) * 512;
    void *img = kmalloc((unsigned)bytes);
    if (!img) { kprintf("[execve] %s: kmalloc %d failed\n", abs, bytes); return -1; }
    if (vfs_read(abs, img, bytes) <= 0) { kprintf("[execve] %s: read failed\n", abs); kfree(img); return -1; }
    char nm[32], ext[8];
    if (aex_info(img, nm, ext) != 0) { kprintf("[execve] %s: bad aex header\n", abs); kfree(img); return -1; }

    /* 3. Point of no return: swap the user address space. */
    kprintf("[execve] pid %d: %s loading\n", p->pid, abs);   /* DIAG: reached = child alive */
    uint64_t t_exec = exec_rdtsc();
    uint64_t cr3 = p->cr3;
    vmm_free_user(cr3);
    uint64_t entry = aex_load(img, (uint64_t)bytes, nm, ext, NULL);  /* maps into the active (p->cr3) space */
    kfree(img);
    uint64_t t_load = exec_rdtsc();
    if (!entry) { kprintf("[execve] %s: aex_load failed\n", abs); proc_exit(127); }

    /* 4+5. Fresh user stack with the SysV argc/argv/envp layout. */
    uint64_t sp = setup_cli_stack(cr3, entry, argv, argc, envp, envc);
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
    if (sz > 0x7fffffff - 511) return -1;    /* sz + 511 would overflow int */
    int bytes = ((sz + 511) / 512) * 512;
    void *img = kmalloc((unsigned)bytes);
    if (!img) return -1;
    if (vfs_read(path, img, bytes) <= 0) { kfree(img); return -1; }
    char nm[32], ext[8];
    if (aex_info(img, nm, ext) != 0) { kfree(img); return -1; }

    uint64_t space = vmm_new_space();
    if (!space) { kfree(img); return -1; }

    int argc = 0; while (argv && argv[argc]) argc++;

    /* Load + build the stack with the new space active (it isn't current yet). */
    uint64_t prev;
    __asm__ volatile ("cli");
    __asm__ volatile ("mov %%cr3, %0" : "=r"(prev));
    vmm_switch(space);
    uint64_t entry = aex_load(img, (uint64_t)bytes, nm, ext, NULL);
    uint64_t sp = entry ? setup_cli_stack(space, entry, argv, argc, 0, 0) : 0;
    vmm_switch(prev);
    __asm__ volatile ("sti");
    kfree(img);
    if (!entry || !sp) { vmm_free_space(space); return -1; }

    struct proc *p = proc_create(space, NULL, nm, 0);
    if (!p) { vmm_free_space(space); return -1; }

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
