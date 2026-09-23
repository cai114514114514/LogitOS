/* Cross-core double-free contract for the production heap and ticket locks.
 * A per-CPU magazine scan can reject the second free on the same CPU while
 * silently parking the same block on two different CPUs. Check the returned
 * addresses, then drain: a duplicate queue entry can also corrupt the bins. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kheap.h"
#include "pmm.h"

#define HOST_RAM (64u * 1024u * 1024u)

uint64_t mm_host_base, mm_host_kend;
_Thread_local uint64_t mm_host_cr3;
static _Thread_local int cpu;
static uint64_t next_phys = 4096;
static atomic_int warnings;
static atomic_int race_ready, race_go;
static void *race_ptr;
static int failures;

int kheap_cpu_index(void) { return cpu; }
int logit_lock_host_cpu(void) { return cpu; }
void tlb_service(void) {}
void serial_putc(char c) { (void)c; }

void kprintf(const char *fmt, ...)
{
    if (strstr(fmt, "double free")) atomic_fetch_add(&warnings, 1);
}

uint64_t pmm_alloc_contig(size_t frames)
{
    size_t bytes = frames * 4096;
    if (frames > HOST_RAM / 4096 || next_phys > HOST_RAM - bytes) return 0;
    uint64_t phys = next_phys;
    next_phys += bytes;
    return phys;
}

uint64_t pmm_alloc_contig_masked(size_t frames, uint64_t mask,
                                 size_t align, size_t boundary)
{
    (void)mask; (void)align; (void)boundary;
    return pmm_alloc_contig(frames);
}

static void check(int condition, const char *what)
{
    if (condition) return;
    printf("FAIL %s\n", what);
    failures++;
}

static void drain_and_check(void)
{
    /* Too large for the stub RAM: allocation must drain every magazine before
     * PMM refusal. Checking bins afterward catches duplicate queue entries
     * that happen not to be the next two allocations. */
    check(kmalloc(128u * 1024u * 1024u) == NULL, "oversized allocation refuses");
    struct kheap_stats stats;
    kheap_get_stats(&stats);
    check(stats.magazine_bytes == 0, "drain empties every CPU magazine");
    check(stats.live_bytes == 0 && stats.live_blocks == 0,
          "drain releases each parked block exactly once");
    check(stats.free_bytes + 16 * stats.grows == stats.arena_bytes,
          "drain leaves each arena wholly accounted for");
}

static void *race_free(void *arg)
{
    cpu = (int)(uintptr_t)arg;
    atomic_fetch_add_explicit(&race_ready, 1, memory_order_release);
    while (!atomic_load_explicit(&race_go, memory_order_acquire)) sched_yield();
    kfree(race_ptr);
    return NULL;
}

static void free_on_two_cpus(void *ptr)
{
    pthread_t workers[2];
    race_ptr = ptr;
    atomic_store(&race_ready, 0);
    atomic_store(&race_go, 0);
    for (uintptr_t i = 0; i < 2; i++) {
        if (pthread_create(&workers[i], NULL, race_free, (void *)i)) {
            perror("pthread_create");
            exit(2);
        }
    }
    while (atomic_load_explicit(&race_ready, memory_order_acquire) != 2) sched_yield();
    atomic_store_explicit(&race_go, 1, memory_order_release);
    for (int i = 0; i < 2; i++) pthread_join(workers[i], NULL);
}

static void check_two_owners(size_t size)
{
    cpu = 0;
    void *first = kmalloc(size);
    cpu = 1;
    void *second = kmalloc(size);
    check(first && second && first != second,
          "cross-core double free never returns one block to two owners");
    if (first) { cpu = 0; kfree(first); }
    if (second && second != first) { cpu = 1; kfree(second); }
}

int main(void)
{
    void *ram = aligned_alloc(4096, HOST_RAM);
    if (!ram) { perror("host RAM"); return 2; }
    mm_host_base = (uint64_t)(uintptr_t)ram;

    /* Deterministic old-code failure: each core's local scan sees one entry. */
    cpu = 0;
    void *single = kmalloc(32);
    if (!single) return 2;
    kfree(single);
    cpu = 1;
    kfree(single);
    check(atomic_load(&warnings) == 1, "cross-core duplicate free is refused");
    check_two_owners(32);
    if (failures) return 1; /* Do not drain deliberately corrupted control bins. */
    drain_and_check();

    /* Race the same pointer on two real host threads, with different CPU IDs. */
    cpu = 0;
    void *raced = kmalloc(64);
    if (!raced) return 2;
    free_on_two_cpus(raced);
    check(atomic_load(&warnings) == 2, "concurrent duplicate free is refused once");
    check_two_owners(64);
    if (failures) return 1;
    drain_and_check();

    /* One core's magazine is full, so its kfree takes the slow path while the
     * other can still park the same block. The ownership claim must cover both. */
    cpu = 0;
    void *blocks[33];
    for (int i = 0; i < 33; i++) {
        blocks[i] = kmalloc(128);
        if (!blocks[i]) return 2;
    }
    for (int i = 0; i < 32; i++) kfree(blocks[i]);
    free_on_two_cpus(blocks[32]);
    check(atomic_load(&warnings) == 3, "full-magazine slow free is claimed once");
    check_two_owners(128);
    if (!failures) drain_and_check();

    printf("KHEAP_CROSSCORE warnings=%d failures=%d\n", atomic_load(&warnings), failures);
    free(ram);
    return failures ? 1 : 0;
}
