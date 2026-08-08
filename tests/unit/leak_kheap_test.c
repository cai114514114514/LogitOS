/* leak_kheap_test -- does the KERNEL HEAP give memory back?
 *
 * The question this exists to answer, and why frames could not answer it.
 * -----------------------------------------------------------------------
 * `pmm` is auditable: pmm_audit() re-derives every counter and pmm_bugs()
 * counts every illegal transition, so a frame leak in the frame allocator is
 * loud. The kernel heap is a DIFFERENT allocator with a DIFFERENT failure mode:
 * kmalloc takes frames from pmm in 4 MiB arenas and NEVER returns them. So a
 * kheap that cannot reuse what it already holds consumes physical memory
 * forever while pmm's invariants stay perfectly intact and pmm_audit() stays
 * clean. In `free frames` the two are indistinguishable. The number that tells
 * them apart is kheap's own arena_bytes, and this test is about its slope.
 *
 * THE WORKLOAD is one GUI app open/close cycle, in miniature and on the host:
 *
 *   open   kmalloc the .aex load image (megabytes) and the window surface
 *          (cw*ch*4 -- megabytes at device resolution)
 *   run    kmalloc a handful of medium buffers the app's life needs (a pipe
 *          ring, a file buffer, a TCP reassembly ring) and HOLD them
 *   close   kfree the surface and the image; release the medium buffers
 *
 * Every one of those payloads is over 2048 bytes, so every one of them lands
 * in bin 8 -- the catch-all size class. That is the whole story: kmalloc served
 * a request from the first block in bin 8 that was big enough, WHOLE. A 16 KiB
 * file buffer would take a 3 MiB freed window surface and hold all 3 MiB for
 * its lifetime; the next window surface then found nothing that fit and grow()
 * took another 4 MiB of frames that pmm would never see again.
 *
 * THE ASSERTION is a slope, not a number: after a warm-up the arena must stop
 * growing, and the steady-state cycles must add ZERO bytes of arena. A test
 * that asserted "arena < some MiB" would pass on a leak that is merely slower
 * than the test is long.
 *
 * THE NEGATIVE CONTROL is compiled, not imagined: -DKHEAP_NO_SPLIT restores
 * the old whole-block reuse, and tests/unit/leak_run.sh requires this same
 * binary to FAIL when built with it. An assertion nobody has watched fail is
 * not known to be able to.
 *
 * Build/run: sh tests/unit/leak_run.sh   (or `make test-leak`)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"
#include "kheap.h"

#define MiB (1024ull * 1024ull)

/* Sizes taken from the real machine, not invented. The Dock does not launch one
 * app over and over -- the WM's own churn stress rotates Terminal / Clock /
 * Monitor, and each has a DIFFERENT .aex size and a DIFFERENT window, so each
 * open/close cycle puts a differently-sized block on the free list. That
 * heterogeneity is not a detail of the test, it is the mechanism: a uniform
 * workload reuses its own blocks exactly and hides the bug completely. */
struct appsz { unsigned image, surface; const char *name; };
static const struct appsz APPS[] = {
    { 3000u * 1024u, 1024u * 711u * 4u, "browser" },   /* 3.0 MB aex, 2.8 MiB window */
    {  180u * 1024u,  420u * 300u * 4u, "clock"   },   /* small aex, small window    */
    {  700u * 1024u,  760u * 520u * 4u, "terminal"},   /* middling both              */
};
#define NAPPS ((int)(sizeof APPS / sizeof APPS[0]))

/* Buffers with a lifetime longer than one window: a pipe ring, a VFS file
 * buffer, a TCP reassembly ring (64 KiB, net/transport/tcp.c). All over 2048
 * bytes, so all of them land in bin 8 alongside the multi-megabyte ones. */
#define NMED        6
static const unsigned med_sz[NMED] = { 4096, 8192, 16384, 65536, 32768, 4096 };

static unsigned long long arena(void)
{
    struct kheap_stats s; kheap_get_stats(&s); return s.arena_bytes;
}

/* One app open/close cycle. `hold` carries the medium buffers over into the
 * NEXT cycle before being released -- they have to be alive at the moment the
 * next window is allocated, because that overlap is when a non-splitting
 * allocator hands a 2.8 MiB block to a 4 KiB request and then has nothing left
 * for the window. */
static int cycle(void *hold[NMED], int k)
{
    const struct appsz *a = &APPS[k % NAPPS];
    void *img  = kmalloc(a->image);
    void *surf = kmalloc(a->surface);
    if (!img || !surf) return 0;
    memset(img, 0x5A, a->image);            /* touch it: ASan/UBSan want real writes */
    memset(surf, 0x3C, a->surface);

    for (int i = 0; i < NMED; i++) {
        if (hold[i]) kfree(hold[i]);        /* last cycle's, released mid-flight */
        hold[i] = kmalloc(med_sz[i]);
        if (!hold[i]) return 0;
        memset(hold[i], i, med_sz[i]);
    }

    kfree(img);                             /* wm_launch drops the load buffer... */
    kfree(surf);                            /* ...and reap() drops the surface     */
    return 1;
}

