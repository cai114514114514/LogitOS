/* SPDX-License-Identifier: MIT */
/* Real production inline helpers, all supported CPU owners; no timing claim. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kbench.h"

struct kb_cpu g_kb[KB_MAXCPU];
struct kb_sys_cpu g_kb_sys[KB_MAXCPU];
int g_kb_stat;
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static unsigned ready, start;
static const unsigned slots[] = {0, 10, 189, 255};
#define RECORDS 10000u
static unsigned checks;
static void require(int ok, const char *message)
{
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static uint64_t sys_cycles(unsigned cpu, unsigned nr)
{ return (uint64_t)(cpu + 1) * 100 + nr + 1; }
static void *writer(void *arg)
{
    unsigned cpu = (unsigned)(uintptr_t)arg;
    pthread_mutex_lock(&start_mutex);
    ready++;
    pthread_cond_broadcast(&start_cond);
    while (!start) pthread_cond_wait(&start_cond, &start_mutex);
    pthread_mutex_unlock(&start_mutex);
    for (unsigned i = 0; i < RECORDS; i++) {
        unsigned nr = slots[i % 4];
        kb_sys_record(cpu, nr, sys_cycles(cpu, nr));
        kb_entry_record(cpu, i % KB_NCLASS, cpu + 11);
    }
    return NULL;
}
static uint64_t expected_count(unsigned nr)
{
    for (unsigned i = 0; i < 4; i++) if (slots[i] == nr) return RECORDS / 4;
    return 0;
}
int main(void)
{
    require(KB_MAXCPU == 32 && KB_NSYS == 256, "all CPU owners and all 256 ABI slots");
    require(sizeof(struct kb_sys_snapshot) == 4096, "snapshot occupies one page");
    for (unsigned c = 0; c < KB_MAXCPU; c++)
        require((uintptr_t)&g_kb_sys[c] % 64 == 0 && sizeof(g_kb_sys[c]) % 64 == 0,
                "CPU histograms have separate cache lines");
    pthread_t threads[KB_MAXCPU];
    for (unsigned c = 0; c < KB_MAXCPU; c++)
        require(pthread_create(&threads[c], NULL, writer, (void *)(uintptr_t)c) == 0,
                "create all CPU writer threads");
    pthread_mutex_lock(&start_mutex);
    while (ready != KB_MAXCPU) pthread_cond_wait(&start_cond, &start_mutex);
    start = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mutex);
    /* Concurrent report readers use the same real atomic helper. Values may
     * come from adjacent instants, so no count/cycle pair consistency is assumed. */
    struct kb_sys_snapshot live;
    for (unsigned i = 0; i < 16; i++) kb_sys_snapshot_read(&live);
    for (unsigned c = 0; c < KB_MAXCPU; c++)
        require(pthread_join(threads[c], NULL) == 0, "join every CPU writer");

    /* CPU1 is checked first: forcing every caller to CPU0 fails deterministically,
     * even on a host which schedules all eight threads without any lost update. */
    require(g_kb_sys[1].n[189] == RECORDS / 4,
            "each CPU owns its own syscall counters");
    for (unsigned c = 0; c < KB_MAXCPU; c++) {
        for (unsigned nr = 0; nr < KB_NSYS; nr++) {
            uint64_t n = expected_count(nr);
            require(g_kb_sys[c].n[nr] == n, "exact count in each CPU and ABI slot");
            require(g_kb_sys[c].cyc[nr] == n * sys_cycles(c, nr),
                    "exact cycles in each CPU and ABI slot");
        }
        for (unsigned cls = 0; cls < KB_NCLASS; cls++) {
            require(g_kb[c].n[cls] == RECORDS / KB_NCLASS, "exact per-CPU entry counts");
            require(g_kb[c].cyc[cls] == (RECORDS / KB_NCLASS) * (c + 11),
                    "exact per-CPU entry cycles");
        }
    }
    struct kb_sys_snapshot a, b, stable;
    kb_sys_snapshot_read(&a);
    kb_sys_snapshot_read(&b);
    require(memcmp(&a, &b, sizeof(a)) == 0, "repeated snapshots do not consume counters");
    for (unsigned nr = 0; nr < KB_NSYS; nr++) {
        uint64_t cyc = 0;
        for (unsigned c = 0; c < KB_MAXCPU; c++) cyc += expected_count(nr) * sys_cycles(c, nr);
        require(a.n[nr] == KB_MAXCPU * expected_count(nr), "snapshot exact aggregate counts");
        require(a.cyc[nr] == cyc, "snapshot exact aggregate cycles");
    }
    require(a.n[189] == 80000 && a.n[255] == 80000, "ABI189 and last slot are accounted");
    stable = b;
    memset(&a, 0, sizeof(a)); /* ranking may consume only this local copy */
    kb_sys_snapshot_read(&b);
    require(memcmp(&stable, &b, sizeof(b)) == 0, "local report sorting cannot clear live counters");
    struct kb_sys_cpu sys_before[KB_MAXCPU];
    struct kb_cpu entry_before[KB_MAXCPU];
    memcpy(sys_before, g_kb_sys, sizeof(sys_before));
    memcpy(entry_before, g_kb, sizeof(entry_before));
    kb_sys_record(KB_MAXCPU, 189, 42);
    kb_sys_record(~0u, 189, 42);
    kb_sys_record(0, KB_NSYS, 42);
    kb_sys_record(0, UINT64_MAX, 42);
    kb_entry_record(KB_MAXCPU, 0, 42);
    kb_entry_record(~0u, 0, 42);
    kb_entry_record(0, KB_NCLASS, 42);
    kb_entry_record(0, ~0u, 42);
    require(memcmp(sys_before, g_kb_sys, sizeof(sys_before)) == 0, "invalid CPU or ABI never writes");
    require(memcmp(entry_before, g_kb, sizeof(entry_before)) == 0, "invalid CPU or entry class never writes");
    __atomic_store_n(&g_kb_stat, 1, __ATOMIC_RELAXED);
    require(kb_stat_enabled(), "atomic enabled flag is visible");
    __atomic_store_n(&g_kb_stat, 0, __ATOMIC_RELAXED);
    require(!kb_stat_enabled(), "atomic disabled flag is visible");
    printf("bkl kbench: %u checks, 32 CPU writers, 320000 exact syscall records, PASS\n", checks);
    return 0;
}
