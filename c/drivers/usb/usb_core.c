/* USB core: enumeration and driver binding.
 *
 * Enumeration is the sequence USB 2.0 section 9.1.2 and xHCI 4.3 describe, and
 * the order is not negotiable -- a device answers nothing until its port has
 * been reset, has no address until the controller assigns one, and describes
 * nothing useful until its configuration has been read in two passes (you must
 * read the configuration descriptor to learn how long the configuration is).
 *
 * Binding is keyed on the INTERFACE class/subclass/protocol triple, which is the
 * reason this file is worth more than the HID driver it currently serves: a
 * keyboard, a flash drive and a hub all arrive through the same nine steps, and
 * only the last one differs.
 *
 * WHERE THIS RUNS. Nowhere in kmain: this driver is registered declaratively
 * (DRIVER_DECLARE at the bottom) and the device model's probe/bind pass calls
 * xhci_probe() for any PCI function of class 0x0C/0x03/0x30. No file outside
 * c/drivers/usb/ mentions USB at all -- no call in kmain, no line in the
 * Makefile, no vector in interrupts.c.
 *
 * Enumeration happens synchronously inside probe(), which kmain reaches via
 * dev_probe_all() after smp_init(). Two things about that context are worth
 * knowing. It is late enough to do real work -- the AHCI driver reads disks
 * from its own probe() -- but the boot path still has interrupts masked, so
 * timer_ms() would not advance and every timeout in the controller driver would
 * degenerate to a raw spin count. So the bring-up opens an interrupt window
 * around itself and restores the caller's IF afterwards, exactly as
 * c/drivers/virtio/virtio.c's request path does at the same point in the boot,
 * and for the same reason: the poll needs a time base.
 *
 * After that, everything is interrupt-driven. There is no USB thread and no
 * polling loop.
 */

#include <stdint.h>
#include <stddef.h>
#include "usb.h"
#include "usb_desc.h"
#include "xhci.h"
#include "driver.h"
#include "pci.h"
#include "kprintf.h"
#include "pit.h"

void *memset(void *, int, size_t);

void usb_hid_register(void);          /* usb_hid.c */

#define USB_MAX_DRIVERS 4

static struct usb_device g_dev[USB_MAX_DEVICES];
static const struct usb_driver *g_drv[USB_MAX_DRIVERS];
static int g_ndrv;
static int g_present;

static unsigned long g_found;
unsigned long usb_reports_total;      /* bumped by usb_hid.c */
unsigned long usb_keys_total;
unsigned long usb_motion_total;

unsigned long usb_devices_found(void)    { return g_found; }
unsigned long usb_reports_delivered(void){ return usb_reports_total; }
unsigned long usb_keys_posted(void)      { return usb_keys_total; }
unsigned long usb_motion_posted(void)    { return usb_motion_total; }
int           usb_present(void)          { return g_present; }

void usb_register_driver(const struct usb_driver *drv)
{
    if (g_ndrv < USB_MAX_DRIVERS) g_drv[g_ndrv++] = drv;
}

int usb_control(struct usb_device *d, uint8_t rt, uint8_t req, uint16_t val,
                uint16_t idx, void *data, uint16_t len)
{
    return xhci_control(d, rt, req, val, idx, data, len);
}

static struct usb_device *dev_alloc(void)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (!g_dev[i].used) {
            memset(&g_dev[i], 0, sizeof g_dev[i]);
            g_dev[i].used = 1;
            return &g_dev[i];
        }
    return NULL;
}

static const char *speed_name(int s)
{
    switch (s) {
    case XSPEED_LOW:   return "low";
    case XSPEED_FULL:  return "full";
    case XSPEED_HIGH:  return "high";
    case XSPEED_SUPER: return "super";
    default:           return "?";
    }
}

static int get_descriptor(struct usb_device *d, uint8_t type, uint8_t index,
                          void *buf, uint16_t len)
{
    return usb_control(d, USB_RT_DIR_IN | USB_RT_TYPE_STD | USB_RT_RECIP_DEV,
                       USB_REQ_GET_DESCRIPTOR,
                       (uint16_t)((type << 8) | index), 0, buf, len);
}

/* A device that answers a request wrong once and right on retry is the normal
 * case, not the exceptional one: the 10 ms recovery after a port reset is a
 * minimum, not a guarantee, and low-speed devices in particular NAK for a
 * while. Three attempts, and say so when it took more than one. */
