#ifndef LOGIT_PCI_H
#define LOGIT_PCI_H
/* PCI bus driver: configuration access (ECAM when the ACPI MCFG table gives us
 * one, else the legacy 0xCF8/0xCFC ports), full enumeration of every bus behind
 * every PCI-to-PCI bridge, capability walking and BAR sizing.
 *
 * Enumeration publishes each function into the device model (c/drivers/core);
 * nothing here decides which driver gets what. See driver.h for the contract. */

#include <stdint.h>

/* ------------------------------------------------------- legacy interface --
 * `pci_find` predates the device model. It only ever looks at bus 0, function 0
 * and reports BAR0. It is kept because e1000/nvme/virtio still call it; new
 * code must use the device model (dev_find_class / a match table) instead. */
struct pci_dev {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint32_t bar0;
    uint8_t  irq_line;
};
int pci_find(uint16_t vendor, uint16_t device, struct pci_dev *out);

/* --------------------------------------------------------- config access --
 * `off` is a byte offset. Offsets >= 0x100 (PCIe extended config) only resolve
 * through ECAM and read back 0xFFFFFFFF on the legacy port path. */
uint32_t pci_cfg_read (uint8_t bus, uint8_t slot, uint8_t func, uint16_t off);
void     pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, uint32_t val);
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off);
uint8_t  pci_cfg_read8 (uint8_t bus, uint8_t slot, uint8_t func, uint16_t off);
void     pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, uint16_t val);
void     pci_cfg_write8 (uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, uint8_t val);

/* Command register bits (config 0x04) */
#define PCI_CMD_IO          0x0001
#define PCI_CMD_MEM         0x0002
#define PCI_CMD_MASTER      0x0004
#define PCI_CMD_INTX_DIS    0x0400   /* 1 = legacy INTx suppressed (set for MSI) */
/* Status register bits (config 0x06) */
#define PCI_STATUS_CAPLIST  0x0010

/* Config-space offsets worth naming */
#define PCI_CFG_VENDOR      0x00
#define PCI_CFG_COMMAND     0x04
#define PCI_CFG_STATUS      0x06
#define PCI_CFG_REVISION    0x08
#define PCI_CFG_CLASS       0x08   /* dword: rev, prog-if, subclass, class */
#define PCI_CFG_HEADER_TYPE 0x0E
#define PCI_CFG_BAR0        0x10
#define PCI_CFG_SEC_BUS     0x19   /* type 1: secondary bus number */
#define PCI_CFG_SUB_BUS     0x1A   /* type 1: subordinate bus number */
#define PCI_CFG_SUBSYS      0x2C
#define PCI_CFG_CAP_PTR     0x34
#define PCI_CFG_IRQ_LINE    0x3C
#define PCI_CFG_IRQ_PIN     0x3D

/* Capability IDs */
#define PCI_CAP_MSI     0x05
#define PCI_CAP_VENDOR  0x09
#define PCI_CAP_PCIE    0x10
#define PCI_CAP_MSIX    0x11

/* Base class codes (config 0x0B) */
#define PCI_CLASS_STORAGE   0x01
#define PCI_CLASS_NETWORK   0x02
#define PCI_CLASS_DISPLAY   0x03
#define PCI_CLASS_MULTIMEDIA 0x04
#define PCI_CLASS_BRIDGE    0x06
#define PCI_CLASS_SERIAL    0x0C   /* subclass 0x03 = USB; prog-if 0x30 = xHCI */

/* ------------------------------------------------------------------ ECAM --
 * pci_ecam_init() reads the ACPI MCFG table (acpi_mcfg_entry) and maps the
 * segment-0 window. Returns 1 if ECAM is in use, 0 if we fell back to 0xCF8.
 * pci_ecam_set() is the same thing with the window supplied directly -- the
 * host unit tests point it at a synthetic buffer. */
int      pci_ecam_init(void);
int      pci_ecam_set(uint64_t base, uint16_t seg, uint8_t bus_start, uint8_t bus_end);
int      pci_ecam_active(void);
uint64_t pci_ecam_base(void);

/* ---------------------------------------------------------- capabilities --
 * Walk the config-space capability chain for `cap_id`, returning its offset or
 * 0. Terminates on a malformed or looping chain (each visited offset is
 * recorded; a repeat ends the walk). */
uint8_t  pci_cap_find(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id);
/* Iterate: pass prev = 0 for the first match, then the previous result to get
 * the next one. A device may carry several capabilities of the same ID -- virtio
 * describes each of its config structures with its own vendor (0x09) capability,
 * so "find the first" is not enough. Returns 0 when there are no more. */
uint8_t  pci_cap_next(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id, uint8_t prev);
/* PCIe extended capabilities (offset >= 0x100). Requires ECAM; returns 0
 * otherwise. Same loop protection. */
uint16_t pci_ext_cap_find(uint8_t bus, uint8_t slot, uint8_t func, uint16_t cap_id);

/* --------------------------------------------------------------- BAR/res --
 * Size BAR `idx` of a function by the write-all-ones probe (decode is disabled
 * around the probe and the original value restored). Fills *out and returns the
 * number of BAR indices consumed: 1 normally, 2 for a 64-bit memory BAR, and 1
 * with out->flags == 0 when the BAR is unimplemented. */
struct dev_resource;
int pci_bar_probe(uint8_t bus, uint8_t slot, uint8_t func, int idx, struct dev_resource *out);

/* --------------------------------------------------------------- classes -- */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* ---------------------------------------------------------- enumeration -- */
/* Walk bus 0 and every bus reachable through a PCI-to-PCI bridge, publishing
 * each function into the device model. Idempotent; returns the device count. */
int pci_enumerate(void);

/* ECAM (if any) + enumeration. Call once, early: everything downstream --
 * including the legacy pci_find() -- answers out of the registry it builds. */
int pci_init(void);

#endif /* LOGIT_PCI_H */
