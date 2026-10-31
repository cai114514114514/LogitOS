/* I/O APIC: routes device IRQs (GSIs) to LAPIC vectors on a chosen CPU, the
 * modern replacement for the 8259 PIC. We mask the PIC and route the ISA lines
 * (timer/keyboard/mouse) and the NIC through here; EOI then goes to the LAPIC. */
#include <stdint.h>
#include "ioapic.h"
#include "ioapic_map.h"
#include "acpi.h"
#include "apic_model.h"
#include "kprintf.h"
#include "vmm.h"
#include "../../../drivers/core/io_lock.h"
static io_lock_t ioapic_gate;
static unsigned ioapic_ready;

static volatile uint8_t *io;        /* IOAPIC MMIO base */
static uint32_t gsi_base;
static int      max_rte;            /* max redirection entry index (from VER reg) */

static uint32_t rd(uint8_t reg) { *(volatile uint32_t *)(io + 0) = reg; return *(volatile uint32_t *)(io + 0x10); }
static void     wr(uint8_t reg, uint32_t v) { *(volatile uint32_t *)(io + 0) = reg; *(volatile uint32_t *)(io + 0x10) = v; }

/* GSI selection is part of the selector/window transaction: all callers hold
 * ioapic_gate, so another CPU cannot switch the global MMIO leaf mid-write.
 * rd()/wr() deliberately keep their original bodies for existing host gates. */
static int chip_select(uint32_t gsi)
{
    const struct ioapic_chip *chip = ioapic_map_chip(ioapic_map_find(gsi));
    if (!chip) {
        return -1;
    }
    io = (volatile uint8_t *)(uintptr_t)chip->phys;
    gsi_base = chip->gsi_base;
    max_rte = chip->nr_rte - 1;
    return 0;
}

/* Discover every MADT controller before publishing readiness. Intel 82093AA
 * section 3 defines VER[23:16] as the maximum RTE index (not the count).
 * Linux v6.12 io_apic.c: io_apic_get_redir_entries()/mp_register_ioapic().
 * Model/QEMU evidence only: not verified on X79-VG4 hardware. */
