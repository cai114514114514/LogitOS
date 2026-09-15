/* RAM-only direct-map and allocation-zone test. This links the real PMM and
 * walks the page tables it builds, not a model of the intended mapping.
 * The arena reserves virtual space past 4 GiB but touches only metadata and a
 * few MiB of payload: a high-address test must not need 4 GiB of host RAM. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
#include <stdlib.h>
#include "pmm.h"
#include "mmhost.h"

uint64_t mm_host_base, mm_host_kend, mm_host_cr3;
static int checks, fails;
static void check(int yes, const char *why)
{ checks++; if (!yes) { printf("FAIL: %s\n", why); fails++; } }
void kprintf(const char *fmt, ...)
{
    /* Suppress per-allocation watermark chatter, retaining boot/map reports. */
    if (strstr(fmt, "[mm] low:")) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
int rmap_init(uint64_t frames) { (void)frames; return 0; }
void reclaim_on_alloc(void) {}
void oom_alloc_fail(void) {}

#define INFO 0x180000ull
#define ROOT 0x100000ull
#define LOW_END 0x04000000ull
#define HIGH_A (PMM_LOW_LIMIT + 0x1000ull)
#define HIGH_A_SIZE 0x402000ull
#define HIGH_B 0x100000000ull
#define HIGH_B_SIZE 0x201000ull
#define ARENA_END (HIGH_B + HIGH_B_SIZE)
#define PTE_ADDR 0x000ffffffffff000ull

struct entry { uint64_t addr, len; uint32_t type, reserved; };
static void setup(int high)
{
    mm_host_kend = 0x200000;
    mm_host_cr3 = ROOT;
    memset(mm_p2v(ROOT), 0, 4096);
    unsigned char *info = mm_p2v(INFO);
    struct mtag { uint32_t type, size, stride, version; } *mt = (void *)(info + 8);
    mt->type = 6; mt->size = sizeof(*mt) + 4 * sizeof(struct entry);
    mt->stride = sizeof(struct entry); mt->version = 0;
    struct entry *e = (void *)(mt + 1);
    e[0] = (struct entry){0, LOW_END, 1, 0};
    e[1] = (struct entry){HIGH_A, HIGH_A_SIZE, high ? 1u : 2u, 0};
    e[2] = (struct entry){0x80000000ull, 0x200000ull, 2, 0};
    e[3] = (struct entry){HIGH_B, HIGH_B_SIZE, high ? 1u : 2u, 0};
    uint32_t *end = (uint32_t *)((unsigned char *)mt + mt->size);
    end[0] = 0; end[1] = 8;
    ((uint32_t *)info)[0] = 8 + mt->size + 8;
    ((uint32_t *)info)[1] = 0;
    pmm_init(INFO);
}

/* Independent hardware-shaped walk: no production map helper is reused. */
static uint64_t translated(uint64_t va, uint64_t *leaf, int *user, int *low_tables)
{
    uint64_t phys = ROOT;
    *user = 1; *low_tables = 1; *leaf = 0;
    const unsigned shifts[] = {39, 30, 21, 12};
    for (unsigned level = 0; level < 4; level++) {
        if (phys >= PMM_LOW_LIMIT) *low_tables = 0;
        uint64_t e = ((uint64_t *)mm_p2v(phys))[(va >> shifts[level]) & 511];
        if (!(e & 1)) return UINT64_MAX;
        if (!(e & 4)) *user = 0;
        if (level == 3 || (e & 128)) {
            uint64_t offmask = (1ull << shifts[level]) - 1;
            *leaf = e;
            return (e & PTE_ADDR & ~offmask) | (va & offmask);
        }
        phys = e & PTE_ADDR;
    }
    return UINT64_MAX;
}
static void mapped(uint64_t phys, int large)
{
    uint64_t leaf; int user, low_tables;
    uint64_t got = translated(PHYSMAP_BASE + phys, &leaf, &user, &low_tables);
    check(got == phys, "high RAM page translates to its own physical address");
    check(!user, "high mapping is inaccessible to ring 3");
    check(low_tables, "all bootstrap mapping tables remain below 1 GiB");
    check((leaf & 2) != 0, "high mapping allows kernel writes");
#ifdef PHYSMAP_TEST_NO_NX
    check(!(leaf & (1ull << 63)), "NX-disabled boot never writes reserved NX bit");
#else
    check((leaf & (1ull << 63)) != 0, "NX-enabled high mapping is non-executable");
#endif
    check(!!(leaf & 128) == large, "only whole aligned RAM intervals use large pages");
}
static void absent(uint64_t phys)
{
    uint64_t leaf; int user, low_tables;
    check(translated(PHYSMAP_BASE + phys, &leaf, &user, &low_tables) == UINT64_MAX,
          "reserved hole or boundary outside RAM is not direct-mapped");
}

