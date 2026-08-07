/* MSI / MSI-X, and the legacy INTx fallback, behind one model-level call.
 *
 * Why this is not per driver: an MSI is nothing but "write this data to this
 * address when you want attention", where the address encodes a LAPIC and the
 * data encodes a vector. Every driver that hand-rolls it ends up hand-picking a
 * vector, hand-installing an IDT gate and hand-routing a GSI -- which is exactly
 * why the e1000 in this tree is polled: there was no way to ask for an
 * interrupt. dev_irq_request() is that way.
 *
 * Preference order MSI-X > MSI > INTx. MSI-X first because its table lives in a
 * BAR (so it needs no config-space writes per vector and supports far more of
 * them); MSI second; INTx last because it is shared, level-triggered and needs
 * the I/O APIC.
 *
 * Message address/data (Intel SDM 10.11):
 *   address = 0xFEE00000 | (destination APIC id << 12)
 *             bit 3 = redirection hint (0 = fixed), bit 2 = destination mode
 *             (0 = physical). Both left at 0.
 *   data    = vector | (delivery mode << 8) | (level << 14) | (trigger << 15)
 *             Delivery mode 000 = fixed, edge-triggered. MSI is edge by
 *             definition -- it is a posted write, there is no line to level. */
#include <stdint.h>
#include <stddef.h>
#include "pci.h"
#include "driver.h"
#include "irq.h"
#include "vmm.h"
#include "lapic.h"
#include "ioapic.h"
#include "acpi.h"
#include "smp.h"
#include "kprintf.h"

#define MSI_ADDR_BASE   0xFEE00000u

/* MSI message-control bits (config cap + 2) */
#define MSI_MC_ENABLE   0x0001
#define MSI_MC_MMC      0x000E      /* multiple-message capable (log2) */
#define MSI_MC_MME      0x0070      /* multiple-message enable (log2) */
#define MSI_MC_64BIT    0x0080
#define MSI_MC_PVM      0x0100      /* per-vector masking */

/* MSI-X message-control bits (config cap + 2) */
#define MSIX_MC_TSIZE   0x07FF      /* table size - 1 */
#define MSIX_MC_FUNCMASK 0x4000
#define MSIX_MC_ENABLE  0x8000

static uint32_t msi_addr(uint32_t apic_id) { return MSI_ADDR_BASE | (apic_id << 12); }
static uint32_t msi_data(int vec)          { return (uint32_t)(vec & 0xFF); }

/* --------------------------------------------------------------- MSI-X -- */
static int msix_setup(struct device *d, int vec)
{
    uint8_t c = d->cap_msix;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
    uint32_t tbl = pci_cfg_read(d->bus, d->slot, d->func, (uint16_t)(c + 4));
    int bir = (int)(tbl & 7);
    uint64_t off = tbl & ~7u;

    if (bir >= DEV_NRES) return -1;
    uint64_t base = dev_bar_map(d, bir);
    if (!base) return -1;
    if (off + 16 > d->res[bir].size) return -1;      /* table outside its own BAR */

    /* Mask the whole function while the table is inconsistent, then program
     * entry 0 and unmask it. Order matters: an unmasked entry with a stale
     * address is a write to whatever used to be there. */
    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2),
                    (uint16_t)(mc | MSIX_MC_FUNCMASK | MSIX_MC_ENABLE));

    volatile uint32_t *e = (volatile uint32_t *)(uintptr_t)(base + off);
    e[3] = 1;                                        /* vector control: masked */
    e[0] = msi_addr(lapic_id());
    e[1] = 0;
    e[2] = msi_data(vec);
    __asm__ volatile ("" ::: "memory");
    e[3] = 0;                                        /* unmask this vector */

    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2),
                    (uint16_t)((mc & ~MSIX_MC_FUNCMASK) | MSIX_MC_ENABLE));

    /* MSI-X and INTx are mutually exclusive; suppress the line. */
    uint16_t cmd = pci_cfg_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
    pci_cfg_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND,
                    (uint16_t)(cmd | PCI_CMD_INTX_DIS));
    return 0;
}

static void msix_teardown(struct device *d)
{
    uint8_t c = d->cap_msix;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2),
                    (uint16_t)(mc & ~MSIX_MC_ENABLE));
}

/* ----------------------------------------------------------------- MSI -- */
static int msi_setup(struct device *d, int vec)
{
    uint8_t c = d->cap_msi;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));

    pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 4), msi_addr(lapic_id()));
    if (mc & MSI_MC_64BIT) {
        pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 8), 0);
        pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 12), (uint16_t)msi_data(vec));
        if (mc & MSI_MC_PVM)                          /* mask bits at cap+0x10 */
            pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 16), 0);
    } else {
        pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 8), (uint16_t)msi_data(vec));
        if (mc & MSI_MC_PVM)                          /* mask bits at cap+0x0C */
            pci_cfg_write(d->bus, d->slot, d->func, (uint16_t)(c + 12), 0);
    }

    /* One vector: MME = 0 regardless of how many the device offers. */
    mc = (uint16_t)((mc & ~MSI_MC_MME) | MSI_MC_ENABLE);
    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2), mc);

    uint16_t cmd = pci_cfg_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND);
    pci_cfg_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND,
                    (uint16_t)(cmd | PCI_CMD_INTX_DIS));
    return 0;
}

