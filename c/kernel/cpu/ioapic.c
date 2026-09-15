/* I/O APIC: routes device IRQs (GSIs) to LAPIC vectors on a chosen CPU, the
 * modern replacement for the 8259 PIC. We mask the PIC and route the ISA lines
 * (timer/keyboard/mouse) and the NIC through here; EOI then goes to the LAPIC. */
#include <stdint.h>
#include "ioapic.h"
#include "acpi.h"
#include "apic_model.h"
#include "kprintf.h"
#include "vmm.h"
#include "../../drivers/core/io_lock.h"
static io_lock_t ioapic_gate;
static unsigned ioapic_ready;

static volatile uint8_t *io;        /* IOAPIC MMIO base */
static uint32_t gsi_base;
static int      max_rte;            /* max redirection entry index (from VER reg) */

static uint32_t rd(uint8_t reg) { *(volatile uint32_t *)(io + 0) = reg; return *(volatile uint32_t *)(io + 0x10); }
static void     wr(uint8_t reg, uint32_t v) { *(volatile uint32_t *)(io + 0) = reg; *(volatile uint32_t *)(io + 0x10) = v; }

int ioapic_init(void)
{
    uint32_t base = acpi_ioapic_addr();
    if (!base) return IOAPIC_ROUTE_SAFE_REJECT;
    vmm_map_page(base, base, VMM_WRITABLE | VMM_NOCACHE);
    IO_GUARD(&ioapic_gate);
    unsigned state = __atomic_load_n(&ioapic_ready, __ATOMIC_RELAXED);
    if (state == 1) return IOAPIC_ROUTE_OK;
    if (state == 2) return IOAPIC_ROUTE_UNSAFE;
    io = (volatile uint8_t *)(uintptr_t)base;
    gsi_base = acpi_ioapic_gsibase();
    max_rte = (rd(0x01) >> 16) & 0xFF;                  /* max redirection entry */
#ifndef LOGIT_X2APIC_NEGCTL_ALLOW_INVALID_IOAPIC_VER
    if (max_rte > (0xff - 0x11) / 2) {
        kprintf("[ioapic] invalid redirection-entry count max=%d\n", max_rte);
        max_rte = -1;
        return IOAPIC_ROUTE_SAFE_REJECT;
    }
#endif
    for (int i = 0; i <= max_rte; i++) {                /* mask everything to start */
        uint8_t low_reg = (uint8_t)(0x10 + 2 * i);
        uint8_t high_reg = (uint8_t)(low_reg + 1);
        wr(low_reg, rd(low_reg) | (1u << 16));
#ifndef LOGIT_X2APIC_NEGCTL_SKIP_INIT_MASK_READBACK
        if (!(rd(low_reg) & (1u << 16))) {
            __atomic_store_n(&ioapic_ready, 2, __ATOMIC_RELEASE);
            kprintf("[ioapic] initial mask readback failed rte=%d\n", i);
            return IOAPIC_ROUTE_UNSAFE;
        }
#endif
        /* Destination may be cleared only after the corresponding RTE is
         * confirmed masked; otherwise a stale live firmware route is mutated. */
        wr(high_reg, 0);
    }
    __atomic_store_n(&ioapic_ready, 1, __ATOMIC_RELEASE);
    return IOAPIC_ROUTE_OK;
}

int ioapic_present(void) { return __atomic_load_n(&ioapic_ready, __ATOMIC_ACQUIRE) == 1; }

int ioapic_gsi_valid(uint32_t gsi)
{
    /* Discovery is immutable after the release publication of ready. */
    return ioapic_present() && gsi >= gsi_base && gsi - gsi_base <= (uint32_t)max_rte;
}

int ioapic_can_route(uint32_t gsi, uint32_t apic_id)
{
    uint8_t encoded;
    return ioapic_gsi_valid(gsi) &&
           apic_model_external_dest8(apic_id, &encoded) == 0;
}

int ioapic_mask(uint32_t gsi)
{
    if (!ioapic_gsi_valid(gsi)) return -1;
    IO_GUARD(&ioapic_gate);
    uint8_t reg = (uint8_t)(0x10 + 2 * (gsi - gsi_base));
    wr(reg, rd(reg) | (1u << 16));
    /* A posted mask write alone is not the vector-retirement boundary. Read
     * the same RTE back while holding the selector/data register lock. */
    return (rd(reg) & (1u << 16)) ? 0 : -1;
}

int ioapic_is_masked(uint32_t gsi)
{
    if (!ioapic_gsi_valid(gsi)) return -1;
    IO_GUARD(&ioapic_gate);
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
    IO_GUARD(&ioapic_gate);
    if (gsi < gsi_base) return IOAPIC_ROUTE_SAFE_REJECT;
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
