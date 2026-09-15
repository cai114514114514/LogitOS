/* ACPI High Precision Event Timer clocksource.
 *
 * WHY THIS EXISTS
 * X79-era Xeon boards normally have an HPET, while their firmware or a VM may
 * decline to advertise an invariant TSC.  The old fallback was then the
 * 100 Hz PIT: correct, but every deadline and animation advanced in 10 ms
 * steps.  HPET supplies a free-running counter with a firmware-described MMIO
 * address and period, so it is a useful middle rung between TSC and PIT.
 *
 * WHAT THIS DELIBERATELY DOES NOT OWN
 * The comparator timers and their interrupt routes remain untouched.  PIT
 * still drives scheduler ticks.  Taking comparator IRQs would require ACPI
 * interrupt-routing and suspend/resume policy that this driver cannot prove;
 * a high-resolution read-only clocksource is useful without pretending that
 * work is done.
 */
#include "hpet.h"

#define HPET_TABLE_BYTES       56u
#define HPET_REG_CAP_ID        0x000u
#define HPET_REG_CONFIG        0x010u
#define HPET_REG_COUNTER       0x0f0u
#define HPET_REG_TIMER0_CONFIG 0x100u
#define HPET_REG_TIMER_STRIDE  0x020u
#define HPET_MAP_BYTES         0x500u

static uint16_t rd16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

int hpet_parse_table(const void *table, size_t bytes, struct hpet_desc *out)
{
    if (!table || !out || bytes < HPET_TABLE_BYTES) return -1;
    const uint8_t *p = (const uint8_t *)table;
    if (p[0] != 'H' || p[1] != 'P' || p[2] != 'E' || p[3] != 'T') return -1;
    uint32_t length = rd32(p + 4);
    if (length < HPET_TABLE_BYTES || length > bytes) return -1;

    /* GAS space 0 is system memory.  Accepting space 1 here and casting its
     * numeric port address to a pointer looks plausible and faults much later,
     * after the source has already replaced PIT.  Offset registers cannot be
     * represented by this MMIO driver either, so refuse both cases up front. */
#ifndef HPET_NEGCTL_SKIP_GAS
    if (p[40] != 0) return -1;
#endif
    if (p[42] != 0 || (p[41] != 0 && p[41] != 64) ||
        (p[43] != 0 && p[43] != 4)) return -1;
    uint64_t address = rd64(p + 44);
    if (!address || (address & 7u) || address > UINT64_MAX - HPET_MAP_BYTES) return -1;

    out->block_id       = rd32(p + 36);
    out->address        = address;
    out->sequence       = p[52];
    out->min_tick       = rd16(p + 53);
    out->page_protection = p[55];
    return 0;
}

int hpet_parse_caps(uint64_t raw, uint32_t firmware_block_id,
                    struct hpet_caps *out)
{
    if (!out) return -1;
#ifndef HPET_NEGCTL_SKIP_BLOCK_ID
    /* ACPI's Event Timer Block ID is a firmware snapshot of the hardware's
     * General Capabilities low dword.  A mismatch means the table points at a
     * different device/revision than the MMIO window we mapped, so trusting
     * its counter would turn corrupt firmware into silent clock drift. */
    if ((uint32_t)raw != firmware_block_id) return -1;
#else
    (void)firmware_block_id;
#endif
#ifndef HPET_NEGCTL_SKIP_REVISION
    /* HPET 1.0a reserves revision 00h as invalid.  A zero revision often means
     * an all-zero/unimplemented MMIO window; accepting it can make a plausible
     * period elsewhere in the qword look like a live counter. */
    if ((raw & 0xffu) == 0) return -1;
#endif
    uint32_t period = (uint32_t)(raw >> 32);
    /* ACPI requires at least 10 MHz (period <= 100 ns).  A zero or slower
     * value turns cycle-to-nanosecond conversion into division by zero or
     * silently trusts hardware outside the interface contract. */
    if (!period || period > 100000000u) return -1;
    uint64_t hz = 1000000000000000ull / period;
    if (hz < 10000000ull || hz > 10000000000ull) return -1;
    out->period_fs = period;
    out->hz        = hz;
    out->counter64 = (raw & (1ull << 13)) != 0;
    out->mask      = out->counter64 ? UINT64_MAX : UINT32_MAX;
    out->timers    = (uint8_t)(((raw >> 8) & 0x1fu) + 1u);
    return 0;
}

#ifdef LOGIT_HPET_HOST
int hpet_init(void) { return -1; }
int hpet_ready(void) { return 0; }
uint64_t hpet_read(void) { return 0; }
uint64_t hpet_hz(void) { return 0; }
uint64_t hpet_mask(void) { return 0; }
uint32_t hpet_period_fs(void) { return 0; }
#else
#include "acpi.h"
#include "kprintf.h"
#include "mm.h"
#include "mmhost.h"
#include "pmm.h"
#include "vmm.h"

static volatile uint8_t *g_mmio;
static struct hpet_caps g_caps;

static uint64_t mmio_read64(unsigned off)
{ return *(volatile uint64_t *)(g_mmio + off); }
static void mmio_write64(unsigned off, uint64_t v)
{ *(volatile uint64_t *)(g_mmio + off) = v; }

