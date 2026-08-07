#include <stdint.h>
#include <stddef.h>
#include "usercopy.h"
#include "sched.h"
#include "vmm.h"

void *memcpy(void *, const void *, size_t);

/* Kernel access to user memory, and the one place copy-on-write is genuinely
 * dangerous rather than merely fiddly.
 *
 * ------------------------------------------------------------------------
 * WHY THIS FILE CHANGED AT ALL
 *
 * user_range_ok(ptr, len, write=1) used to be a pure CHECK: it read the PTEs
 * and asked whether the WRITABLE bit was set, and its ~65 callers then wrote
 * to the buffer themselves -- some through user_copy_to(), most (SYS_READ,
 * SYS_PIPE, the net and GUI calls) by handing the raw user pointer to code
 * that writes it directly.
 *
 * Under copy-on-write a page that is logically writable is PHYSICALLY
 * read-only. So the old check has three possible behaviours and two of them
 * are wrong:
 *
 *   1. Leave it a pure check. Every legitimate write to a copy-on-write page
 *      is rejected, and read()/pipe()/recv() start failing with no diagnosis
 *      the moment a process forks. Wrong, and loudly so.
 *
 *   2. Relax the check and memcpy anyway. That takes a write fault in RING 0.
 *      In this kernel a ring-0 fault is fatal by design (interrupts.c:
 *      panic_exception halts the machine), so a forked shell reading from a
 *      pipe would kill the whole system. Wrong, and fatally so.
 *
 *   3. Relax it and arrange a writable mapping "somewhere" to write through,
 *      leaving the process's own mapping copy-on-write. That is DIRTY COW,
 *      CVE-2016-5195: the kernel's private writable view races the process's
 *      own copy-on-write resolution, and the write lands in the shared frame
 *      -- i.e. in memory the process was supposed to have been given a private
 *      copy of. Nine years in Linux, local privilege escalation.
 *
 * The correct shape is the fourth: RESOLVE the copy-on-write first, through
 * exactly the same code path a userspace write would take, so that when the
 * kernel writes, the process's own mapping is already private and writable.
 * There is one page table, one frame, and one resolution -- there is no second
 * view to race against. That is what vmm_user_range_fault_in() does, and why
 * user_range_ok() now calls it.
 *
 * ------------------------------------------------------------------------
 * WHY RESOLVE-THEN-COPY HAS NO WINDOW HERE
 *
 * Dirty COW is a TOCTOU: the check and the write are separated in time, and in
 * between, the mapping is reverted (madvise(MADV_DONTNEED)) so the write goes
 * to the shared frame. For that to be possible here, something would have to
 * turn the page back into a copy-on-write page between this call and the
 * caller's memcpy. Exactly three operations can do that -- fork
 * (vmm_clone_user), execve (vmm_free_user) and munmap -- and all three are
 * syscalls made BY THE PROCESS ITSELF. They cannot run in that window because:
 *
 *   (a) a proc has exactly one thread (struct proc::tid), so the process is not
 *       running in ring 3 while it is inside this syscall; and
 *   (b) another process cannot revert THIS space's PTEs -- fork only ever makes
 *       the FORKER's pages copy-on-write, and a different process writing its
 *       own copy of a shared frame allocates a new frame and never touches ours.
 *
 * So the check and the write are, in effect, atomic with respect to the page.
 * That is a property of the process model, not of this file, so it is asserted
 * where it can regress: the day a proc can have two threads, or a syscall can
 * unmap another process's memory, this becomes a real race and the resolve +
 * copy must move under a per-address-space lock held across both. Nothing else
 * in this file needs to change for that; the seam is deliberately here.
 * ------------------------------------------------------------------------ */

int user_range_ok(const void *ptr, uint64_t len, int write)
{
    /* Not a pure check any more: it makes the range usable, or fails. A
     * copy-on-write page is unshared and an untouched anonymous page is filled
     * BEFORE this returns 1, so every existing caller's plain memcpy (or its
     * direct write through the user pointer) is safe exactly as written.
     * Ranges that are not legitimately the process's still return 0, having
     * changed nothing -- vmm_user_range_fault_in only acts on the two cases
     * mm_fault_classify() is willing to name. */
    return vmm_user_range_fault_in(sched_current_cr3(), ptr, len, write);
}

int user_range_mapped(const void *ptr, uint64_t len, int write)
{
    /* The old pure-check semantics, for anyone who wants to ask a question
     * about the current mapping WITHOUT changing it (diagnostics, assertions).
     * Not what a syscall entry point wants. */
    return vmm_user_range_ok(sched_current_cr3(), ptr, len, write);
}

int user_copy_from(void *dst, const void *src, uint64_t len)
{
    if (!dst) return -1;
    if (len == 0) return 0;
    if (!user_range_ok(src, len, 0)) return -1;
    memcpy(dst, src, (size_t)len);
    return 0;
}

int user_copy_to(void *dst, const void *src, uint64_t len)
{
    if (!src) return -1;
    if (len == 0) return 0;
    if (!user_range_ok(dst, len, 1)) return -1;    /* resolves copy-on-write first */
    memcpy(dst, src, (size_t)len);
    return 0;
}

int user_copy_string(char *dst, int max, const char *src)
{
    if (!dst || max <= 0) return -1;
    for (int i = 0; i < max; i++) {
        if (!user_range_ok(src + i, 1, 0)) return -1;
        dst[i] = src[i];
        if (dst[i] == 0) return i;
    }
    dst[max - 1] = 0;
    return -1;
}
