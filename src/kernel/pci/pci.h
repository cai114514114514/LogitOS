#ifndef AETHER_PCI_H
#define AETHER_PCI_H

#include <stdint.h>

/* A located PCI device. */
struct pci_dev {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint32_t bar0;          /* base address register 0 (MMIO base, low bits masked) */
    uint8_t  irq_line;      /* legacy IRQ (unused: we poll) */
};

/* Read/write 32-bit PCI configuration-space dwords (offset must be 4-aligned). */
uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void     pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val);

/* Scan for the first device matching vendor:device. Returns 0 if found (fills
 * *out), -1 otherwise. Also enables MMIO + bus-master in its command register. */
int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out);

#endif /* AETHER_PCI_H */