static int get_descriptor_retry(struct usb_device *d, uint8_t type, uint8_t index,
                                void *buf, uint16_t len)
{
    for (int try = 0; try < 3; try++) {
        int n = get_descriptor(d, type, index, buf, len);
        if (n >= 0) {
            if (try) kprintf("[usb] descriptor %02x took %d attempts\n", type, try + 1);
            return n;
        }
    }
    return -1;
}

/* One interface, one driver. Endpoint configuration is transport work and
 * happens here, before probe(), so a class driver never issues an xHCI command:
 * by the time probe() runs its endpoints have transfer rings and doorbells. */
static int bind_interface(struct usb_device *d, int ifno)
{
    struct usb_interface *it = &d->cfg.iface[ifno];

    for (int i = 0; i < g_ndrv; i++) {
        const struct usb_driver *drv = g_drv[i];
        const struct usb_match *m = drv->match;
        int matched = 0;
        for (; m && !(m->if_class == 0 && m->if_subclass == 0 && m->if_proto == 0); m++) {
            if (m->if_class != USB_ANY && m->if_class != it->if_class) continue;
            if (m->if_subclass != USB_ANY && m->if_subclass != it->if_subclass) continue;
            if (m->if_proto != USB_ANY && m->if_proto != it->if_proto) continue;
            matched = 1;
            break;
        }
        if (!matched) continue;

        if (xhci_configure_ep(d, it) != 0) {
            kprintf("[usb] endpoint configuration failed for if%d\n", ifno);
            return -1;
        }
        d->ifno = ifno;
        if (drv->probe(d, ifno) == 0) {
            d->drv = drv;
            kprintf("[usb] if%d bound to driver '%s'\n", ifno, drv->name);
            return 0;
        }
    }
    return -1;
}

static int enumerate_port(int port)
{
    int speed = 0;
    if (xhci_port_reset(port, &speed) != 0) return -1;

    int slot = xhci_enable_slot();
    if (slot < 0) return -1;

    struct usb_device *d = dev_alloc();
    if (!d) { kprintf("[usb] device table full\n"); xhci_free_slot(slot); return -1; }
    d->slot = (uint8_t)slot;
    d->port = (uint8_t)port;
    d->speed = (uint8_t)speed;

    if (xhci_address_device(d, 0) != 0) goto fail;

    /* Pass 1 of the device descriptor: 8 bytes, which is all any device is
     * required to answer with the default max packet size. Byte 7 is
     * bMaxPacketSize0 -- the thing we needed in order to ask correctly. */
    uint8_t buf[256];
    int n = get_descriptor_retry(d, USB_DT_DEVICE, 0, buf, 8);
    if (n < 8) { kprintf("[usb] port %d: no device descriptor\n", port); goto fail; }
    int mp0 = buf[7];
    if (speed == XSPEED_FULL && mp0 != 8) {
        if (mp0 != 16 && mp0 != 32 && mp0 != 64) {
            kprintf("[usb] port %d: bMaxPacketSize0 %d is not legal\n", port, mp0);
            goto fail;
        }
        if (xhci_set_ep0_packet(d, mp0) != 0) goto fail;
    }

    n = get_descriptor_retry(d, USB_DT_DEVICE, 0, buf, 18);
    if (n < 18 || usb_parse_device_desc(buf, n, &d->dd) != 0) {
        kprintf("[usb] port %d: device descriptor rejected (%d bytes)\n", port, n);
        goto fail;
    }

    /* Pass 1 of the configuration: 9 bytes, to learn wTotalLength. */
    n = get_descriptor_retry(d, USB_DT_CONFIG, 0, buf, 9);
    if (n < 9) { kprintf("[usb] port %d: no config descriptor\n", port); goto fail; }
    uint16_t total = usb_config_total_len(buf, n);
    if (total < 9 || total > sizeof buf) {
        kprintf("[usb] port %d: wTotalLength %d out of range\n", port, total);
        goto fail;
    }
    n = get_descriptor_retry(d, USB_DT_CONFIG, 0, buf, total);
    if (n < (int)total || usb_parse_config(buf, n, &d->cfg) != 0) {
        kprintf("[usb] port %d: configuration rejected (%d of %d bytes)\n", port, n, total);
        goto fail;
    }

    if (usb_control(d, USB_RT_TYPE_STD | USB_RT_RECIP_DEV, USB_REQ_SET_CONFIGURATION,
                    d->cfg.value, 0, NULL, 0) < 0) {
        kprintf("[usb] port %d: SET_CONFIGURATION failed\n", port);
        goto fail;
    }

    g_found++;
    kprintf("USB_DEV port=%d addr=%d speed=%s vid=%04x pid=%04x class=%02x ifs=%d\n",
            port, d->addr, speed_name(speed), d->dd.vendor, d->dd.product,
            d->dd.dev_class, d->cfg.n_if);

    int bound = 0;
    for (int i = 0; i < d->cfg.n_if; i++) {
        struct usb_interface *it = &d->cfg.iface[i];
        kprintf("USB_IF port=%d if=%d class=%02x sub=%02x proto=%02x eps=%d\n",
                port, it->num, it->if_class, it->if_subclass, it->if_proto, it->n_ep);
        if (!bound && bind_interface(d, i) == 0) bound = 1;
    }
    if (!bound)
        kprintf("[usb] port %d: no driver for this device (it is enumerated, just idle)\n", port);
    return 0;

fail:
    xhci_free_slot(slot);
    d->used = 0;
    return -1;
}

