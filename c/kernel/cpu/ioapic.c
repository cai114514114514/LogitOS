/* I/O APIC: routes device IRQs (GSIs) to LAPIC vectors on a chosen CPU, the
 * modern replacement for the 8259 PIC. We mask the PIC and route the ISA lines
 * (timer/keyboard/mouse) and the NIC through here; EOI then goes to the LAPIC. */
#include <stdint.h>
#include "ioapic.h"
#include "acpi.h"
#include "vmm.h"

static volatile uint8_t *io;        /* IOAPIC MMIO base */
static uint32_t gsi_base;
static int      max_rte;            /* max redirection entry index (from VER reg) */

static uint32_t rd(uint8_t reg) { *(volatile uint32_t *)(io + 0) = reg; return *(volatile uint32_t *)(io + 0x10); }
static void     wr(uint8_t reg, uint32_t v) { *(volatile uint32_t *)(io + 0) = reg; *(volatile uint32_t *)(io + 0x10) = v; }

void ioapic_init(void)
{
    uint32_t base = acpi_ioapic_addr();
    if (!base) return;
    vmm_map_page(base, base, VMM_WRITABLE | VMM_NOCACHE);
    io = (volatile uint8_t *)base;
    gsi_base = acpi_ioapic_gsibase();
    max_rte = (rd(0x01) >> 16) & 0xFF;                  /* max redirection entry */
    for (int i = 0; i <= max_rte; i++) {                /* mask everything to start */
        wr(0x10 + 2 * i, 1 << 16);
        wr(0x11 + 2 * i, 0);
    }
}

int ioapic_present(void) { return io != 0; }

/* Route GSI -> vector on `apic_id`. level/active_low matter for PCI lines
 * (level, active-low); ISA lines are edge, active-high (both 0). */
void ioapic_route(uint32_t gsi, uint8_t vec, uint8_t apic_id, int level, int active_low)
{
    if (!io || gsi < gsi_base) return;
    int n = (int)(gsi - gsi_base);
    if (n > max_rte) return;                            /* beyond the redirection table */
    uint32_t low = (uint32_t)vec
                 | (active_low ? (1u << 13) : 0)        /* polarity */
                 | (level ? (1u << 15) : 0);            /* trigger; mask bit 16 = 0 (enabled) */
    wr(0x11 + 2 * n, (uint32_t)apic_id << 24);          /* destination APIC id */
    wr(0x10 + 2 * n, low);
}

/* Route a legacy ISA IRQ (handles MADT source overrides) to `vec` on `apic_id`. */
void ioapic_route_isa(int isa_irq, uint8_t vec, uint8_t apic_id)
{
    uint32_t gsi = acpi_gsi_for_irq(isa_irq);
    uint16_t fl = acpi_gsi_flags(isa_irq);
    int active_low = ((fl & 0x3) == 0x3);              /* MPS INTI: 11 = active low */
    int level      = ((fl & 0xC) == 0xC);             /* 11 = level */
    ioapic_route(gsi, vec, apic_id, level, active_low);
}
