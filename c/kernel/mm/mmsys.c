#include <stdint.h>
#include <stddef.h>
#include "mm.h"
#include "vma.h"
#include "vmm.h"
#include "pmm.h"
#include "rmap.h"
#include "swap.h"
#include "reclaim.h"
#include "sched.h"
#include "usercopy.h"
#include "kprintf.h"
#include "logit_abi.h"
#include "proc.h"      /* SYS_MMAP_FILE: proc_current()/proc_fd_get() to turn an fd into a path */
#include "file.h"      /* struct file, F_VFS, ->path -- see the case for why an fd has one */
#include "pcache.h"    /* SYS_MMAP_FILE: pcache_file_open()/pcache_file_put() */

/* Diagnostics selected by SYS_MEMINFO with a NULL buffer. Deliberately defined
 * here and not in include/abi/logit_abi.h: that header is the cross-cutting
 * kernel<->user ABI and belongs to everybody, whereas these four numbers are a
 * private door into c/kernel/mm/ that only its own tests use. */
#define MMCTL_REPORT   1
#define MMCTL_AUDIT    2
#define MMCTL_RECLAIM  3
#define MMCTL_STATS    4

/* The userland face of memory management: mmap (anonymous AND file-backed),
 * munmap, meminfo.
 *
 * It lives here, in one function, rather than as cases inside syscall.c's
 * switch, for two reasons. The obvious one is ownership -- this line owns
 * c/kernel/mm/ and not c/kernel/exec/syscall.c, so the change that file needs
 * is a forwarding case, not a body that has to be reviewed by whoever owns the
 * dispatcher. The better one is that the argument validation for an mmap ABI
 * (page rounding, ranges that wrap, ranges that leave the user region) belongs
 * next to the code that enforces those bounds, not next to forty unrelated
 * syscalls.
 *
 * REQUIRED IN c/kernel/exec/syscall.c (not this line's file):
 *
 *     #include "mm.h"
 *     ...
 *     case SYS_MMAP:
 *     case SYS_MMAP_FILE:
 *     case SYS_MUNMAP:
 *     case SYS_MEMINFO:
 *         r->rax = (uint64_t)mm_syscall((long)r->rax, (long)r->rdi,
 *                                       (long)r->rsi, (long)r->rdx);
 *         return;
 *
 * SYS_MMAP_FILE (162, include/abi/logit_abi.h) reuses this exact three-long
 * forwarding shape -- its one argument is a user pointer to a
 * logit_mmap_file_req, passed in `a` (rdi) same as every other single-struct
 * syscall in this ABI (SYS_IMG_DECODE, SYS_GUI_BLIT, ...), so syscall.c's
 * dispatch line needed nothing beyond the case label itself. Numbers 92/93/94
 * were taken after re-reading include/abi/logit_abi.h when SYS_MMAP/MUNMAP/
 * MEMINFO were added (91 was then the highest in use); 162 likewise, against
 * 161 (SYS_CAP_QUERY) being the highest at the time this was added. */

void *memset(void *, int, size_t);