int ioapic_init(void)
{
    IO_GUARD(&ioapic_gate);
    unsigned state = __atomic_load_n(&ioapic_ready, __ATOMIC_RELAXED);
    if (state == 1) {
        return IOAPIC_ROUTE_OK;
    }
    if (state == 2) {
        return IOAPIC_ROUTE_UNSAFE;
    }
    ioapic_map_reset();
    for (int index = 0; index < acpi_ioapic_count(); index++) {
        struct acpi_ioapic_info info;
        if (acpi_ioapic_get(index, &info) != 0) {
            continue;
        }
        /* Correction: mapping only the base page missed IOWIN for a MADT
         * base ending in ff0. Intel 82093AA section 3 places IOREGSEL at +00
         * and IOWIN at +10. The earlier comment incorrectly attributed EOI
         * at +40 to that part: Intel 6/C200 datasheet section 13.5.1 Table
         * 13-4 and section 13.5.4 define the PCH's direct EOIR at +40.
         * https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/6-chipset-c200-chipset-datasheet.pdf
         * Cover all three before VER; the version check still gates EOI use.
         * Keep arithmetic 64-bit so a base of fffffff0 cannot wrap to zero.
         * Both pages must be uncached, not just accessible via an old map. */
        uint64_t page = (uint64_t)info.addr & ~0xfffull;
        uint64_t last_page = ((uint64_t)info.addr + 0x43) & ~0xfffull;
        vmm_map_page(page, page, VMM_WRITABLE | VMM_NOCACHE);
        if (last_page != page) {
            vmm_map_page(last_page, last_page, VMM_WRITABLE | VMM_NOCACHE);
        }
        io = (volatile uint8_t *)(uintptr_t)info.addr;
        gsi_base = info.gsi_base;
        uint32_t version = rd(0x01);
        max_rte = (version >> 16) & 0xff;
#ifndef LOGIT_X2APIC_NEGCTL_ALLOW_INVALID_IOAPIC_VER
        if (max_rte > (0xff - 0x11) / 2) {
            kprintf("[ioapic] chip skipped @%08x: invalid redirection-entry count max=%d\n",
                    info.addr, max_rte);
            continue;
        }
#endif
        /* Validate ranges before touching RTEs. A duplicate or overlapping
         * controller must not alter a route already owned by another chip. */
        unsigned entries = (unsigned)max_rte + 1;
#ifdef LOGIT_X2APIC_NEGCTL_ALLOW_INVALID_IOAPIC_VER
        /* Preserve the old bad-VER control: it must reach wrapped selector
         * writes, not be rescued by the new map's independent validation. */
        unsigned registered_entries = entries > 120 ? 120 : entries;
#else
        unsigned registered_entries = entries;
#endif
        int chip = ioapic_map_add(info.addr, info.gsi_base, registered_entries,
                                  info.id, (uint8_t)version);
        if (chip < 0) {
            kprintf("[ioapic] chip skipped @%08x: invalid or overlapping GSI range\n", info.addr);
            continue;
        }
        for (int i = 0; i <= max_rte; i++) {
            /* 82093AA section 3.2.4: RTE low mask bit 16; high destination
             * bits 31:24. Confirm masking before clearing a firmware target. */
            uint8_t low_reg = (uint8_t)(0x10 + 2 * i);
            uint8_t high_reg = (uint8_t)(low_reg + 1);
            wr(low_reg, rd(low_reg) | (1u << 16));
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_INIT_MASK_READBACK
            if (!(rd(low_reg) & (1u << 16))) {
                __atomic_store_n(&ioapic_ready, 2, __ATOMIC_RELEASE);
                kprintf("[ioapic] initial mask readback failed chip=%d rte=%d\n", chip, i);
                return IOAPIC_ROUTE_UNSAFE;
            }
#endif
            wr(high_reg, 0);
        }
        kprintf("[ioapic] chip %d id %u @%08x gsi %u-%u ver %02x\n", chip,
                info.id, info.addr, info.gsi_base, info.gsi_base + entries - 1,
                (unsigned)(version & 0xff));
    }
    if (!ioapic_map_count()) {
        return IOAPIC_ROUTE_SAFE_REJECT;
    }
    __atomic_store_n(&ioapic_ready, 1, __ATOMIC_RELEASE);
    return IOAPIC_ROUTE_OK;
}

int ioapic_present(void) { return __atomic_load_n(&ioapic_ready, __ATOMIC_ACQUIRE) == 1; }

/* Keep these timeout leaves in the runtime tail: the established INTx gates
 * compile that tail verbatim after their single-/multi-chip MMIO models. */
#if __STDC_HOSTED__
#include <time.h>
#else
#include "../../core/ktime.h"
#endif

/* Keep the timeout independent of timer IRQ delivery: rerouting can run with
 * IF clear. Intel SDM vol. 2 RDTSC supplies ticks; ktime supplies their measured
 * frequency. Hosted register models use the host clock instead of x86 asm. */
static uint64_t drain_ticks(void)
{
#if __STDC_HOSTED__
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
#else
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
#endif
}

/* One millisecond is a retirement budget, not a chipset response-time claim.
 * Without calibrated TSC we still poll, but only under the iteration ceiling
 * below; inventing a CPU frequency would make the deadline misleading. */
static uint64_t drain_budget(void)
{
#if __STDC_HOSTED__
    return 1000000;
#else
    return time_source_hz(TIMESRC_TSC) / 1000;
#endif
}

/* Linux v6.12 arch/x86/kernel/apic/io_apic.c: ioapic_irq_get_chip_state()
 * distinguishes a queued level IRQ from an idle callback counter. Also wait
 * for Delivery Status (82093AA section 3.2.4 bit 12): an accepted delivery can
 * still be in progress before Remote IRR (bit 14) becomes visible. Take only
 * short MMIO locks, leaving the old vector/handler installed for its real EOI.
 * The 100000-read ceiling also bounds broken/stopped/uncalibrated clocks. */
