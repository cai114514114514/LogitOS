/* Port, PCI-config and firmware-memory access for the board drivers.
 *
 * The board code only ever touches bus 0 (the PCH's LPC bridge at 00:1f.0),
 * a handful of legacy I/O ports, and read-only firmware tables. Routing those
 * through one header lets tests/hw/board substitute a synthesized machine without
 * a second copy of the logic: the host build defines BOARD_HOST_TEST and links
 * its own bio_* functions. */
#ifndef BOARD_IO_H
#define BOARD_IO_H
#include <stdint.h>

#ifdef BOARD_HOST_TEST
uint8_t  bio_inb(uint16_t port);
void     bio_outb(uint16_t port, uint8_t value);
uint16_t bio_inw(uint16_t port);
void     bio_outw(uint16_t port, uint16_t value);
uint32_t bio_cfg32(uint8_t slot, uint8_t func, uint16_t off);
const void *bio_firmware(uint64_t phys, uint32_t bytes);
void     bio_udelay(unsigned us);
int      bio_device_aperture(uint64_t phys, uint64_t len);
void     kprintf(const char *fmt, ...);
#else
#include "io.h"
#include "pci.h"
#include "acpi.h"
#include "ktime.h"
#include "kprintf.h"
#include "driver.h"
static inline uint8_t  bio_inb(uint16_t port)                  { return inb(port); }
static inline void     bio_outb(uint16_t port, uint8_t value)  { outb(port, value); }
static inline uint16_t bio_inw(uint16_t port)                  { return inw(port); }
static inline void     bio_outw(uint16_t port, uint16_t value) { outw(port, value); }
static inline uint32_t bio_cfg32(uint8_t slot, uint8_t func, uint16_t off)
{ return pci_cfg_read(0, slot, func, off); }
static inline const void *bio_firmware(uint64_t phys, uint32_t bytes)
{ return acpi_map_firmware(phys, bytes); }
/* 1 if [phys, phys+len) overlaps a memory BAR of any enumerated device. */
static inline int bio_device_aperture(uint64_t phys, uint64_t len)
{
    /* PCIe ECAM (MCFG): reserved in E820, MMIO in fact */
    uint64_t base; uint16_t seg; uint8_t b0, b1;
    for (int i = 0; acpi_mcfg_entry(i, &base, &seg, &b0, &b1) == 0; i++) {
        uint64_t size = ((uint64_t)b1 - b0 + 1) << 20;
        uint64_t lo = base + ((uint64_t)b0 << 20);
        if (phys < lo + size && lo < phys + len) return 1;
    }
    for (int i = 0; i < dev_count(); i++) {
        const struct device *d = dev_at(i);
        for (int r = 0; d && r < DEV_NRES; r++) {
            const struct dev_resource *b = &d->res[r];
            if ((b->flags & DEV_RES_MEM) && b->start && b->size &&
                phys < b->start + b->size && b->start < phys + len) return 1;
        }
    }
    return 0;
}
static inline void bio_udelay(unsigned us)
{
    uint64_t end = time_mono_ns() + (uint64_t)us * 1000ull;
    while (time_mono_ns() < end)
        __asm__ volatile ("pause");
}
#endif

static inline uint16_t bio_cfg16(uint8_t slot, uint8_t func, uint16_t off)
{ return (uint16_t)(bio_cfg32(slot, func, off & ~3u) >> ((off & 2u) * 8)); }
static inline uint8_t bio_cfg8(uint8_t slot, uint8_t func, uint16_t off)
{ return (uint8_t)(bio_cfg32(slot, func, off & ~3u) >> ((off & 3u) * 8)); }

#endif