long mm_syscall(long num, long a, long b, long c)
{
    uint64_t cr3 = sched_current_cr3();
    if (!cr3) return -1;

    switch (num) {
    case SYS_MMAP: {
        uint64_t len = (uint64_t)a;
        int prot = (int)b;
        uint64_t hint = (uint64_t)c;
        uint32_t p = 0;
        if (prot & MMAP_PROT_READ)  p |= VMA_READ;
        if (prot & MMAP_PROT_WRITE) p |= VMA_WRITE;
        if (prot & MMAP_PROT_EXEC)  p |= VMA_EXEC;
        if (!p) p = VMA_READ;                    /* PROT_NONE is not offered; readable is
                                                  * the floor, since a reservation nobody
                                                  * may touch is indistinguishable from
                                                  * not reserving it */
        /* Reserving costs nothing but address space -- the frames arrive on the
         * first touch, through the same fault path a copy-on-write page does. */
        return (long)vma_reserve(cr3, hint, len, p);
    }

    /* File-backed mmap. Connection #2 of the page cache (c/kernel/mm/pcache.h):
     * vma_reserve_file() is the ONLY producer of a file-backed VMA, and until
     * this case existed nothing ever called it, so MM_FAULT_FILE (fault.c) was
     * dead code -- reachable in the classifier's table, never in a real fault.
     *
     * WHY AN FD AND NOT A PATH. The struct carries `fd`, not a string, because
     * the access-control decision (may this process open this file at all) was
     * already made, once, at SYS_OPEN -- re-deciding it here from a bare path
     * would be a second, independent copy of that check with its own chance to
     * disagree. An fd is also already resolved to a canonical, absolute path:
     * file_open_vfs() (c/kernel/exec/file.c) stores exactly the string
     * SYS_OPEN's proc_resolve() produced, in f->path -- the very thing
     * pcache_file_open() needs to stat and, on a miss, read. */
    case SYS_MMAP_FILE: {
        struct logit_mmap_file_req req;
        if (user_copy_from(&req, (const void *)(uint64_t)a, sizeof req) < 0) return 0;

        /* READ-ONLY, ALWAYS -- refused OUT LOUD, not downgraded. See pcache.h's
         * "WHAT IS NOT HERE, DELIBERATELY" and the struct's own doc comment in
         * logit_abi.h: this is the one call in this switch that does not share
         * SYS_MMAP's "0 on failure" convention, on purpose, because "you asked
         * for something this kernel cannot ever do" is a different fact from
         * "there was no room right now" and a caller is entitled to tell them
         * apart. Checked BEFORE anything is opened or reserved -- a request this
         * kernel is going to refuse should not spend a pcache file slot first. */
        if (req.prot & MMAP_PROT_WRITE) return LOGIT_MMAP_FILE_E_WRITE;

        if (!req.len || (req.off & 0xFFF)) return 0;   /* zero length, or a byte
                                                        * offset that is not a
                                                        * page boundary -- vma.c's
                                                        * index arithmetic (foff/
                                                        * 4096) assumes it is */

        struct proc *p = proc_current();
        if (!p) return 0;
        struct file *f = proc_fd_get(p, req.fd);
        /* Only F_VFS names a real, path-addressed, on-disk file. F_PIPE/F_TTY/
         * F_SOCK have no backing path at all, and mapping one would mean
         * inventing an identity for pcache_file_open() to key on that the fd
         * does not actually have -- there is nothing to be honest about there,
         * so it is refused rather than guessed at (the same reasoning pcv_stat()
         * in pcache_vfs.c applies one layer down, for paths that resolve but
         * are not LogitFS regular files). */
        if (!f || f->type != F_VFS) return 0;

        int fh = pcache_file_open(f->path);
        if (fh < 0) return 0;              /* not cacheable, or the file table
                                             * is full -- pcache_file_open()
                                             * already said so on the console */

        uint32_t vp = 0;
        if (req.prot & MMAP_PROT_READ) vp |= VMA_READ;
        if (req.prot & MMAP_PROT_EXEC) vp |= VMA_EXEC;
        if (!vp) vp = VMA_READ;            /* PROT_NONE floor, same as SYS_MMAP */

        uint64_t base = vma_reserve_file(cr3, req.hint, req.len, vp, fh, req.off);
        /* vma_reserve_file() takes its OWN reference on `fh` when it succeeds
         * (vma.c: pcache_file_ref(fh), right before returning the base). Ours
         * from pcache_file_open() above is a second, transient one that exists
         * only to keep the entry alive across this call -- put it unconditionally,
         * on both the success and the failure path, so the mapping ends up
         * holding exactly the one reference the VMA owns, never zero (a
         * use-after-free the moment reclaim or a write invalidates it) and never
         * two (a reference nothing will ever put, because vma_release() only
         * ever puts what the AREA holds -- see vma.h's lifetime table). */
        pcache_file_put(fh);
        return (long)base;
    }

    case SYS_MUNMAP: {
        uint64_t start, end;
        if (vma_range((uint64_t)a, (uint64_t)b, &start, &end) < 0) return -1;
        /* Drop the reservation FIRST. If the unmap were first, a fault landing
         * between the two (this runs under the BKL, so it cannot -- but the
         * ordering should not depend on that) would re-fill a page that is
         * about to be thrown away. */
        if (vma_release(cr3, start, end - start) < 0) return -1;
        vmm_unmap_range_in(cr3, start, end - start);
        return 0;
    }

    case SYS_MEMINFO: {
        struct logit_meminfo mi;
        /* ---------------------------------------------------------------
         * a == 0 is the DIAGNOSTIC door, not an error.
         *
         * Reclaim's whole verification story is "assert that it actually ran,
         * by a counter the kernel keeps" -- and a counter nothing can read from
         * outside the kernel is not evidence. The on-device harness has to be
         * able to ask, from the shell, on a running machine, and the only
         * things it can run are /bin/sh and the programs on the disk.
         *
         * So a meminfo call with a NULL buffer selects a diagnostic with `b`
         * instead of copying a struct out. It rides on a syscall number that
         * already exists because adding one would mean editing
         * c/kernel/exec/syscall.c, which this line does not own.
         *
         *     as -e 'print(syscall(94, 0, 1, 0))'   -- print the mm report
         *
         * EXPOSURE, stated rather than hidden: this is reachable from any ring-3
         * process. MMCTL_AUDIT is O(total frames) and MMCTL_RECLAIM forces a
         * sweep, so an unprivileged program can make the kernel do bounded work
         * on demand. Neither can corrupt anything and neither can be made to run
         * unboundedly, but both belong behind the credentials check the
         * user/kernel-separation line is adding; when it lands, this is one
         * `if` away from being root-only.
         * --------------------------------------------------------------- */
        if (!a) {
            switch (b) {
            case MMCTL_REPORT:
                mm_report("on demand");
                return 0;
            case MMCTL_AUDIT: {
                /* Both structures, re-derived and compared. Returns the number
                 * of inconsistencies, so the harness asserts on 0 rather than
                 * on the absence of a string in a log. */
                int errs = pmm_audit() + rmap_audit();
                kprintf("[mm] audit: %d inconsistencies, %d allocator bugs, "
                        "%d reverse-map bugs, %d reclaim bugs\n",
                        errs, (int)pmm_bugs(), (int)rmap_bugs(), (int)reclaim_bugs());
                return errs;
            }
            case MMCTL_RECLAIM:
                /* Force a pass and say how many frames came back. `c` is how
                 * many are wanted; 0 asks for a nominal 64. */
                return (long)reclaim_frames(c ? (uint64_t)c : 64);
            case MMCTL_STATS: {
                /* One machine-readable line. The harness greps this rather than
                 * the prose report, so a change to the report's wording cannot
                 * silently break the test. */
                kprintf("[mmstat] free=%d total=%d evicted=%d dropped=%d swapped=%d "
                        "swapin=%d swapfail=%d second=%d slots=%d pins=%d "
                        "reserve_hits=%d allocfail=%d bugs=%d\n",
                        (int)pmm_free_frames(), (int)pmm_total_frames(),
                        (int)(reclaim_dropped() + reclaim_swapped()),
                        (int)reclaim_dropped(), (int)reclaim_swapped(),
                        (int)reclaim_swapins(), (int)reclaim_swapin_fail(),
                        (int)reclaim_second_chance(), (int)swap_slots_used(),
                        (int)pmm_pins_live(), (int)pmm_reserve_hits(),
                        (int)pmm_alloc_failures(),
                        (int)(pmm_bugs() + rmap_bugs() + reclaim_bugs()));
                return 0;
            }
            default:
                return -1;
            }
        }
        memset(&mi, 0, sizeof mi);
        mi.frame_bytes   = FRAME_SIZE;
        mi.frames_total  = pmm_total_frames();
        mi.frames_free   = pmm_free_frames();
        mi.frames_used   = pmm_used_frames();
        mi.frames_shared = pmm_shared_frames();
        mi.refs_total    = pmm_refs_total();
        mi.frames_pinned = pmm_pinned_frames();
        mi.cow_pages     = mm_cow_pages();
        mi.cow_faults    = mm_cow_faults();
        mi.cow_reuse     = mm_cow_reuse();
        mi.anon_faults   = mm_anon_faults();
        mi.mm_bugs       = pmm_bugs();
        mi.mmap_reserved = vma_reserved_bytes(cr3);
        if (user_copy_to((void *)(uint64_t)a, &mi, sizeof mi) < 0) return -1;
        return 0;
    }

    default:
        return -1;
    }
}