static int wait_drained(uint32_t gsi)
{
    uint64_t budget = drain_budget();
    uint64_t start = drain_ticks();
    for (unsigned attempt = 0; attempt < 100000; attempt++) {
        uint32_t low = ioapic_rte_low(gsi);
        if (low == 0xffffffffu || !(low & (1u << 16))) {
            return -1;
        }
        if (!(low & (1u << 12)) &&
            (!(low & (1u << 15)) || !(low & (1u << 14)))) {
            return 0;
        }
        if (budget && drain_ticks() - start >= budget) {
            break;
        }
        io_relax();
    }
    return -1;
}

/* Explicitly acknowledge the OLD vector while the RTE remains masked.
 * Linux v6.12 io_apic.c: __eoi_ioapic_pin()/io_apic_eoi() use MMIO +0x40
 * (not an IOREGSEL index) on version >= 0x20. Older chips need the masked
 * level->edge->level sequence; writing zero to read-only Remote IRR cannot
 * clear it. Intel 6-series datasheet section 13.5.4 matches EOI against the
 * CURRENT RTE vector, not the vector remembered at delivery. Drain/EOI before
 * writing a new vector. This only settles the IOAPIC; irq_free_vector must
 * separately quarantine the old CPU/vector until its LAPIC IRR/ISR is clear.
 * Source: https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/apic/io_apic.c
 * Model evidence only: not verified on X79-VG4 hardware. */
static int eoi_old_route(uint32_t gsi)
{
    IO_GUARD(&ioapic_gate);
    if (chip_select(gsi) != 0) {
        return -1;
    }
    uint8_t reg = (uint8_t)(0x10 + 2 * (gsi - gsi_base));
    uint32_t low = rd(reg);
    if (!(low & (1u << 16))) {
        return -1;
    }
    if (!(low & (1u << 15)) || !(low & (1u << 14))) {
        return 0;
    }
    const struct ioapic_chip *chip = ioapic_map_chip(ioapic_map_find(gsi));
    if (chip->ver >= 0x20) {
        *(volatile uint32_t *)(io + 0x40) = low & 0xff;
    } else {
        wr(reg, low & ~(1u << 15));
        /* Flush the edge write before restoring level, retaining mask and
         * old vector throughout (Linux __eoi_ioapic_pin()). */
        (void)rd(reg);
        wr(reg, low);
    }
    return 0;
}

int ioapic_gsi_valid(uint32_t gsi)
{
    /* Discovery is immutable after the release publication of ready. */
    return ioapic_present() && ioapic_map_find(gsi) >= 0;
}

int ioapic_can_route(uint32_t gsi, uint32_t apic_id)
{
    uint8_t encoded;
    return ioapic_gsi_valid(gsi) &&
           apic_model_external_dest8(apic_id, &encoded) == 0;
}

int ioapic_mask(uint32_t gsi)
{
    if (!ioapic_gsi_valid(gsi)) {
        return -1;
    }
    {
        IO_GUARD(&ioapic_gate);
        if (chip_select(gsi) != 0) {
            return -1;
        }
        uint8_t reg = (uint8_t)(0x10 + 2 * (gsi - gsi_base));
        wr(reg, rd(reg) | (1u << 16));
        if (!(rd(reg) & (1u << 16))) {
            return -1;
        }
    }
    /* Correction: Mask, and later even a cleared Remote IRR after explicit
     * EOI, were treated as proof that the old vector could be freed. Neither
     * proves the target CPU has no accepted interrupt. This function settles
     * only the IOAPIC; irq_free_vector retains CPU-side quarantine separately.
     * Linux v6.12 io_apic.c __eoi_ioapic_pin() and vector.c __vector_cleanup().
     * Allow normal EOI first, then bound controller-side rescue as before. */
#ifndef PRT_NEGCTL_NO_RIRR_DRAIN
    if (wait_drained(gsi) == 0) {
        return 0;
    }
    if (eoi_old_route(gsi) == 0 && wait_drained(gsi) == 0) {
        return 0;
    }
    kprintf("[irq-route] GSI %u Remote IRR/delivery did not drain; isolated (masked), polling required\n", gsi);
    return -1;
#else
    return 0;
#endif
}

