/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_USB_BOT_H
#define LOGIT_USB_BOT_H
#include <stdint.h>

/* USB-IF Bulk-Only Transport 1.0, sections 3/5/6:
 * https://www.usb.org/sites/default/files/usbmassbulk_10.pdf
 * SCSI byte layouts are cross-checked against Seagate's public command manual,
 * especially READ CAPACITY(16) protection/last-LBA fields and cache sync IMMED:
 * https://www.seagate.com/files/staticfiles/support/docs/manual/Interface%20manuals/100293068h.pdf
 * This is the production command engine, independent of the host controller.
 * The owner must serialize the WHOLE command, including reset recovery, across
 * every LUN on an interface. Per-endpoint locks cannot provide that ordering. */
struct usb_bot {
    void *ctx;
    int (*bulk)(void *, uint8_t, void *, uint32_t);
    int (*control)(void *, uint8_t, uint8_t, uint16_t, uint16_t, void *, uint16_t);
    int (*clear_halt)(void *, uint8_t);
    uint8_t in, out, interface;
    uint32_t tag, resets;
    int dead;                    /* failed recovery: refuse subsequent CBWs */
};

/* 0 = command passed, 1 = SCSI command failed (REQUEST SENSE is appropriate),
 * -1 = transport/phase error. A failed WRITE is never silently submitted twice.
 * done is RELEVANT/PROCESSED bytes from CSW, not just a host DMA byte count. */
int usb_bot_exec(struct usb_bot *, uint8_t lun, const uint8_t *cdb,
                 uint8_t cdb_len, void *data, uint32_t len, int in, uint32_t *done);
int usb_bot_reset(struct usb_bot *);
int usb_bot_max_lun(struct usb_bot *);  /* 0..15; -1 for malformed response */

/* SCSI wire helpers also used by the real class driver. All integer accesses
 * are explicit byte order operations; neither unaligned structs nor native
 * endian casts describe the device protocol. */
uint32_t usb_scsi_be32(const uint8_t *);
uint64_t usb_scsi_be64(const uint8_t *);
int usb_scsi_capacity10(const uint8_t *, uint32_t len, uint64_t *sectors);
int usb_scsi_capacity16(const uint8_t *, uint32_t len, uint64_t *sectors);
int usb_scsi_rw_cdb(uint8_t out[16], int write, uint64_t lba, uint32_t count);
int usb_scsi_sense(const uint8_t *, uint32_t len, uint8_t *key, uint8_t *asc, uint8_t *ascq);
#endif
