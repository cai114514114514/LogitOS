#include <stdint.h>
#include "pci.h"
#include "io.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define CFG_VENDOR   0x00   /* u16 vendor, u16 device */
#define CFG_COMMAND  0x04   /* u16 command, u16 status */
#define CFG_BAR0     0x10
#define CFG_IRQ      0x3C   /* u8 irq line */

#define CMD_IO_SPACE   0x01
#define CMD_MEM_SPACE  0x02
#define CMD_BUS_MASTER 0x04

static uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return (uint32_t)((1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                      ((uint32_t)func << 8) | (off & 0xFC));
}

uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

void pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, val);
}

int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out)
{
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = pci_cfg_read(0, slot, 0, CFG_VENDOR);
        uint16_t v = id & 0xFFFF, d = id >> 16;
        if (v == 0xFFFF)
            continue;                      /* no device in this slot */
        if (v != vendor || d != device)
            continue;

        out->bus = 0; out->slot = slot; out->func = 0;
        out->vendor = v; out->device = d;
        out->bar0 = pci_cfg_read(0, slot, 0, CFG_BAR0) & ~(uint32_t)0xF;
        out->irq_line = (uint8_t)(pci_cfg_read(0, slot, 0, CFG_IRQ) & 0xFF);

        /* Enable memory space + bus mastering so the NIC can DMA. */
        uint32_t cmd = pci_cfg_read(0, slot, 0, CFG_COMMAND);
        cmd |= CMD_IO_SPACE | CMD_MEM_SPACE | CMD_BUS_MASTER;
        pci_cfg_write(0, slot, 0, CFG_COMMAND, cmd);
        return 0;
    }
    return -1;
}