uint32_t ioapic_rte_low(uint32_t gsi)
{
    if (!ioapic_gsi_valid(gsi)) return 0xffffffffu;
    IO_GUARD(&ioapic_gate);
    if (chip_select(gsi) != 0) return 0xffffffffu;
    return rd((uint8_t)(0x10 + 2 * (gsi - gsi_base)));
}

int ioapic_is_masked(uint32_t gsi)
{
    if (!ioapic_gsi_valid(gsi)) return -1;
    IO_GUARD(&ioapic_gate);
    if (chip_select(gsi) != 0) return -1;
    return !!(rd((uint8_t)(0x10 + 2 * (gsi - gsi_base))) & (1u << 16));
}

static int __attribute__((unused)) mask_locked(uint8_t low_reg)
{
    uint32_t low = rd(low_reg);
    wr(low_reg, low | (1u << 16));
    return (rd(low_reg) & (1u << 16)) ? IOAPIC_ROUTE_SAFE_REJECT
                                      : IOAPIC_ROUTE_UNSAFE;
}

/* Route GSI -> vector on `apic_id`. level/active_low matter for PCI lines
 * (level, active-low); ISA lines are edge, active-high (both 0). */
int ioapic_route(uint32_t gsi, uint8_t vec, uint32_t apic_id,
                 int level, int active_low)
{
    uint8_t destination;
    if (!ioapic_present() || !ioapic_gsi_valid(gsi)) return IOAPIC_ROUTE_SAFE_REJECT;
    if (apic_model_external_dest8(apic_id, &destination) != 0) {
        kprintf("[ioapic] refusing 8-bit destination apic_id=%u\n",
                (unsigned)apic_id);
        return IOAPIC_ROUTE_SAFE_REJECT;
    }
    /* Shared polarity changes retain their vector, but must still acknowledge
     * an old level delivery before changing trigger mode. A stuck masked pin
     * is refused; rollback must not silently re-enable it either. */
    if (ioapic_mask(gsi) != 0) {
        return ioapic_is_masked(gsi) == 1 ? IOAPIC_ROUTE_SAFE_REJECT
                                         : IOAPIC_ROUTE_UNSAFE;
    }
    IO_GUARD(&ioapic_gate);
    if (chip_select(gsi) != 0) return IOAPIC_ROUTE_SAFE_REJECT;
    int n = (int)(gsi - gsi_base);
    if (n > max_rte) return IOAPIC_ROUTE_SAFE_REJECT;   /* beyond redirection table */
    uint32_t low = (uint32_t)vec
                 | (active_low ? (1u << 13) : 0)        /* polarity */
                 | (level ? (1u << 15) : 0);            /* trigger; mask bit 16 = 0 (enabled) */
    /* Keep the entry masked while changing destination/vector. An ignored
     * high write can otherwise expose a stale destination during the interval
     * before readback catches it. */
    uint8_t low_reg = (uint8_t)(0x10 + 2 * n);
    uint8_t high_reg = (uint8_t)(low_reg + 1);
    uint32_t high = (uint32_t)destination << 24;
    wr(low_reg, rd(low_reg) | (1u << 16));
    if (!(rd(low_reg) & (1u << 16))) {
        kprintf("[ioapic] cannot mask route before update gsi=%u\n", (unsigned)gsi);
        return IOAPIC_ROUTE_UNSAFE;
    }
    wr(high_reg, high);                                  /* 8-bit destination */
    wr(low_reg, low | (1u << 16));
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_ROUTE_READBACK
    /* Verify the complete new route while it is still masked. Delivery status
     * and remote-IRR are read-only and may change asynchronously. */
    const uint32_t writable_low = 0x0001afffu;
    uint32_t got_high = rd(high_reg);
    uint32_t got_low = rd(low_reg);
    if ((got_high & 0xff000000u) != high ||
        (got_low & writable_low) != ((low | (1u << 16)) & writable_low)) {
        int safe = mask_locked(low_reg);
        kprintf("[ioapic] route readback failed gsi=%u apic_id=%u\n",
                (unsigned)gsi, (unsigned)apic_id);
        return safe;
    }
#endif
    wr(low_reg, low);                                    /* verified route: enable */
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_ROUTE_READBACK
    got_high = rd(high_reg);
    got_low = rd(low_reg);
    if ((got_high & 0xff000000u) != high ||
        (got_low & writable_low) != (low & writable_low)) {
        int safe = mask_locked(low_reg);
        kprintf("[ioapic] route enable readback failed gsi=%u apic_id=%u\n",
                (unsigned)gsi, (unsigned)apic_id);
        return safe;
    }
#endif
    return IOAPIC_ROUTE_OK;
}

