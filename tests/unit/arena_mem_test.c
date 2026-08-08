/* arena_mem_test: what the mini-libc heap COSTS, as opposed to what it holds.
 *
 * THE THING BEING TESTED. c/apps/libc/src/malloc.c used to own its memory as
 * `static unsigned char arena[ARENA_SIZE]` -- a .bss array. On this machine a
 * .bss byte is not free: elf_load() (c/kernel/exec/elf.c) walks
 * [p_vaddr, p_vaddr+p_memsz) at load time and does pmm_alloc() + memset(0) for
 * every page in it. So the arena was fully resident before main() ran.
 * browser.aex shipped a 104.9 MiB .bss of which this one array was 96 MiB.
 *
 * It is now reserved with SYS_MMAP instead, so frames appear on first touch.
 * That change has two halves and this file tests both, because each has a
 * distinct way of being silently wrong:
 *
 *   1. THE RESERVATION must not be .bss. The measurable consequence is the
 *      linked object's .bss SIZE -- which is exactly what p_memsz is computed
 *      from, so it is the guest cost and not a proxy for it. Asserting on
 *      malloc's own high-water mark would NOT test this: occupancy is identical
 *      either way, and on Linux a .bss array is demand-paged too, so the host's
 *      own RSS cannot tell the two builds apart. The section size can.
 *
 *   2. THE BOUND must still refuse. This is the half that is easy to forget and
 *      worse than not doing the work at all. With a static array, running out
 *      of arena produced NULL, which every caller in this tree handles. With
 *      demand paging and no bound, malloc would return an address for memory
 *      the machine does not have and the program would die on the first WRITE,
 *      inside the fault handler, with nothing able to report it. Demand paging
 *      without a bound converts a clean NULL into a crash.
 *
 * NEGATIVE CONTROLS (run by tests/unit/arena_run.sh, both REQUIRED to fail):
 *   -DARENA_NO_MMAP    puts the static array back  -> check 1 fails
 *   -DARENA_NO_BOUND   removes the commit bound    -> check 2 fails
 * An assertion nobody has watched fail is not a known-failing assertion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* The allocator under test is linked into this binary under its real names, so
 * everything in the process -- including stdio's own buffers -- goes through
 * it. That is deliberate: it is how it runs on the machine. */
extern size_t malloc_hwm;
extern size_t malloc_peak;
extern int    malloc_arena_failed;
size_t malloc_arena_size(void);
size_t malloc_arena_limit(void);

/* DEFEATING DEAD-ALLOCATION ELIMINATION -- read this before editing any test
 * below. clang knows malloc() is an allocator, so at -O2 it will DELETE an
 * allocation whose result is never read, and a malloc/free pair around dead
 * memory disappears entirely. The first version of this file did exactly that:
 * it "handed out" 200 MiB while the allocator was never called once, and it
 * reported the bound as broken when the bound was fine. A memory test whose
 * allocations the optimiser can prove dead measures nothing at all.
 *
 * So every block allocated here is written to AND read back into `sink`, which
 * is volatile. That makes the memory observable, which makes the allocation
 * load-bearing, which is the only reason any number below means anything. */
static volatile unsigned long sink;

static void touch(void *p, size_t n, unsigned char pat)
{
    if (!p) return;
    memset(p, pat, n);
    sink += ((unsigned char *)p)[0] + ((unsigned char *)p)[n - 1];
}

