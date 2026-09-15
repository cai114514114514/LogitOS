#ifndef LOGIT_UNIXSTUB_KHEAP_H
#define LOGIT_UNIXSTUB_KHEAP_H

/* Host stub for c/kernel/mm/phys/kheap.h, so c/net/core/unix.c compiles into the
 * white-box gate. Same shape as tests/unit/fsstub/kheap.h -- the kernel heap is
 * the host heap -- with TWO additions the AF_UNIX gate needs and the fs one
 * does not:
 *
 *   ustub_fail_alloc   makes the NEXT kmalloc fail. unix_connect() and
 *                      unix_bind() both allocate, and both have a rollback path
 *                      (t_exhaust asserts the client is not left marked
 *                      connected, and the datagram socket not left named). On a
 *                      host with 16 GiB those paths are unreachable by
 *                      accident, so they are unreachable by the test unless the
 *                      allocator can be told to refuse.
 *
 *   ustub_live_allocs  outstanding blocks. unix_test.c's last check is
 *                      `leak: no live allocations`, and a leak here is
 *                      invisible from every other check in the file: a socket
 *                      that never frees its `struct uconn` still moves every
 *                      byte correctly.
 *
 * REAL malloc/free underneath, deliberately, so the gate can be run under ASan
 * and see an overflow or a use-after-free that a counter cannot.
 *
 * WHY THIS FILE DID NOT EXIST UNTIL NOW: `UNIX_INC` in tests/net.mk has pointed
 * at -Itests/unit/unixstub since the AF_UNIX gate landed, and the directory was
 * never committed (`git ls-files tests/unit/ | grep -i unix` returned only
 * unix_test.c). `make test-unix` therefore died at unix.c:6 on a missing
 * kheap.h -- so the "132 checks and THREE controls" CLAUDE.md quotes had not
 * been run since. Written 2026-08-28 against unix.c and unix_test.c as they
 * stand; the counts in tests/net.mk's negative-control table were re-measured
 * against it rather than trusted. */

#include <stdlib.h>
#include <stddef.h>

extern int  ustub_fail_alloc;    /* 1 = the next kmalloc returns NULL */
extern long ustub_live_allocs;   /* blocks handed out and not yet kfree'd */

static inline void *kmalloc(size_t n)
{
    /* ONE-SHOT, not a mode: unix_connect() allocates exactly once, and a switch
     * that stayed on would make the rollback checks after it fail for the wrong
     * reason. The test clears the flag itself as well; clearing it here is what
     * makes the failure land on the call under test and nothing after it. */
    if (ustub_fail_alloc) { ustub_fail_alloc = 0; return NULL; }
    void *p = malloc(n);
    if (p) ustub_live_allocs++;
    return p;
}

static inline void kfree(void *p)
{
    /* kfree(NULL) is a no-op in c/kernel/mm/phys/kheap.c and must not be counted
     * here either, or a failed allocation's rollback would drive the leak
     * counter negative and the final check would pass by cancellation. */
    if (!p) return;
    ustub_live_allocs--;
    free(p);
}

#endif /* LOGIT_UNIXSTUB_KHEAP_H */
