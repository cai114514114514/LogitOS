#include <stdint.h>
#include <stddef.h>
#include "interrupts.h"
#include "proc.h"
#include "sched.h"
#include "file.h"
#include "vmm.h"
#include "pmm.h"
#include "aex.h"
#include "vfs.h"
#include "kheap.h"
#include "usercopy.h"
#include "aqua_abi.h"
#include "kprintf.h"

void *memcpy(void *, const void *, size_t);

#define MAXARG          48
#define ARGBUFSZ        4096          /* total bytes for all argv + envp strings */
#define CLI_STACK_PAGES 256           /* 1 MiB user stack for a CLI program */

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

/* Map a CLI user stack above the program image and build the SysV initial stack
 * (argc, argv[], NULL, envp[], NULL, strings). The target space MUST be active.
 * Returns the user rsp (points at argc, 16-aligned), or 0 on OOM. */
static uint64_t setup_cli_stack(uint64_t entry, char **argv, int argc, char **envp, int envc)
{
    uint64_t base = entry & ~(uint64_t)0xFFFFF;
    uint64_t top = base + 0x4000000;                 /* 64 MiB above base */
    for (int i = 1; i <= CLI_STACK_PAGES; i++) {
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
    if (argc < 0) return -1;
    int envc = copy_uvec((char **)r->rdx, envp, argstore, &used, ARGBUFSZ);
    if (envc < 0) return -1;

    /* 2. Load + validate the program image (kernel buffer) before destroying the
     *    old space, so a bad path/exec leaves the caller intact and returns -1. */
    int sz = vfs_size(abs);
    if (sz <= AEX_HDR_SIZE) return -1;
    int bytes = ((sz + 511) / 512) * 512;
    void *img = kmalloc((unsigned)bytes);
    if (!img) return -1;
    if (vfs_read(abs, img, bytes) <= 0) { kfree(img); return -1; }
    char nm[32], ext[8];
    if (aex_info(img, nm, ext) != 0) { kfree(img); return -1; }

    /* 3. Point of no return: swap the user address space. */
    uint64_t cr3 = p->cr3;
    vmm_free_user(cr3);
    uint64_t entry = aex_load(img, sz, nm, ext); /* maps into the active (p->cr3) space */
    kfree(img);
    if (!entry) proc_exit(127);                  /* image gone, can't recover */

    /* 4+5. Fresh user stack with the SysV argc/argv/envp layout. */
    uint64_t sp = setup_cli_stack(entry, argv, argc, envp, envc);
    if (!sp) proc_exit(127);

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
    if (sz <= AEX_HDR_SIZE) return -1;
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
    uint64_t entry = aex_load(img, sz, nm, ext);
    uint64_t sp = entry ? setup_cli_stack(entry, argv, argc, 0, 0) : 0;
    vmm_switch(prev);
    __asm__ volatile ("sti");
    kfree(img);
    if (!entry || !sp) { vmm_free_space(space); return -1; }

    struct proc *p = proc_create(space, NULL, nm, 0);
    if (!p) { vmm_free_space(space); return -1; }

    struct file *tty = file_open_tty();              /* fd 0/1/2 -> serial console */
    if (tty) { p->fd[0] = tty; file_dup(tty); p->fd[1] = tty; file_dup(tty); p->fd[2] = tty; }

    p->tid = thread_create_user(nm, entry, sp, p, space);
    return p->pid;
}