static int fails;
static void ck(int cond, const char *fmt, ...)
{
    va_list ap;
    printf(cond ? "  ok   " : "  FAIL ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    if (!cond) fails++;
}

/* ---- 1: the reservation is not .bss --------------------------------------
 * ARENA_BSS_BYTES is measured at build time from the compiled malloc.o and
 * passed in on the command line -- a number read off the object file, not one
 * this program can talk itself into. */
#ifndef ARENA_BSS_BYTES
#error "build must pass -DARENA_BSS_BYTES=<.bss of malloc.o>"
#endif
#ifndef ARENA_BUILD_SIZE
#error "build must pass -DARENA_BUILD_SIZE=<-DARENA_SIZE it used>"
#endif

static void test_not_bss(void)
{
    printf("\n--- 1: the heap is reserved, not linked into .bss ---\n");
    printf("  built with ARENA_SIZE = %llu MiB\n",
           (unsigned long long)ARENA_BUILD_SIZE >> 20);
    printf("  malloc.o .bss        = %llu bytes\n",
           (unsigned long long)ARENA_BSS_BYTES);

    /* The allocator's own state is a few hundred bytes: bins[56], the arena
     * pointer, the counters. Anything on the order of the arena means the
     * array is back. 64 KiB is far above the real figure (~304 B) and far
     * below any arena worth having, so it separates the two builds without
     * being a tripwire on the exact size of the bookkeeping. */
    ck((unsigned long long)ARENA_BSS_BYTES < 65536ull,
       ".bss of the allocator is %llu B, not the %llu MiB arena",
       (unsigned long long)ARENA_BSS_BYTES,
       (unsigned long long)ARENA_BUILD_SIZE >> 20);

    ck(!malloc_arena_failed, "the reservation succeeded");
    ck(malloc_arena_size() >= (1u << 20), "usable arena is %zu MiB",
       malloc_arena_size() >> 20);
}

/* ---- 2: occupancy tracks use, and the bound refuses ----------------------- */

static void test_occupancy(void)
{
    printf("\n--- 2: occupancy tracks what is live, not what is reserved ---\n");

    size_t before = malloc_hwm;
    enum { N = 4096, SZ = 4096 };
    void **v = malloc(N * sizeof *v);
    ck(v != NULL, "control allocation of the pointer table");
    for (int i = 0; i < N; i++) {
        v[i] = malloc(SZ);
        touch(v[i], SZ, 0xA5);                  /* touch it: this is the cost */
    }
    size_t held = malloc_hwm;
    ck(held > before, "high-water rose while %d x %d B were live", N, SZ);
    ck(held <= before + (size_t)N * (SZ + 64) + (1u << 20),
       "high-water %zu KiB is close to the %d KiB actually asked for",
       held >> 10, (N * SZ) >> 10);

    for (int i = 0; i < N; i++) free(v[i]);
    free(v);

    /* Freeing does not lower the high-water mark -- those pages stay resident,
     * which is the honest accounting: the process has touched them. What must
     * hold is that re-allocating the same shape does not push it up again,
     * because that is the fragmentation failure kheap.c documents (splitting
     * without coalescing made it strictly worse, 2.7 MB a cycle, unbounded). */
    size_t after_free = malloc_hwm;
    for (int cycle = 0; cycle < 8; cycle++) {
        void **w = malloc(N * sizeof *w);
        if (!w) break;
        for (int i = 0; i < N; i++) { w[i] = malloc(SZ); touch(w[i], SZ, 0x5A); }
        for (int i = 0; i < N; i++) free(w[i]);
        free(w);
    }
    ck(malloc_hwm == after_free,
       "8 more alloc/free cycles of the same shape added 0 KiB (%zu KiB both times)",
       malloc_hwm >> 10);
}

static void test_bound(void)
{
    printf("\n--- 3: the bound refuses, rather than handing out a crash ---\n");
    printf("  commit bound = %zu MiB of a %zu MiB reservation\n",
           malloc_arena_limit() >> 20, malloc_arena_size() >> 20);

    ck(malloc_arena_limit() <= malloc_arena_size(),
       "the bound is inside the reservation");

    /* Walk past the bound in 1 MiB steps, holding everything, and require a
     * NULL before the arena is exhausted. Each block is WRITTEN to, so if the
     * bound were not enforced on the machine this loop is what would take the
     * process down in the fault handler instead of returning NULL here. */
    size_t lim = malloc_arena_limit();
    int got_null = 0;
    size_t handed = 0;
    enum { MAXHOLD = 4096 };
    static void *hold[MAXHOLD];                  /* held, and read back below */
    int nheld = 0;
    for (int i = 0; i < MAXHOLD; i++) {
        void *p = malloc(1u << 20);
        if (!p) { got_null = 1; break; }
        hold[nheld++] = p;
        touch(p, 1u << 20, (unsigned char)(i + 1));
        handed += 1u << 20;
        if (handed > lim + (8u << 20)) break;    /* ran away: the bound did nothing */
    }
    /* Read every block back. Nothing above is dead, so nothing above is
     * removable, so `handed` is a count of real allocations. */
    for (int i = 0; i < nheld; i++) sink += ((unsigned char *)hold[i])[0];
    for (int i = 0; i < nheld; i++) free(hold[i]);
    ck(got_null, "malloc returned NULL at the bound after handing out %zu MiB",
       handed >> 20);
    ck(handed <= lim + (2u << 20),
       "it stopped near the %zu MiB bound, not past it (%zu MiB handed out)",
       lim >> 20, handed >> 20);

    /* And the failure is reportable: the caller can say how big the heap is and
     * how much of it it was allowed, which is what a browser needs in order to
     * print "this page needs more memory than I have" instead of vanishing. */
    ck(malloc_arena_size() > 0 && malloc_arena_limit() > 0,
       "the sizes behind that refusal are readable (%zu / %zu MiB)",
       malloc_arena_limit() >> 20, malloc_arena_size() >> 20);
}

int main(void)
{
    printf("arena_mem_test: the cost of the mini-libc heap\n");
    test_not_bss();
    test_occupancy();
    test_bound();
    printf("\npeak live %zu KiB, high-water %zu KiB, reservation %zu MiB\n",
           malloc_peak >> 10, malloc_hwm >> 10, malloc_arena_size() >> 20);
    if (fails) { printf("arena_mem_test: %d FAILED\n", fails); return 1; }
    printf("arena_mem_test: ALL PASS\n");
    return 0;
}
