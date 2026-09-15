/* /proc's source: the memory, time and address-space half.
 *
 * The process-table half is in c/kernel/exec/proc.c, beside the table and its
 * lock -- see the comment there. This file holds the three answers that come
 * from somewhere else, and it exists at all so that c/fs/procfs.c can include
 * NO KERNEL HEADER and therefore run in a host unit test unchanged (procfs.h,
 * "the source seam").
 *
 * Everything here is a read of a counter something else already maintains.
 * There is no accounting in this file and there must not be: /proc is a second
 * reader of facts, and a number computed here would be a number only /proc
 * knows, which is how two subsystems come to disagree about the same machine.
 *
 * NOT INCLUDED, on purpose: c/kernel/mm/swap.h and reclaim.h. /proc/meminfo
 * would be a natural place for the eviction counters, and reclaim.h is the
 * door to them -- but the block line has an in-flight edit in swap.c/swap.h
 * that does not compile under -Werror right now, and reclaim.h reaches it.
 * Adding those rows means including a header that is being rewritten, so the
 * rows are absent and named here instead of being half-added. They are three
 * lines in r_meminfo() and three fields in struct procfs_mem the day that
 * settles. */

#include <stdint.h>
#include <stddef.h>
#include "procfs.h"
#include "vma.h"
#include "pmm.h"
#include "kheap.h"
#include "ktime.h"

int procfs_src_area(int pid, int i, struct procfs_area *out)
{
    struct procfs_task t;
    /* THE LIFETIME RULE, in code: the process is looked up FIRST, every time,
     * and the address space is only reached through what that lookup returned.
     * A cr3 cached across calls would outlive the process by exactly as long
     * as somebody held the fd -- and vmm_free_space() would have handed those
     * page tables back to the PMM by then. */
    if (!procfs_src_task(pid, &t)) return -1;
    if (!out) return -1;

    struct vma v;
    if (!vma_nth(t.cr3, i, &v)) return 0;

    out->start = v.start;
    out->end   = v.end;
    out->foff  = v.foff;
    out->file  = v.file;
    out->shm   = v.shm;
    /* VMA_* and PROCFS_* are separately defined and translated here rather
     * than assumed equal. They happen to hold the same three bit values today;
     * an #if to prove it would be a compile-time assertion that c/fs must
     * never diverge from c/kernel/mm, which is a promise this seam exists to
     * NOT make. Three ands cost nothing. */
    out->prot = 0;
    if (v.prot & VMA_READ)  out->prot |= PROCFS_R;
    if (v.prot & VMA_WRITE) out->prot |= PROCFS_W;
    if (v.prot & VMA_EXEC)  out->prot |= PROCFS_X;
    return 1;
}

void procfs_src_mem(struct procfs_mem *out)
{
    if (!out) return;
    struct kheap_stats k;
    kheap_get_stats(&k);

    out->total_frames = pmm_total_frames();
    out->free_frames  = pmm_free_frames();
    out->used_frames  = pmm_used_frames();
    out->frame_bytes  = FRAME_SIZE;   /* pmm.h:7 -- the allocator's unit, not a guess */
    out->heap_arena   = k.arena_bytes;
    out->heap_live    = k.live_bytes;
    out->heap_free    = k.free_bytes;
    out->heap_allocs  = k.allocs;
    out->heap_frees   = k.frees;
    out->heap_grows   = k.grows;
    out->spaces_live  = vma_spaces_live();
}

/* MONOTONIC, not realtime. An uptime derived from the wall clock steps
 * whenever the wall clock is set, and c/kernel/core/ktime.h is explicit that
 * the monotonic clock is "the ONLY clock you may subtract". */
uint64_t procfs_src_uptime_ms(void) { return time_mono_ms(); }

/* THE VERSION STRING IS DUPLICATED, and this comment is the whole of the
 * apology. The other copy is c/apps/libc/src/uname.c:16-18, which is where
 * uname(2) gets "0.29" / "LogitOS M29 x86_64" -- in USERLAND, because until
 * this file existed the kernel never had to state its own version to anybody.
 * Two copies of one fact is a thing this tree argues against everywhere else,
 * and the fix is a single definition in include/abi/logit_abi.h that both
 * read; that header is contended and shared, so the honest move today is to
 * duplicate it LOUDLY rather than to quietly edit a file this line does not
 * own. If they ever disagree, /proc/version is the one that is wrong. */
const char *procfs_src_version(void)
{
    return "LogitOS version 0.29 (x86_64)";
}