int hpet_ready(void) { return g_mmio != 0; }
uint64_t hpet_hz(void) { return g_mmio ? g_caps.hz : 0; }
uint64_t hpet_mask(void) { return g_mmio ? g_caps.mask : 0; }
uint32_t hpet_period_fs(void) { return g_mmio ? g_caps.period_fs : 0; }
uint64_t hpet_read(void)
{ return g_mmio ? (mmio_read64(HPET_REG_COUNTER) & g_caps.mask) : 0; }

int hpet_init(void)
{
    if (g_mmio) return 0;
    const uint8_t *table = (const uint8_t *)acpi_find_table("HPET");
    if (!table) { kprintf("[hpet] unavailable: ACPI HPET table absent\n"); return -1; }

    struct hpet_desc d;
    /* acpi_find_table() has already bounded and checksummed the whole SDT.  Its
     * length is safe to read only after that validation, and the parser checks
     * its own minimum again so these two layers cannot silently drift. */
    uint32_t table_len = rd32(table + 4);
    if (hpet_parse_table(table, table_len, &d) != 0) {
        kprintf("[hpet] unavailable: malformed ACPI HPET table\n");
        return -1;
    }
    uint64_t last = d.address + HPET_MAP_BYTES - 1u;
    uint64_t start = d.address & ~0xfffull;
    uint64_t last_page = last & ~0xfffull;
    if (last_page > UINT64_MAX - 0xfffu) {
        kprintf("[hpet] unavailable: MMIO range wraps address space\n");
        return -1;
    }
    uint64_t map_bytes = last_page - start + 0x1000u;
    if (mm_user_addr(start) || mm_user_addr(last)) {
        kprintf("[hpet] unavailable: unsafe MMIO address %p\n", (void *)(uintptr_t)d.address);
        return -1;
    }
    uint64_t cr3 = mm_read_cr3();
    for (uint64_t va = start;; va += 0x1000u) {
        /* ACPI is firmware-authenticated, not infallible.  Recasting an
         * AVAILABLE RAM page as uncached MMIO changes the cache contract for
         * every existing alias and can corrupt live kernel data. */
        if (pmm_is_ram(va, 0x1000u)) {
            kprintf("[hpet] unavailable: MMIO overlaps RAM at %p\n",
                    (void *)(uintptr_t)va);
            return -1;
        }
        uint64_t *old = vmm_pte(cr3, va);
        if (old && (*old & 1) && (((*old & MM_PTE_ADDR) != va) || (*old & VMM_USER))) {
            kprintf("[hpet] unavailable: MMIO conflicts with existing PTE at %p\n",
                    (void *)(uintptr_t)va);
            return -1;
        }
        if (va == last_page) break;
    }
    vmm_map_range(start, start, map_bytes, VMM_WRITABLE | VMM_NOCACHE);
    /* Firmware is allowed to place the register block near a page boundary.
     * Verify every page used by the counter and all 32 possible comparator
     * slots; checking only the base PTE can leave the second page unmapped. */
    for (uint64_t va = start;; va += 0x1000u) {
        uint64_t *pte = vmm_pte(cr3, va);
        if (!pte || !(*pte & 1) || (*pte & MM_PTE_ADDR) != va || (*pte & VMM_USER)) {
            kprintf("[hpet] unavailable: MMIO mapping failed at %p\n",
                    (void *)(uintptr_t)va);
            return -1;
        }
        if (va == last_page) break;
    }
    g_mmio = (volatile uint8_t *)(uintptr_t)d.address;

    uint64_t raw = mmio_read64(HPET_REG_CAP_ID);
    if (hpet_parse_caps(raw, d.block_id, &g_caps) != 0) {
        kprintf("[hpet] unavailable: capabilities disagree with firmware\n");
        g_mmio = 0;
        return -1;
    }
    /* We only read the main counter.  If firmware left a comparator armed,
     * enabling the counter could inject an IRQ whose route we do not own.
     * Refuse the whole source instead of clearing firmware state blindly. */
    for (unsigned i = 0; i < g_caps.timers; i++) {
        uint64_t cfg = mmio_read64(HPET_REG_TIMER0_CONFIG + i * HPET_REG_TIMER_STRIDE);
        if (cfg & (1ull << 2)) {
            kprintf("[hpet] unavailable: comparator %u already owns an interrupt\n", i);
            g_mmio = 0;
            return -1;
        }
    }

    uint64_t cfg = mmio_read64(HPET_REG_CONFIG);
    /* Never request legacy-replacement routing (bit 1); it would reroute the
     * PIT/RTC IRQs out from under drivers that still own them. */
    mmio_write64(HPET_REG_CONFIG, (cfg | 1ull) & ~2ull);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t before = hpet_read(), after = before;
    for (unsigned i = 0; i < 1000000u && after == before; i++) after = hpet_read();
    if (after == before) {
        mmio_write64(HPET_REG_CONFIG, cfg);
        g_mmio = 0;
        kprintf("[hpet] unavailable: main counter did not advance\n");
        return -1;
    }

    kprintf("[hpet] ready: base=%p period=%ufs counter=%u-bit timers=%u\n",
            (void *)(uintptr_t)d.address, (unsigned)g_caps.period_fs,
            g_caps.counter64 ? 64u : 32u, (unsigned)g_caps.timers);
    return 0;
}
#endif
