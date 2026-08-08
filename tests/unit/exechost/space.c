#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>
#include "space.h"
#include "vmm.h"
#include "prot.h"

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* ---- the fake page table: open-addressed va -> pte ----------------------- */
#define NSLOT (1u << 20)
static uint64_t key[NSLOT];      /* va | 1 when occupied (va is page aligned) */
static uint64_t val[NSLOT];
/* The occupied slots, in insertion order. A fuzz run resets the space tens of
 * thousands of times, and sweeping a million slots each time is most of the
 * run; sweeping only the ones in use is none of it. */
#define NOCC (1u << 19)
static uint32_t occ[NOCC];
static uint32_t nocc;
static uint64_t g_used, g_budget = 400000;
static int g_nx = 0, g_quiet = 0, g_msgs;
static char g_last[512];

static uint32_t hsh(uint64_t va)
{
    va >>= 12;
    va *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(va >> 40) & (NSLOT - 1);
}

static uint64_t *slot(uint64_t va, int create)
{
    va &= ~0xFFFull;
    uint32_t i = hsh(va);
    for (uint32_t n = 0; n < NSLOT; n++, i = (i + 1) & (NSLOT - 1)) {
        if (key[i] == (va | 1)) return &val[i];
        if (!key[i]) {
            if (!create) return 0;
            if (nocc >= NOCC) { fprintf(stderr, "space: too many mapped pages\n"); exit(2); }
            key[i] = va | 1; val[i] = 0;
            occ[nocc++] = i;
            return &val[i];
        }
    }
    return 0;
}

void space_set_budget(uint64_t f) { g_budget = f; }
uint64_t space_frames_used(void)  { return g_used; }
int space_nx_enabled(void)        { return g_nx; }
void space_set_nx(int on)         { g_nx = on; }
void space_quiet(int on)          { g_quiet = on; }
int space_msgs(void)              { return g_msgs; }
const char *space_last_msg(void)  { return g_last; }
void space_msgs_reset(void)       { g_msgs = 0; g_last[0] = 0; }

void space_reset(void)
{
    for (uint32_t k = 0; k < nocc; k++) {
        uint32_t i = occ[k];
        if (key[i]) munmap((void *)(key[i] & ~0xFFFull), 4096);
        key[i] = 0; val[i] = 0;
    }
    nocc = 0;
    g_used = 0;
}

uint64_t space_pages_outside(uint64_t lo, uint64_t hi)
{
    uint64_t n = 0;
    for (uint32_t k = 0; k < nocc; k++) {
        if (!key[occ[k]]) continue;
        uint64_t va = key[occ[k]] & ~0xFFFull;
        if (va < lo || va >= hi) n++;
    }
    return n;
}

uint64_t space_pages_mapped(void) { return nocc; }

uint64_t space_pte(uint64_t va) { uint64_t *e = slot(va, 0); return e ? *e : 0; }
int space_nx(uint64_t va)       { return (space_pte(va) >> 63) & 1; }

/* ---- the loader's dependencies ------------------------------------------ */
uint64_t pmm_alloc(void)
{
    if (g_used >= g_budget) return 0;
    void *p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;
    g_used++;
    return (uint64_t)p;
}

static int host_prot(uint64_t flags)
{
    int p = PROT_READ;
    if (flags & VMM_WRITABLE) p |= PROT_WRITE;
    if (!(flags & PTE_NX))    p |= PROT_EXEC;
    return p;
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags)
{
    virt &= ~0xFFFull;
    uint64_t *e = slot(virt, 1);
    if (!e) { fprintf(stderr, "space: page table full\n"); exit(2); }
    if (phys != virt) {
        /* A move, i.e. a first mapping: relocate the frame onto the virtual
         * address the loader chose. Anything already there is replaced, exactly
         * as writing a PTE would. */
        void *r = mremap((void *)phys, 4096, 4096,
                         MREMAP_MAYMOVE | MREMAP_FIXED, (void *)virt);
        if (r == MAP_FAILED) {
            fprintf(stderr, "space: mremap %llx -> %llx failed\n",
                    (unsigned long long)phys, (unsigned long long)virt);
            exit(2);
        }
    }
    if (mprotect((void *)virt, 4096, host_prot(flags)) != 0) {
        fprintf(stderr, "space: mprotect failed\n");
        exit(2);
    }
    *e = (virt & PTE_ADDR_MASK) | 1 | (flags & ~PTE_ADDR_MASK);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags)
{
    for (uint64_t o = 0; o < size; o += 4096)
        vmm_map_page(virt + o, phys + o, flags);
}

uint64_t *vmm_pte(uint64_t cr3, uint64_t virt) { (void)cr3; return slot(virt, 0); }

int cpu_prot_nx(void)        { return 1; }
int cpu_prot_nx_usable(void) { return g_nx; }
int cpu_prot_smep(void)      { return 1; }
int cpu_prot_smap(void)      { return 1; }

void kernel_random_bytes(uint8_t *out, int len)
{
    /* Deterministic here on purpose: a test that asserts "16 bytes arrived"
     * must not be able to pass by accident on a buffer that was already
     * nonzero, and a fuzz run has to be reproducible. */
    static uint64_t s = 0x2545F4914F6CDD1Dull;
    for (int i = 0; i < len; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        out[i] = (uint8_t)(s >> 24);
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last, sizeof g_last, fmt, ap);
    va_end(ap);
    g_msgs++;
    if (!g_quiet) fputs(g_last, stdout);
}

/* ---- measuring writability with the real MMU ---------------------------- */
static sigjmp_buf g_jmp;
static volatile int g_trapped;
static void onsegv(int s) { (void)s; g_trapped = 1; siglongjmp(g_jmp, 1); }

int space_writable(uint64_t va)
{
    if (!(space_pte(va) & 1)) return 0;
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onsegv;
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    g_trapped = 0;
    if (sigsetjmp(g_jmp, 1) == 0) {
        volatile unsigned char *p = (volatile unsigned char *)va;
        unsigned char keep = *p;
        *p = (unsigned char)(keep ^ 0xFF);
        *p = keep;
    }
    sigaction(SIGSEGV, &old_segv, 0);
    sigaction(SIGBUS, &old_bus, 0);
    return !g_trapped;
}
