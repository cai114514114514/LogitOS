#ifndef LOGIT_ATA_H
#define LOGIT_ATA_H

#include <stdint.h>

/* Minimal ATA PIO driver (primary bus, master). Polled, LBA28. */

/* Read `count` 512-byte sectors starting at `lba` into `buf`
 * (buf must hold count*512 bytes). Returns 0 on success, -1 on error. */
int ata_read(uint32_t lba, uint8_t count, void *buf);

/* Write `count` 512-byte sectors from `buf` starting at `lba`, then flush.
 * `buf` must hold count*512 bytes. Returns 0 on success, -1 on error. */
int ata_write(uint32_t lba, uint8_t count, const void *buf);

/* IDENTIFY DEVICE the primary master. Fills *sectors (LBA28 user-addressable
 * count) and a 41-byte model string. Returns 0 if a drive answered, -1 if the
 * port is empty or the drive did not respond -- which is how the block layer
 * decides whether to register an ata0 at all, instead of registering a phantom
 * device on every machine that has no IDE controller. */
int ata_identify(uint64_t *sectors, char model[41]);

/* Non-zero while a PIO transfer is in flight: the timer IRQ must not preempt
 * (schedule()) mid-transfer, or the half-read controller state is abandoned. */
int ata_busy(void);

/* The flag behind ata_busy(), exposed because AHCI shares it.
 *
 * c/kernel/cpu/interrupts.c consults ata_busy() to skip schedule() while an
 * ATA-family transfer is in flight; an AHCI command makes exactly the same
 * claim (a controller with a command outstanding and a PRDT pointing into a
 * buffer that must not be handed to another thread), so ahci.c raises this one
 * rather than introducing a second flag that interrupts.c would also have to
 * be taught about. */
extern volatile int g_ata_busy;
/* A no-op that returns success: ata_write() already issues FLUSH CACHE (0xE7)
 * after every write, so an ATA disk is on media by the time the write returns.
 * Present so the block layer has one shape for every backend. */
int ata_flush(void);

#endif /* LOGIT_ATA_H */
