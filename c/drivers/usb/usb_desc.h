#ifndef LOGIT_USB_DESC_H
#define LOGIT_USB_DESC_H

/* USB descriptor parsing -- pure, no controller, no allocation.
 *
 * Descriptors are the first bytes a machine accepts from a device it has never
 * seen, over a bus where anyone can plug anything in. A parser that trusts
 * bLength walks off the end of the buffer; a parser that trusts wTotalLength
 * reads memory the device never sent; a parser that does not reject bLength == 0
 * loops forever inside an interrupt-adjacent code path. None of those are
 * hypothetical -- they are the three bugs every from-scratch USB stack ships
 * first. So this file validates and returns -1 rather than partially filling a
 * structure, and tests/unit/usb_desc_test.c feeds it truncated and malformed
 * input and requires rejection, not a best effort.
 *
 * References: USB 2.0 specification 9.6 (Standard Descriptors); HID 1.11 6.2.1
 * (the Class Descriptor that hangs off an interface).
 */

#include <stdint.h>

/* --- descriptor types (USB 2.0 Table 9-5) --- */
#define USB_DT_DEVICE     0x01
#define USB_DT_CONFIG     0x02
#define USB_DT_STRING     0x03
#define USB_DT_INTERFACE  0x04
#define USB_DT_ENDPOINT   0x05
#define USB_DT_HID        0x21
#define USB_DT_HID_REPORT 0x22

/* --- endpoint attributes (Table 9-13) --- */
#define USB_XFER_CONTROL 0
#define USB_XFER_ISOC    1
#define USB_XFER_BULK    2
#define USB_XFER_INT     3

#define USB_EP_NUM(a)   ((a) & 0x0F)
#define USB_EP_IS_IN(a) (((a) & 0x80) != 0)
#define USB_EP_XFER(at) ((at) & 0x03)

/* --- interface classes we care about --- */
#define USB_CLASS_HID      0x03
#define USB_CLASS_MASS     0x08
#define USB_CLASS_HUB      0x09

#define USB_HID_SUB_BOOT   0x01
#define USB_HID_PROTO_KBD  0x01
#define USB_HID_PROTO_MOUSE 0x02

/* Bounded because everything in this kernel is: a device claiming more is
 * rejected rather than silently truncated (a truncated interface list is how a
 * driver ends up bound to an endpoint the device does not have). */
#define USB_MAX_EP_PER_IF 8
#define USB_MAX_IF        8

struct usb_device_desc {
    uint16_t bcd_usb;
    uint8_t  dev_class, dev_subclass, dev_proto;
    uint8_t  max_packet0;      /* bMaxPacketSize0 -- for SuperSpeed this is the
                                * EXPONENT, i.e. 9 means 512. Callers must know. */
    uint16_t vendor, product, bcd_device;
    uint8_t  i_manufacturer, i_product, i_serial;
    uint8_t  n_configs;
};

struct usb_endpoint {
    uint8_t  addr;             /* bEndpointAddress, direction in bit 7 */
    uint8_t  attr;             /* bmAttributes */
    uint16_t max_packet;
    uint8_t  interval;         /* bInterval, in the device's own units */
};

struct usb_interface {
    uint8_t num, alt, n_ep;
    uint8_t if_class, if_subclass, if_proto;
    struct usb_endpoint ep[USB_MAX_EP_PER_IF];
    uint8_t  has_hid;
    uint16_t hid_report_len;   /* wDescriptorLength of the first report desc */
};

struct usb_config {
    uint16_t total_len;
    uint8_t  n_if;             /* interfaces actually recorded (alt 0 only) */
    uint8_t  value;            /* bConfigurationValue -- SET_CONFIGURATION arg */
    uint8_t  attributes, max_power;
    struct usb_interface iface[USB_MAX_IF];
};

/* 18 bytes exactly, type 1. -> 0 on success, -1 if short/mistyped. */
int usb_parse_device_desc(const uint8_t *buf, int len, struct usb_device_desc *out);

/* Walk a full configuration blob (the config descriptor followed by its
 * interface / endpoint / class descriptors). Requires wTotalLength to be
 * present in full: a caller that fetched fewer bytes than the device asked for
 * gets -1, because half a configuration cannot be configured.
 *
 * Alternate settings other than 0 are skipped -- we never issue SET_INTERFACE,
 * so binding to an alt setting we will not select would be a lie. */
int usb_parse_config(const uint8_t *buf, int len, struct usb_config *out);

/* wTotalLength out of the first 9 bytes, so the caller knows how much to fetch
 * on the second pass. 0 if those 9 bytes are not a config descriptor. */
uint16_t usb_config_total_len(const uint8_t *buf, int len);

/* First endpoint of `iface` matching transfer type and direction, or NULL. */
const struct usb_endpoint *usb_find_ep(const struct usb_interface *iface, int xfer, int in);

#endif /* LOGIT_USB_DESC_H */