static void msi_teardown(struct device *d)
{
    uint8_t c = d->cap_msi;
    uint16_t mc = pci_cfg_read16(d->bus, d->slot, d->func, (uint16_t)(c + 2));
    pci_cfg_write16(d->bus, d->slot, d->func, (uint16_t)(c + 2),
                    (uint16_t)(mc & ~MSI_MC_ENABLE));
}

/* ---------------------------------------------------------------- INTx -- */
static int intx_setup(struct device *d, int vec)
{
    if (!d->irq_pin || !ioapic_present()) return -1;
    /* Without an AML interpreter we cannot evaluate _PRT, so the GSI comes from
     * the firmware-programmed interrupt line (config 0x3C) -- which is what the
     * e1000 has always used here. Lines < 16 go through the MADT's ISA source
     * overrides; 16..23 are already GSIs. */
    uint32_t gsi = (d->irq_line < 16) ? acpi_gsi_for_irq(d->irq_line) : d->irq_line;
    if (gsi >= 24 || d->irq_line == 0xFF) return -1;

    /* EDGE, not level, despite INTx being a level-triggered line: QEMU's TCG
     * I/O APIC does not clear a level RTE's remote-IRR on EOI, so a level entry
     * re-fires forever after its first interrupt. This is the same compromise
     * smp.c already makes for the NIC line, and it is documented there. */
    pci_cfg_write16(d->bus, d->slot, d->func, PCI_CFG_COMMAND,
                    (uint16_t)(pci_cfg_read16(d->bus, d->slot, d->func, PCI_CFG_COMMAND)
                               & ~PCI_CMD_INTX_DIS));
    ioapic_route(gsi, (uint8_t)vec, (uint8_t)lapic_id(), 0, 1);
    return 0;
}

/* --------------------------------------------------------- the model API -- */
static int g_max_mode = DEV_IRQ_MSIX;

int dev_irq_prefer(int max_mode)
{
    int old = g_max_mode;
    if (max_mode >= DEV_IRQ_INTX && max_mode <= DEV_IRQ_MSIX) g_max_mode = max_mode;
    return old;
}

int dev_irq_request(struct device *dev, irq_handler_t fn, void *arg, const char *name)
{
    if (!dev || !fn || dev->bus_type != DEV_BUS_PCI) return -1;
    if (dev->irq_mode != DEV_IRQ_NONE) return dev->irq_vec;   /* already wired */

    /* Message-signalled interrupts are delivered by a memory write to the LAPIC,
     * so there must be a LAPIC to write to. Before smp_init() there is not. */
    int have_lapic = lapic_ready();

    int vec = irq_alloc_vector(fn, arg, name);
    if (vec < 0) return -1;

    if (have_lapic && g_max_mode >= DEV_IRQ_MSIX && dev->cap_msix && msix_setup(dev, vec) == 0) {
        dev->irq_mode = DEV_IRQ_MSIX;
    } else if (have_lapic && g_max_mode >= DEV_IRQ_MSI && dev->cap_msi && msi_setup(dev, vec) == 0) {
        dev->irq_mode = DEV_IRQ_MSI;
    } else if (intx_setup(dev, vec) == 0) {
        dev->irq_mode = DEV_IRQ_INTX;
    } else {
        irq_free_vector(vec);
        return -1;
    }
    dev->irq_vec = (int16_t)vec;
    kprintf("[irq] %s: %s vector %d (%s)\n", dev->name,
            dev->irq_mode == DEV_IRQ_MSIX ? "msi-x" :
            dev->irq_mode == DEV_IRQ_MSI  ? "msi" : "intx",
            vec, name ? name : "?");
    return vec;
}

void dev_irq_release(struct device *dev)
{
    if (!dev || dev->irq_mode == DEV_IRQ_NONE) return;
    if (dev->irq_mode == DEV_IRQ_MSIX) msix_teardown(dev);
    else if (dev->irq_mode == DEV_IRQ_MSI) msi_teardown(dev);
    if (dev->irq_vec >= 0) irq_free_vector(dev->irq_vec);
    dev->irq_mode = DEV_IRQ_NONE;
    dev->irq_vec  = -1;
}

uint64_t dev_irq_count(const struct device *dev)
{
    if (!dev || dev->irq_vec < 0) return 0;
    return irq_vector_count(dev->irq_vec);
}
