#ifndef AETHER_ATA_H
#define AETHER_ATA_H

#include <stdint.h>

/* Minimal ATA PIO driver (primary bus, master). Polled, LBA28. */

/* Read `count` 512-byte sectors starting at `lba` into `buf`
 * (buf must hold count*512 bytes). Returns 0 on success, -1 on error. */
int ata_read(uint32_t lba, uint8_t count, void *buf);

/* Write `count` 512-byte sectors from `buf` starting at `lba`, then flush.
 * `buf` must hold count*512 bytes. Returns 0 on success, -1 on error. */
int ata_write(uint32_t lba, uint8_t count, const void *buf);

/* Non-zero while a PIO transfer is in flight: the timer IRQ must not preempt
 * (schedule()) mid-transfer, or the half-read controller state is abandoned. */
int ata_busy(void);

#endif /* AETHER_ATA_H */
