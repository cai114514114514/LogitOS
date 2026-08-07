#include <stdint.h>
#include <stddef.h>
#include "mm.h"
#include "vma.h"
#include "vmm.h"
#include "pmm.h"
#include "sched.h"
#include "usercopy.h"
#include "logit_abi.h"

/* The userland face of memory management: mmap, munmap, meminfo.
 *
 * It lives here, in one function, rather than as three cases inside
 * syscall.c's switch, for two reasons. The obvious one is ownership -- this
 * line owns c/kernel/mm/ and not c/kernel/exec/syscall.c, so the change that
 * file needs is a single three-line case that forwards, not a body that has to
 * be reviewed by whoever owns the dispatcher. The better one is that the
 * argument validation for an mmap ABI (page rounding, ranges that wrap, ranges
 * that leave the user region) belongs next to the code that enforces those
 * bounds, not next to forty unrelated syscalls.
 *
 * REQUIRED IN c/kernel/exec/syscall.c (not this line's file):
 *
 *     #include "mm.h"
 *     ...
 *     case SYS_MMAP:
 *     case SYS_MUNMAP:
 *     case SYS_MEMINFO:
 *         r->rax = (uint64_t)mm_syscall((long)r->rax, (long)r->rdi,
 *                                       (long)r->rsi, (long)r->rdx);
 *         return;
 *
 * Numbers 92/93/94, taken after re-reading include/abi/logit_abi.h; 91 was the
 * highest in use (the audio line's SYS_SND_STATE). */

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
        if (!a) return -1;
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
