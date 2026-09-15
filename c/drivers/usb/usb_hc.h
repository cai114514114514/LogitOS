/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_USB_HC_H
#define LOGIT_USB_HC_H
#include <stdint.h>
#include "../core/io_lock.h"
struct device;
struct usb_device;
struct usb_interface;
struct usb_hc;

/* Shared speed encoding also matches the standard xHCI PSI values. EHCI must
 * translate these into QH EPS; copying this field into an EHCI QH is wrong. */
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW 2
#define USB_SPEED_HIGH 3
#define USB_SPEED_SUPER 4
#define USB_MAX_CONTROLLERS 8
#define USB_MAX_DEPTH 5

/* All operations except events/root_change_pending/int_in_* run in thread context and may wait.
 * A backend owns addresses, endpoint toggles, DMA and completion lifetime.
 * device_open consumes the already reset topology in d and assigns d->addr
 * (and a backend slot if needed); failure must remain closeable. No backend
 * enumerates interfaces or chooses a class driver. */
struct usb_hc_ops {
    const char *name;
    int (*root_port_count)(struct usb_hc *);
    int (*root_port_connected)(struct usb_hc *, int port);
    int (*root_port_reset)(struct usb_hc *, int port, int *speed);
    /* Port-change acknowledgement is deliberately split from the IRQ-side
     * pending check.  The former reads/clears a per-port RW1C bit and may take
     * the controller's sleepable owner; the latter must only report whether a
     * kworker pass is needed.  Returning 0 from root_port_changed means the
     * controller did not report a connection transition for that port, so the
     * core must leave an already-bound device alone. */
    int (*root_change_pending)(struct usb_hc *);
    int (*root_port_changed)(struct usb_hc *, int port, int *connected);
    int (*device_open)(struct usb_device *);
    void (*device_close)(struct usb_device *);
    int (*set_ep0_packet)(struct usb_device *, int packet);
    int (*configure)(struct usb_device *, const struct usb_interface *);
    int (*configure_hub)(struct usb_device *, int ports, int multi_tt, int tt_think);
    int (*control)(struct usb_device *, uint8_t rt, uint8_t request,
                   uint16_t value, uint16_t index, void *data, uint16_t len);
    int (*bulk)(struct usb_device *, uint8_t ep_addr, void *data, uint32_t len);
    int (*int_in_arm)(struct usb_device *, uint8_t ep_addr);
    int (*int_in_poll)(struct usb_device *, uint8_t ep_addr, uint8_t **data);
    /* Called only AFTER successful CLEAR_FEATURE(ENDPOINT_HALT). Also restore
     * backend toggle/dequeue state; returning success without that breaks BOT. */
    int (*clear_halt)(struct usb_device *, uint8_t ep_addr);
    void (*events)(struct usb_hc *);
    void (*irq_enable)(struct usb_hc *);
    int (*shutdown)(struct usb_hc *);
};
struct usb_hc {
    const struct usb_hc_ops *ops;
    void *priv;
    struct device *pci;
    unsigned index;
    io_lock_t admission;
    io_lock_t callbacks;       /* serialize IRQ/timer class decoding on one HC */
    unsigned online, active;
};

/* Called by a backend's PCI probe after controller initialization. Registers
 * built-in classes once, enumerates root ports and their hubs, then wires IRQ.
 * unregister closes transfer admission, drains users, removes children before
 * parents and shuts down DMA; the backend retains failed-stop allocations. */
int usb_hc_register(struct usb_hc *, struct device *, const struct usb_hc_ops *, void *priv);
void usb_hc_unregister(struct usb_hc *);
void usb_hc_irq(void *hc);
/* Hub caller has already powered/reset this port and waited for recovery. */
int usb_enumerate_child(struct usb_device *hub, unsigned port, unsigned speed);
/* Generic class-facing operations; transferred bytes or -1, except arm,
 * configure and clear_halt, which return 0/-1. */
int usb_configure_interface(struct usb_device *, const struct usb_interface *);
int usb_int_in_arm(struct usb_device *, uint8_t ep_addr);
int usb_int_in_poll(struct usb_device *, uint8_t ep_addr, uint8_t **data);
int usb_bulk(struct usb_device *, uint8_t ep_addr, void *data, uint32_t len);
int usb_clear_halt(struct usb_device *, uint8_t ep_addr);
/* Clear a failed USB2 control/bulk split transaction at the nearest HS hub. */
int usb_clear_tt_buffer(struct usb_device *, uint8_t ep_addr, unsigned xfer_type);
int usb_delay_ms(unsigned ms); /* 0 elapsed, -1 no usable monotonic clock */
#endif
