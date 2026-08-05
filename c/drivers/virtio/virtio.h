#ifndef LOGIT_VIRTIO_H
#define LOGIT_VIRTIO_H

#include <stdint.h>

/* Modern (virtio 1.0) virtio-pci transport + split virtqueue. QEMU exposes the
 * modern config structures in a 64-bit MMIO BAR via vendor PCI capabilities;
 * this driver parses them, negotiates VIRTIO_F_VERSION_1, sets up queues, and
 * does synchronous (single-outstanding) requests by polling the used ring. */

#define VIRTIO_VENDOR        0x1AF4
#define VIRTIO_DEV_BLK       0x1001   /* transitional virtio-blk (QEMU default) */
#define VIRTIO_DEV_GPU       0x1050   /* modern virtio-gpu */

/* device_status bits */
#define VIRTIO_S_ACK         1
#define VIRTIO_S_DRIVER      2
#define VIRTIO_S_DRIVER_OK   4
#define VIRTIO_S_FEATURES_OK 8

/* virtq_desc.flags */
#define VIRTQ_DESC_F_NEXT    1
#define VIRTQ_DESC_F_WRITE   2   /* buffer is device-writable */

struct virtq_desc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct virtq_avail { uint16_t flags; uint16_t idx; uint16_t ring[]; } __attribute__((packed));
struct virtq_used_elem { uint32_t id; uint32_t len; } __attribute__((packed));
struct virtq_used { uint16_t flags; uint16_t idx; struct virtq_used_elem ring[]; } __attribute__((packed));

struct virtq {
    uint16_t size;
    struct virtq_desc  *desc;
    struct virtq_avail *avail;
    struct virtq_used  *used;
    volatile uint16_t  *notify;   /* this queue's notify register */
    uint16_t last_used;           /* last used.idx we consumed */
    uint16_t free_head;           /* next request's first descriptor (rotates, so a
                                   * stale completion's e->id never aliases ours) */
};

struct virtio_dev {
    volatile uint8_t *common;     /* virtio_pci_common_cfg */
    volatile uint8_t *notify_base;
    uint32_t          notify_mult;
    volatile uint8_t *isr;
    volatile uint8_t *device;     /* device-specific config */
    uint8_t bus, slot, func;
};

/* One scatter/gather buffer for a request. addr is a PHYSICAL address
 * (identity-mapped). device_writes = 1 if the device fills it (read side). */
struct virtio_buf { uint64_t addr; uint32_t len; int device_writes; };

/* Find dev `devid`, map BAR, reset, ACK+DRIVER, negotiate VIRTIO_F_VERSION_1 plus
 * the device feature bits in `want_lo` (bits 0..31). Returns 0, or -1. */
int  virtio_init(uint16_t devid, struct virtio_dev *vd, uint32_t want_lo);

/* Set up queue `qidx` (allocates desc/avail/used). Returns 0, or -1. */
int  virtio_queue_setup(struct virtio_dev *vd, int qidx, struct virtq *vq);

/* Finish init (DRIVER_OK). */
void virtio_driver_ok(struct virtio_dev *vd);

/* Submit a descriptor chain of `n` buffers on queue `qidx`, notify, and poll
 * until the device completes it. Returns the number of bytes the device wrote
 * (used.len), or -1 on timeout. Single outstanding request at a time. */
int  virtio_request(struct virtio_dev *vd, struct virtq *vq, int qidx,
                    struct virtio_buf *bufs, int n);

/* Non-zero while a virtio request is polling: the timer must not preempt it. */
int  virtio_busy(void);

#endif /* LOGIT_VIRTIO_H */
