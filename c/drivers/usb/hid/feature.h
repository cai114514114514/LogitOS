#ifndef LOGIT_USB_HID_FEATURE_H
#define LOGIT_USB_HID_FEATURE_H

#include "../hid_report.h"

struct usb_device;

/* Probe-time only, before arming the input endpoint. Returns 1 after verified
 * touchpad mode selection, 0 when no mode field exists, -1 on refusal. */
int hid_touchpad_configure(struct usb_device *device, unsigned interface_number,
                           const struct hid_desc *descriptor);

#endif
