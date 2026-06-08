#ifndef AQUA_NVME_H
#define AQUA_NVME_H

#include <stdint.h>

/* From-scratch NVMe block driver. nvme_init() probes for a controller (PCI class
 * 0x010802); nvme_present() reports success. read/write `count` 512-byte sectors
 * at `lba`. nvme_busy() lets interrupts.c avoid preempting a polled command. */
int nvme_init(void);
int nvme_present(void);
int nvme_busy(void);
int nvme_read(uint32_t lba, uint8_t count, void *buf);
int nvme_write(uint32_t lba, uint8_t count, const void *buf);

#endif /* AQUA_NVME_H */
