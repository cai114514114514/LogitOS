#ifndef LOGIT_USB_H
#define LOGIT_USB_H

/* USB core: what a device is, and how a class driver gets bound to one.
 *
 * The point of this layer is that c/drivers/usb/usb_hid.c contains no xHCI, no
 * PCI and no enumeration -- it is a match table and two callbacks. That is what
 * makes "LogitOS has USB" mean more than "LogitOS has a USB keyboard": the next
 * class driver (mass storage, a hub, a serial adapter) is one new file.
 *
 * There is exactly one host controller type (xhci.c) and the calls below go
 * straight to it rather than through a vtable. A second controller would need
 * one; there is no second controller worth having (see xhci.h), so the
 * indirection is not written yet.
 */

#include <stdint.h>
#include "usb_desc.h"

#define USB_MAX_DEVICES 8

/* Standard request codes / recipients (USB 2.0 Table 9-4, 9-2) */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_SET_INTERFACE     0x0B

#define USB_RT_DIR_IN     0x80
#define USB_RT_TYPE_STD   0x00
#define USB_RT_TYPE_CLASS 0x20
#define USB_RT_RECIP_DEV  0x00
#define USB_RT_RECIP_IF   0x01
#define USB_RT_RECIP_EP   0x02

/* HID class requests (HID 1.11 section 7.2) */
#define HID_REQ_GET_REPORT   0x01
#define HID_REQ_GET_IDLE     0x02
#define HID_REQ_SET_REPORT   0x09
#define HID_REQ_SET_IDLE     0x0A
#define HID_REQ_SET_PROTOCOL 0x0B
#define HID_PROTO_BOOT   0
#define HID_PROTO_REPORT 1

struct usb_driver;

struct usb_device {
    int      used;
    uint8_t  slot;            /* xHCI slot id, 1-based */
    uint8_t  port;            /* root hub port, 1-based */
    uint8_t  speed;           /* XSPEED_* */
    uint8_t  addr;            /* USB address the controller assigned */
    struct usb_device_desc dd;
    struct usb_config cfg;
    int      ifno;            /* index into cfg.iface[] a driver bound to */
    const struct usb_driver *drv;
    void    *drvdata;
};

/* A class driver matches on the INTERFACE triple, not on vendor:product. That
 * is what makes one HID driver cover every keyboard: the device says "I am a
 * boot keyboard" and nobody has to have heard of the vendor. 0xFF is a
 * wildcard, which is how "any HID device, whatever its protocol" is spelled. */
#define USB_ANY 0xFF

struct usb_match {
    uint8_t if_class, if_subclass, if_proto;
};

struct usb_driver {
    const char *name;
    const struct usb_match *match;   /* terminated by an all-0xFF..0 sentinel:
                                      * { 0, 0, 0 } (class 0 is "use the device
                                      * descriptor", never a real interface) */
    int  (*probe)(struct usb_device *dev, int ifno);   /* 0 = bound */
    void (*poll)(struct usb_device *dev);              /* called ~100 Hz */
    void (*remove)(struct usb_device *dev);
};

/* Class drivers register from their own file at bring-up; usb_core calls each
 * registered driver's match table against every interface of every device. */
void usb_register_driver(const struct usb_driver *drv);

/* There is no usb_init(). The controller driver registers itself through the
 * device model (DRIVER_DECLARE in usb_core.c) and is bound by PCI class, so
 * nothing outside c/drivers/usb/ mentions USB -- no call in kmain, no line in
 * the Makefile. Adding a class driver is likewise one new file plus a
 * usb_register_driver() call from usb_core.c's bring-up. */

/* Control transfer, from a class driver. -> bytes transferred, or -1. */
int  usb_control(struct usb_device *d, uint8_t rt, uint8_t req, uint16_t val,
                 uint16_t idx, void *data, uint16_t len);

/* Statistics, reported to serial and asserted by tests/boot/run-usb-test.sh. */
unsigned long usb_devices_found(void);
unsigned long usb_reports_delivered(void);
unsigned long usb_keys_posted(void);
unsigned long usb_motion_posted(void);
int           usb_present(void);

#endif /* LOGIT_USB_H */
