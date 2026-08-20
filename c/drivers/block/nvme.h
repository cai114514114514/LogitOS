#ifndef LOGIT_NVME_H
#define LOGIT_NVME_H

#include <stdint.h>

/* From-scratch NVMe block driver. nvme_init() probes for a controller (PCI class
 * 0x010802); nvme_present() reports success. nvme_busy() lets interrupts.c avoid
 * preempting the driver's own bring-up poll. */
int nvme_init(void);
int nvme_present(void);
int nvme_busy(void);

struct blk_req;   /* c/drivers/block/blkdev.h */

/* THE I/O INTERFACE, and it is the only one.
 *
 * nvme_read/nvme_write/nvme_read_n/nvme_write_n/nvme_flush used to live here.
 * They are gone rather than kept as wrappers, because a wrapper would be a
 * second way into the device and blkdev.h explains at length why two ways is
 * how a synchronous path and an asynchronous one come to disagree. Everything
 * reaches this driver through blkdev.c's request engine now: submit returns 0
 * once the first command is issued, poll returns 1 when the whole request --
 * which may be several MDTS-sized commands -- has finished. */
int nvme_blk_submit(struct blk_req *r);
int nvme_blk_poll(struct blk_req *r);

/* Namespace capacity in 512-byte sectors (0 if not present). The block layer
 * bounds every request with this, so it comes from Identify Namespace and never
 * from a partition table. */
uint64_t nvme_capacity(void);

/* SMART / health, from log page 0x02. Read once at bring-up and printed there;
 * re-read on demand by nvme_health(). Every field is the drive's own claim
 * about itself -- there is nothing here this kernel computed. */
struct nvme_health {
    uint8_t  critical_warning;   /* bit0 spare low, 1 temp, 2 reliability, 3 read-only, 4 volatile-mem backup */
    uint16_t temp_kelvin;        /* composite temperature, Kelvin as the spec reports it */
    uint8_t  spare_pct;          /* available spare, % */
    uint8_t  spare_threshold;    /* the % below which the drive raises bit 0 */
    uint8_t  used_pct;           /* percentage of rated endurance consumed; may exceed 100 */
    uint64_t data_read;          /* 1000 x 512-byte units. 128-bit on the wire, truncated */
    uint64_t data_written;
    uint64_t power_cycles;
    uint64_t power_on_hours;
    uint64_t unsafe_shutdowns;
    uint64_t media_errors;       /* unrecovered data integrity errors */
    uint64_t error_entries;      /* entries in the error information log */
};
int  nvme_health(struct nvme_health *out);   /* 0 on success; re-reads the log page */
void nvme_health_report(void);               /* read + kprintf, called from nvme_init */
int  nvme_health_known(void);                /* has a health page ever been read? */

#endif /* LOGIT_NVME_H */