/* Route a legacy ISA IRQ (handles MADT source overrides) to `vec` on `apic_id`. */
int ioapic_route_isa(int isa_irq, uint8_t vec, uint32_t apic_id)
{
    uint32_t gsi = acpi_gsi_for_irq(isa_irq);
    uint16_t fl = acpi_gsi_flags(isa_irq);
    int active_low = ((fl & 0x3) == 0x3);              /* MPS INTI: 11 = active low */
    int level      = ((fl & 0xC) == 0xC);             /* 11 = level */
    return ioapic_route(gsi, vec, apic_id, level, active_low);
}

int ioapic_route_legacy_set(uint32_t apic_id)
{
    static const int irq[3] = {0, 1, 12};
    static const uint8_t vec[3] = {32, 33, 44};
    uint32_t gsi[3];
    for (int i = 0; i < 3; i++) {
        gsi[i] = acpi_gsi_for_irq(irq[i]);
        if (!ioapic_can_route(gsi[i], apic_id)) return IOAPIC_ROUTE_SAFE_REJECT;
#ifndef LOGIT_X2APIC_NEGCTL_ALLOW_DUPLICATE_GSI
        for (int j = 0; j < i; j++) {
            if (gsi[j] == gsi[i]) {
                kprintf("[ioapic] refusing duplicate legacy GSI %u for IRQ %d/%d\n",
                        (unsigned)gsi[i], irq[j], irq[i]);
                return IOAPIC_ROUTE_SAFE_REJECT;
            }
        }
#endif
    }
    for (int i = 0; i < 3; i++) {
        int status = ioapic_route_isa(irq[i], vec[i], apic_id);
        if (status == IOAPIC_ROUTE_OK) continue;
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_LEGACY_ROLLBACK
        int unsafe = 0;
        for (int j = 0; j < 3; j++)
            if (ioapic_mask(gsi[j]) != 0) unsafe = 1;
        if (unsafe) {
            kprintf("[ioapic] legacy route rollback could not confirm all masks\n");
            return IOAPIC_ROUTE_UNSAFE;
        }
#endif
        return IOAPIC_ROUTE_SAFE_REJECT;
    }
    return IOAPIC_ROUTE_OK;
}

/* Diagnostics use the immutable map, never the last selected MMIO window. */
int ioapic_chip_count(void)
{
    return ioapic_present() ? ioapic_map_count() : 0;
}

/* The panel and serial log use the same physical range spelling. */
int ioapic_describe(int index, char *buf, int max)
{
    const struct ioapic_chip *chip = ioapic_map_chip(index);
    if (!ioapic_present() || !chip || !buf || max <= 0) {
        return -1;
    }
    ksnprintf(buf, max, "ioapic %d id %u @%08x gsi %u-%u ver %02x", index,
              chip->id, chip->phys, chip->gsi_base,
              chip->gsi_base + chip->nr_rte - 1, chip->ver);
    return 0;
}