int main(void)
{
    /* 256 MiB of simulated RAM: half the machine, and enough that a leak has
     * room to be a slope rather than an immediate allocation failure. */
    mm_sim_init(256);

    void *hold[NMED] = { 0 };

    /* --- warm-up. The first cycles legitimately grow the arena: nothing has
     * been freed yet, so there is nothing to reuse. This is the settling the
     * measurement has to exclude, exactly as run-mm-test.sh throws its first
     * fork batch away. */
    const int WARM = 3 * NAPPS;             /* whole number of rotations */
    for (int i = 0; i < WARM; i++)
        mm_ok(cycle(hold, i), "warm-up cycle %d completed", i);
    unsigned long long a_warm = arena();

    /* --- the measurement. Twenty more identical cycles. Identical work on a
     * settled allocator must cost zero additional arena. */
    const int N = 20 * NAPPS;               /* whole number of rotations again */
    for (int i = 0; i < N; i++)
        mm_ok(cycle(hold, WARM + i), "steady-state cycle %d completed", i);
    unsigned long long a_end = arena();

    long long slope = (long long)(a_end - a_warm) / N;
    printf("  arena after warm-up %llu KiB, after %d more cycles %llu KiB"
           " (%lld bytes/cycle)\n",
           a_warm / 1024, N, a_end / 1024, slope);

    /* THE assertion. Zero, not "small": every cycle frees exactly what it
     * allocated, so a steady-state allocator that needs even one more byte on
     * the hundredth cycle than on the fifth needs an unbounded number by the
     * ten-thousandth. The tolerance is zero because the workload is exactly
     * periodic -- there is no sampling noise on the host to absorb. */
    mm_ok(a_end == a_warm,
          "arena stopped growing: %llu KiB after warm-up, %llu KiB after %d "
          "identical cycles (leak of %lld bytes/cycle)",
          a_warm / 1024, a_end / 1024, N, slope);

    /* Live bytes must come back to the same place too -- an arena that stopped
     * growing because the allocator started FAILING would also look flat. */
    struct kheap_stats s; kheap_get_stats(&s);
    printf("  live %llu KiB in %llu blocks, free %llu KiB, over-allocated %llu KiB"
           " of %llu KiB served, %llu allocs / %llu frees / %llu grows /"
           " %llu splits / %llu merges\n",
           s.live_bytes / 1024, s.live_blocks, s.free_bytes / 1024,
           (s.served_bytes - s.req_bytes) / 1024, s.served_bytes / 1024,
           s.allocs, s.frees, s.grows, s.splits, s.merges);
    mm_ok(s.allocs == s.frees + NMED,
          "every allocation but the %d still-held buffers was freed (%llu allocs, %llu frees)",
          NMED, s.allocs, s.frees);

    /* The six held buffers total 130 KiB and nothing else is live, so a heap
     * that is doing its job holds about that much -- not megabytes. This is the
     * assertion that catches whole-block reuse, where a 4 KiB request is served
     * out of a 2.8 MiB window surface and holds all of it. */
    unsigned long long med_total = 0;
    for (int i = 0; i < NMED; i++) med_total += med_sz[i];
    mm_ok(s.live_bytes < med_total + 16 * 1024,
          "the %llu KiB of live buffers cost %llu KiB of heap, not megabytes",
          med_total / 1024, s.live_bytes / 1024);

    /* Over-allocation is the mechanism behind both failures, so assert on it
     * directly and not only on its consequences. With splitting it can only be
     * the 16-byte alignment rounding plus a sub-SPLIT_MIN tail per block. */
    unsigned long long over = s.served_bytes - s.req_bytes;
    mm_ok(over < s.served_bytes / 20,
          "the allocator spent %llu KiB to serve %llu KiB of requests (< 5%% over)",
          s.served_bytes / 1024, s.req_bytes / 1024);
    (void)over;

    /* Coalescing has to actually be happening -- if it never fired, the flat
     * arena above would be luck rather than the mechanism being tested. */
    mm_ok(s.merges > 0, "frees coalesced with their neighbours (%llu merges)", s.merges);

    /* The frame allocator must be clean throughout -- this test is about the
     * heap, and a pmm bug here would mean the wrong thing had been measured. */
    mm_eqi((long long)pmm_bugs(), 0, "pmm invariant violations");
    mm_eqi(pmm_audit(), 0, "pmm audit errors");

    for (int i = 0; i < NMED; i++) kfree(hold[i]);
    mm_sim_done();
    return mm_summary("leak_kheap_test");
}
