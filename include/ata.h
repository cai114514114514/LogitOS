#ifndef AQUA_ATA_H
#define AQUA_ATA_H

#include <stdint.h>

/* Minimal ATA PIO driver (primary bus, master). Polled, LBA28. */

/* Read `count` 512-byte sectors starting at `lba` into `buf`
 * (buf must hold count*512 bytes). Returns 0 on success, -1 on error. */
int ata_read(uint32_t lba, uint8_t count, void *buf);

#endif /* AQUA_ATA_H */
