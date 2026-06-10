/* Host test for the kernel heap (c/kernel/mm/kheap.c, compiled against the
 * stub headers in kheapstub/). The one invariant that matters: NO TWO LIVE
 * ALLOCATIONS MAY OVERLAP (headers included) -- not even when pmm_alloc_contig
 * fails mid-grow. The deterministic scenario below reproduces the app-churn
 * freeze root cause: grow() pushing the bump-area leftover onto a free list
 * and THEN failing the arena allocation without retiring brk/brk_left, leaving
 * the leftover simultaneously free-listed and bump-allocatable.
 *
 * Run: make test-kheap */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "kheap.h"
#include "pmm.h"

void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* --- pmm stub: real aligned host memory + on-demand failure injection --- */
static int pmm_fail;            /* 0 = succeed; 1 = fail every call */
static int pmm_fail_pct;        /* random failure percentage (fuzz phase) */
static int pmm_calls, pmm_fails;

uint64_t pmm_alloc_contig(size_t n)
{
    pmm_calls++;
    if (pmm_fail || (pmm_fail_pct && rand() % 100 < pmm_fail_pct)) {
        pmm_fails++;
        return 0;
    }
    void *p = aligned_alloc(FRAME_SIZE, n * FRAME_SIZE);
    if (!p)
        return 0;
    memset(p, 0xCC, n * FRAME_SIZE);
    return (uint64_t)(uintptr_t)p;
}

/* --- live-allocation table + the overlap invariant --- */
#define MAXLIVE 4096
static struct { uint8_t *p; size_t sz; uint8_t pat; } live[MAXLIVE];
static int nlive, fails;

#define HDR 16   /* kheap block header size: footprint = [p-HDR, p+sz) */

static void check_overlap(uint8_t *p, size_t sz, const char *what)
{
    uint8_t *lo = p - HDR, *hi = p + sz;
    for (int i = 0; i < nlive; i++) {
        uint8_t *l2 = live[i].p - HDR, *h2 = live[i].p + live[i].sz;
        if (lo < h2 && l2 < hi) {
            printf("FAIL [%s] OVERLAP: new [%p,%p) vs live #%d [%p,%p) -- "
                   "two owners for one block (THE churn-freeze bug)\n",
                   what, (void *)lo, (void *)hi, i, (void *)l2, (void *)h2);
            fails++;
            return;
        }
    }
}

static void add_live(uint8_t *p, size_t sz, uint8_t pat)
{
    check_overlap(p, sz, "alloc");
    memset(p, pat, sz);
    live[nlive].p = p; live[nlive].sz = sz; live[nlive].pat = pat;
    nlive++;
}

static void free_live(int i)
{
    /* pattern intact = nobody else scribbled on us while we were live */
    for (size_t k = 0; k < live[i].sz; k++)
        if (live[i].p[k] != live[i].pat) {
            printf("FAIL pattern clobbered in live #%d at +%zu (got %02x want %02x)\n",
                   i, k, live[i].p[k], live[i].pat);
            fails++;
            break;
        }
    kfree(live[i].p);
    live[i] = live[--nlive];
}

int main(void)
{
    /* === Phase 1: deterministic repro of the grow()-failure double-accounting.
     * Carve the first 4 MiB arena down to a 30736-byte tail (16-byte header +
     * 30720 payload -- the exact leftover size observed in the QEMU freezes),
     * then make the next arena allocation fail. */
    uint8_t *big = kmalloc(4 * 1024 * 1024 - HDR - 30736);
    if (!big) { printf("FAIL phase1 setup alloc\n"); return 1; }
    add_live(big, 4 * 1024 * 1024 - HDR - 30736, 0xA1);

    pmm_fail = 1;
    uint8_t *toobig = kmalloc(65536);     /* > leftover, > any bin -> grow -> FAIL */
    if (toobig) { add_live(toobig, 65536, 0xA2); printf("note: 64K alloc unexpectedly ok\n"); }

    uint8_t *c = kmalloc(1024);           /* bump path (if brk survived the failed grow) */
    if (c) add_live(c, 1024, 0xA3);
    uint8_t *d = kmalloc(20000);          /* bin-8 first fit (if the leftover got pushed) */
    if (d) add_live(d, 20000, 0xA4);
    /* Pre-fix: c comes from the bump area AND d from the free list -- the SAME
     * 30736-byte leftover -> check_overlap() above fires. Post-fix: the leftover
     * was never pushed, c bump-allocates from it, d has no fitting block and no
     * arena -> NULL. Either allocation succeeding is fine; coexisting overlapping
     * ones are not. */
    printf("phase1: toobig=%s c=%s d=%s\n",
           toobig ? "ptr" : "NULL", c ? "ptr" : "NULL", d ? "ptr" : "NULL");
    pmm_fail = 0;

    /* === Phase 2: randomized alloc/free fuzz with a 30% arena-failure rate,
     * continuing on the same heap state. Sizes span every bin incl. the
     * oversized bin-8 class (pipes 8224, kstacks 32768, surfaces ~1 MiB). */
    srand(1234);
    pmm_fail_pct = 30;
    static const size_t SZ[] = { 24, 100, 512, 2048, 4096, 8224, 16384,
                                 30720, 32768, 65536, 909440 };
    for (int it = 0; it < 20000; it++) {
        if (nlive < MAXLIVE && (nlive == 0 || rand() % 100 < 55)) {
            size_t sz = SZ[rand() % (sizeof SZ / sizeof *SZ)];
            uint8_t *p = kmalloc(sz);
            if (p) add_live(p, sz, (uint8_t)(0x10 + (it & 0x7F)));
        } else {
            free_live(rand() % nlive);
        }
        if (fails > 5) { printf("(aborting early: %d failures)\n", fails); break; }
    }
    while (nlive) free_live(0);

    printf("pmm: %d calls, %d injected failures\n", pmm_calls, pmm_fails);
    printf(fails ? "\n%d kheap invariant FAILURES\n" : "\nall kheap invariants held\n", fails);
    return fails ? 1 : 0;
}