int main(void)
{
    check(!pmm_physmap_low_ready(), "low alias is not published before PMM initialisation");
    void *arena = mmap(NULL, ARENA_END, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (arena == MAP_FAILED) { perror("sparse physical arena"); return 2; }
    mm_host_base = (uint64_t)(uintptr_t)arena;
    setup(1);
    check(pmm_physmap_low_ready(), "low aliases stay ready even when the high-map control disables high RAM");
    const uint64_t high_pages = (HIGH_A_SIZE + HIGH_B_SIZE) / FRAME_SIZE;
#ifdef PHYSMAP_TEST_LOW_ONLY
    (void)high_pages;
    mapped(0x1234, 1);
    mapped(LOW_END - 1, 1);
    absent(LOW_END);
    absent(HIGH_A);
    check(pmm_physmap_pages() == LOW_END / FRAME_SIZE,
          "high-map control retains all low aliases");
    check(pmm_high_free_frames() == 0, "high-map control releases no high pages");
    uint64_t only_low = pmm_alloc_contig_masked(2, UINT64_MAX, 0, 0);
    check(only_low && only_low < PMM_LOW_LIMIT, "masked DMA falls back to mapped low RAM in control");
    if (only_low) {
        check(mm_v2p(mm_physmap_ptr(only_low)) == only_low, "low independent alias translates in control");
        pmm_free(only_low);pmm_free(only_low + FRAME_SIZE);
    }
    check(pmm_audit() == 0, "low alias control preserves allocator accounting");
    munmap(arena, ARENA_END);
    printf("physmap low control: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
#endif
    check(pmm_physmap_ready(), "physical map is ready");
    check(pmm_high_free_frames() == high_pages, "all and only high AVAILABLE pages released");
    check(pmm_physmap_pages() == high_pages + LOW_END / FRAME_SIZE, "mapped page count counts unique low and high RAM pages");
    mapped(0x1234, 1);
    mapped(LOW_END - 1, 1);
    absent(LOW_END);
    check(pmm_is_ram(INFO, 4096), "reserved boot metadata is still AVAILABLE RAM provenance");
    check(pmm_is_ram(HIGH_B + 3, HIGH_B_SIZE - 3), "RAM check covers unaligned complete extent");
    check(!pmm_is_ram(HIGH_B + HIGH_B_SIZE - 8, 9), "RAM check rejects last-byte hole crossing");
    check(!pmm_is_ram(0x80000000ull, 4096), "RAM check rejects reserved descriptor");
    check(!pmm_is_ram(HIGH_B, 0) && !pmm_is_ram(UINT64_MAX - 7, 16), "RAM check rejects empty and overflow");
    puts("LOW_ALIAS_CHECKS_COMPLETE");
    check(pmm_physmap_table_pages() > 0, "actual low page-table pages were allocated");
    mapped(HIGH_A + 31, 0);
    mapped(PMM_LOW_LIMIT + 0x200123, 1);
    mapped(HIGH_A + HIGH_A_SIZE - 1, 0);
    mapped(HIGH_B + 0x17, 1);
    mapped(HIGH_B + 0x200007, 0);
    absent(PMM_LOW_LIMIT);
    absent(HIGH_A + HIGH_A_SIZE);
    absent(0x80000000ull);
    absent(HIGH_B - FRAME_SIZE);
    absent(ARENA_END);

    uint64_t low0 = pmm_low_zone_free_frames();
    uint64_t low = pmm_alloc();
    uint64_t low_reserve = pmm_alloc_reserve();
    uint64_t contig = pmm_alloc_contig(17);
    check(low && low < PMM_LOW_LIMIT, "legacy allocator stays low with high RAM free");
    check(low_reserve && low_reserve < PMM_LOW_LIMIT, "legacy reserve allocator stays low");
    check(contig && contig + 17 * FRAME_SIZE <= PMM_LOW_LIMIT, "legacy contiguous allocation stays low");
    check(pmm_low_zone_free_frames() == low0 - 19, "low zone charges every allocated page");
    pmm_free(low); pmm_free(low_reserve);
    for (unsigned i = 0; i < 17; i++) pmm_free(contig + i * FRAME_SIZE);
    check(pmm_low_zone_free_frames() == low0, "low pages return to their own pool");

    uint64_t *pages = calloc(high_pages, sizeof(*pages));
    if (!pages) return 2;
    uint64_t free0 = pmm_free_frames();
    for (uint64_t i = 0; i < high_pages; i++) {
        uint64_t f = pmm_alloc_any(); pages[i] = f;
        if (i == 0)
            check(f == HIGH_B, "first ANY payload starts above 4 GiB when available");
        if (i == HIGH_B_SIZE / FRAME_SIZE)
            check(f == HIGH_A, "ANY wraps to 1..4 GiB after far RAM is exhausted");
        int is_ram = (f >= HIGH_A && f < HIGH_A + HIGH_A_SIZE) ||
                     (f >= HIGH_B && f < HIGH_B + HIGH_B_SIZE);
        check(is_ram, "ANY allocation prefers actual high RAM and skips holes");
        if (!f) continue;
        check(mm_v2p(mm_p2v(f)) == f, "physical/CPU alias round trip");
        uint64_t *payload = mm_p2v(f);
        payload[0] = f ^ 0x5555666677778888ull;
        payload[511] = ~f;
    }
    for (uint64_t i = 0; i < high_pages; i++) if (pages[i]) {
        uint64_t *payload = mm_p2v(pages[i]);
        check(payload[0] == (pages[i] ^ 0x5555666677778888ull) && payload[511] == ~pages[i],
              "high payload bytes survive allocations across physical holes and 4 GiB");
    }
    check(pmm_high_max_phys() >= HIGH_B, "actual allocated high physical address crosses 4 GiB");
    check(pmm_high_allocations() == high_pages, "high allocation counter reports actual allocations");
    check(pmm_high_live_frames() == high_pages, "live high pages counted independently of reserved holes");
    check(pmm_high_free_frames() == 0, "high zone exhaustion is observable");
    uint64_t fallback = pmm_alloc_any();
    check(fallback && fallback < PMM_LOW_LIMIT, "ANY falls back to low after high RAM exhausted");
    pmm_free(fallback);
    for (uint64_t i = 0; i < high_pages; i++) if (pages[i]) pmm_free(pages[i]);
    check(pmm_free_frames() == free0, "all payload frames return to the pre-test baseline");
    check(pmm_high_live_frames() == 0, "high live counter returns to zero");
    check(pmm_low_zone_free_frames() == low0, "high allocation does not consume low DMA RAM");

    pmm_set_reserve(pmm_free_frames());
    check(pmm_alloc_any() == 0, "ordinary ANY respects global reserve");
    uint64_t reserved = pmm_alloc_reserve_any();
    check(reserved >= PMM_LOW_LIMIT, "ANY reserve can complete a fault using high RAM");
    if (reserved) pmm_free(reserved);
    pmm_set_reserve(128);
    check(pmm_audit() == 0 && pmm_bugs() == 0, "PMM refcounts and both zones audit clean");
    pmm_report("high-test");
    free(pages);

    /* DMA constraints, using the real immutable RAM map and allocator. */
    uint64_t dma_before = pmm_free_frames();
    uint64_t d64 = pmm_alloc_contig_masked(3, UINT64_MAX, 65536, 65536);
    check(d64 >= HIGH_B && !(d64 & 65535), "64-bit DMA extent prefers aligned far RAM");
    check(pmm_is_ram(d64, 3 * FRAME_SIZE), "DMA allocated extent is actual RAM");
    for (unsigned i = 0; d64 && i < 3; i++) pmm_free(d64 + i * FRAME_SIZE);
    uint64_t d32 = pmm_alloc_contig_masked(3, UINT32_MAX, 8192, 16384);
    check(d32 >= HIGH_A && d32 + 3 * FRAME_SIZE <= 0x100000000ull,
          "32-bit DMA prefers the 1..4 GiB pool");
    check(!(d32 & 8191) && (d32 & 16383) + 3 * FRAME_SIZE <= 16384,
          "DMA alignment and boundary constrain the whole extent");
    for (unsigned i = 0; d32 && i < 3; i++) pmm_free(d32 + i * FRAME_SIZE);
    uint64_t dl = pmm_alloc_contig_masked(2, PMM_LOW_LIMIT - 1, 0, 0);
    check(dl && dl + 2 * FRAME_SIZE <= PMM_LOW_LIMIT, "restricted DMA uses low RAM");
    if (dl) { pmm_free(dl); pmm_free(dl + FRAME_SIZE); }
    check(!pmm_alloc_contig_masked(0, UINT64_MAX, 0, 0), "DMA rejects empty allocation");
    check(!pmm_alloc_contig_masked(SIZE_MAX, UINT64_MAX, 0, 0), "DMA rejects extent overflow");
    check(!pmm_alloc_contig_masked(1, 4094, 0, 0), "DMA rejects a noncontiguous address mask");
    check(!pmm_alloc_contig_masked(1, UINT64_MAX, 6144, 0), "DMA rejects non-power-of-two alignment");
    check(!pmm_alloc_contig_masked(1, UINT64_MAX, 0, 6144), "DMA rejects non-power-of-two boundary");
    check(!pmm_alloc_contig_masked(3, UINT64_MAX, 0, 8192), "DMA refuses extent larger than boundary");
    check(!pmm_alloc_contig_masked(1, 4095, 0, 0), "DMA cannot allocate reserved first frame");
    check(!pmm_alloc_contig_masked(LOW_END / FRAME_SIZE, UINT64_MAX, 0, 0),
          "DMA contiguous scan never bridges unavailable holes");
    uint64_t far_all = pmm_alloc_contig_masked(HIGH_B_SIZE / FRAME_SIZE, UINT64_MAX, 0, 0);
    check(far_all == HIGH_B, "masked allocator can claim the complete far RAM extent");
    uint64_t middle_all = pmm_alloc_contig_masked(HIGH_A_SIZE / FRAME_SIZE, UINT64_MAX, 0, 0);
    check(middle_all == HIGH_A, "masked allocator falls back to 1..4 GiB when far RAM is occupied");
    uint64_t low_fallback = pmm_alloc_contig_masked(1, UINT64_MAX, 0, 0);
    check(low_fallback && low_fallback < PMM_LOW_LIMIT, "masked allocator falls back low when all high RAM is occupied");
    if(low_fallback)pmm_free(low_fallback);
    for(uint64_t i=0;far_all && i<HIGH_B_SIZE/FRAME_SIZE;i++)pmm_free(far_all+i*FRAME_SIZE);
    for(uint64_t i=0;middle_all && i<HIGH_A_SIZE/FRAME_SIZE;i++)pmm_free(middle_all+i*FRAME_SIZE);
    check(pmm_free_frames() == dma_before && pmm_audit() == 0,
          "DMA failures and frees preserve exact PMM accounting");

    /* Compatibility fixture: old host gates start with no hardware CR3 and
     * less than 1 GiB. They must not need to construct a physmap or link a new
     * object to retain their former bootstrap contract. */
    setup(0);
    check(pmm_physmap_pages() == LOW_END / FRAME_SIZE, "low-only RAM still has the independent alias");
    mapped(0x1234, 1);
    check(pmm_high_free_frames() == 0, "reserved high descriptors never enter PMM");
    uint64_t compat = pmm_alloc_any();
    check(compat && compat < LOW_END, "ANY works on old low-memory machine");
    if (compat) pmm_free(compat);
    check(pmm_audit() == 0, "low-only reinitialization audits clean");
    munmap(arena, ARENA_END);
    printf("physmap: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