/* --------------------------------------------- the interrupt handler --- */

/* Runs in interrupt context on the BSP with the BKL held; the device model
 * sends EOI. Everything in here is bounded and allocation-free:
 *   xhci_events()  drains the event ring into the per-endpoint FIFOs
 *   drv->poll()    decodes whatever landed and posts it to the WM input ring
 * and the second step is the same work c/drivers/char/keyboard.c does inside
 * IRQ 1. Nothing sleeps, nothing allocates, nothing takes a lock. */
static void usb_isr(void *arg)
{
    (void)arg;
    xhci_events();
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_dev[i].used && g_dev[i].drv && g_dev[i].drv->poll)
            g_dev[i].drv->poll(&g_dev[i]);
}

/* -------------------------------------------- device-model binding --- */

static struct device *g_hc;

static int xhci_probe(struct device *dev)
{
    if (g_hc) return -1;              /* one controller is all this drives */

    /* The interrupt window. dev_probe_all() runs with IF still clear, so
     * timer_ms() is frozen and every millisecond timeout below would fall back
     * to a raw spin count. Opening IF here gives the controller driver a real
     * time base; the caller's flags are restored before returning, so a probe
     * pass that expects interrupts masked still gets them masked. */
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
    __asm__ volatile ("sti");

    int rc = xhci_init(dev);
    if (rc == 0) {
        g_hc = dev;
        g_present = 1;
        usb_hid_register();

        int n = xhci_port_count();
        kprintf("[usb] scanning %d root ports\n", n);
        for (int p = 1; p <= n; p++)
            if (xhci_port_connected(p)) enumerate_port(p);

        /* MSI-X first, MSI next, legacy INTx last -- the device model chooses
         * and reports which. Only after a vector exists is the interrupter
         * allowed to raise anything. */
        int vec = dev_irq_request(dev, usb_isr, dev, "xhci");
        if (vec < 0) {
            kprintf("[usb] NO INTERRUPT COULD BE WIRED -- devices are enumerated "
                    "but no input will be delivered\n");
        } else {
            xhci_irq_enable();
            kprintf("USB_IRQ vec=%d mode=%d\n", vec, dev->irq_mode);
        }
        /* kprintf has no length modifiers -- %lu prints a literal "%lu". */
        kprintf("USB_READY devices=%d drivers=%d\n", (int)g_found, g_ndrv);
    } else {
        kprintf("[usb] controller bring-up failed; PS/2 input is unaffected\n");
    }

    if (!(fl & 0x200)) __asm__ volatile ("cli");
    return rc;
}

/* By CLASS, not by ID. 0x0C/0x03/0x30 is "serial bus controller / USB / xHCI
 * programming interface" -- the whole reason to match on prog-if is that every
 * xHCI on earth answers to it while no two chipsets share a device ID. */
static const struct dev_match xhci_ids[] = {
    DEV_MATCH_PROGIF(PCI_CLASS_SERIAL, 0x03, 0x30),
    DEV_MATCH_END
};

static struct driver xhci_driver = {
    .name = "xhci",
    .bus_type = DEV_BUS_PCI,
    .match = xhci_ids,
    .probe = xhci_probe,
};
DRIVER_DECLARE(xhci_driver);
