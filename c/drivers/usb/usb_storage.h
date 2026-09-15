/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_USB_STORAGE_H
#define LOGIT_USB_STORAGE_H
/* Register the SCSI-transparent Bulk-Only class, called once by USB core.
 * Boot enumeration publishes attached 512-byte direct-access media as usbN;
 * the existing filesystem root remains selected by the earlier block probe. */
void usb_msc_register(void);
#endif
