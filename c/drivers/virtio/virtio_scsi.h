/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_VIRTIO_SCSI_H
#define LOGIT_VIRTIO_SCSI_H

/* Probe one modern virtio-scsi PCI controller and register supported disks.
 * Called by blk_init before partition/root scanning. Returns disk count, or -1
 * if no usable controller exists. This is boot enumeration, not hotplug. */
int virtio_scsi_init(void);

#endif
