/* See mm_common.h. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "mm_common.h"
#include "pmm.h"

/* --- the seam (c/kernel/mm/mmhost.h) ------------------------------------ */
uint64_t mm_host_base;      /* host address of simulated physical 0 */
uint64_t mm_host_kend;      /* simulated "end of kernel image" */
uint64_t mm_host_cr3;       /* simulated CR3 */

void *mm_sim_ptr(uint64_t phys) { return (void *)(uintptr_t)(mm_host_base + phys); }

/* --- kprintf ------------------------------------------------------------ */
static int log_lines, log_bugs, log_quiet;
static char log_last[512];

void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(log_last, sizeof log_last, fmt, ap);
    va_end(ap);
    log_lines++;
    if (strstr(log_last, "BUG:") || strstr(log_last, "AUDIT:"))
        log_bugs++;
    if (!log_quiet)
        fputs(log_last, stdout);
}

int  mm_log_lines(void) { return log_lines; }
int  mm_log_bugs(void)  { return log_bugs; }
void mm_log_reset(void) { log_lines = log_bugs = 0; log_last[0] = 0; }
void mm_log_quiet(int q) { log_quiet = q; }
const char *mm_log_last(void) { return log_last; }

/* --- assertions --------------------------------------------------------- */
int mm_checks, mm_fails;

void mm_ok(int cond, const char *fmt, ...)
{
    mm_checks++;
    if (cond) return;
    mm_fails++;
    va_list ap; va_start(ap, fmt);
    fputs("  FAIL: ", stdout);
    vprintf(fmt, ap);
    fputs("\n", stdout);
    va_end(ap);
}

void mm_eqi(long long got, long long want, const char *what)
{
    mm_checks++;
    if (got == want) return;
    mm_fails++;
    printf("  FAIL: %s: got %lld, want %lld\n", what, got, want);
}

int mm_summary(const char *suite)
{
    printf("%s: %d checks, %d failures\n", suite, mm_checks, mm_fails);
    return mm_fails ? 1 : 0;
}

/* --- simulated physical memory ------------------------------------------ */
/* Layout of the simulated machine, chosen to mirror the real one closely
 * enough that pmm_init takes the same branches:
 *   0x00000000..0x0009FC00   available (low memory)
 *   0x00100000..end          available (the big region)
 *   kernel image ends at 0x00200000, so pmm parks its metadata there
 *   the Multiboot2 info block sits at 0x00180000 (inside the big region, below
 *   the metadata, so the "re-reserve the info block" path is exercised) */
#define SIM_KEND      0x00200000ull
#define SIM_MB_INFO   0x00180000ull

static void *sim_mem;

uint64_t mm_sim_init(unsigned mib)
{
    uint64_t bytes = (uint64_t)mib * 1024 * 1024;
    if (sim_mem) { free(sim_mem); sim_mem = NULL; }
    sim_mem = aligned_alloc(4096, (size_t)bytes);
    if (!sim_mem) { fprintf(stderr, "sim: out of host memory\n"); exit(2); }
    /* Non-zero fill: nothing may depend on simulated RAM starting zeroed. */
    memset(sim_mem, 0xA5, (size_t)bytes);
    mm_host_base = (uint64_t)(uintptr_t)sim_mem;
    mm_host_kend = SIM_KEND;
    mm_host_cr3  = 0;

    /* Multiboot2 info block: total_size, reserved, then tags. */
    uint8_t *info = mm_sim_ptr(SIM_MB_INFO);
    uint8_t *p = info + 8;

    struct { uint32_t type, size, entry_size, entry_version; } *mt = (void *)p;
    struct ment { uint64_t addr, len; uint32_t type, res; };
    mt->type = 6; mt->entry_size = sizeof(struct ment); mt->entry_version = 0;
    struct ment *e = (struct ment *)(mt + 1);
    e[0].addr = 0;         e[0].len = 0x9FC00;          e[0].type = 1; e[0].res = 0;
    e[1].addr = 0x100000;  e[1].len = bytes - 0x100000; e[1].type = 1; e[1].res = 0;
    mt->size = (uint32_t)(sizeof *mt + 2 * sizeof(struct ment));
    p += (mt->size + 7) & ~7u;

    struct { uint32_t type, size; } *endt = (void *)p;
    endt->type = 0; endt->size = 8;
    p += 8;

    uint32_t total = (uint32_t)(p - info);
    *(uint32_t *)info = total;
    *(uint32_t *)(info + 4) = 0;

    pmm_init(SIM_MB_INFO);
    return SIM_MB_INFO;
}

void mm_sim_done(void)
{
    free(sim_mem);
    sim_mem = NULL;
}
