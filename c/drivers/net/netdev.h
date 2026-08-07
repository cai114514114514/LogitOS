#ifndef LOGIT_NETDEV_H
#define LOGIT_NETDEV_H

#include <stdint.h>
#include "driver.h"

/* The common NIC interface.
 *
 * Before this existed the link layer called e1000_tx() by name, so the OS had
 * networking on exactly one PCI ID (8086:100E) and none at all anywhere else.
 * Now `c/net/link/` talks to whatever card probed successfully.
 *
 * Matching and enumeration are NOT done here -- they belong to the device model
 * (c/drivers/core/driver.h). Each NIC driver is a `struct driver` with a
 * `struct dev_match` table and a probe(), declared with DRIVER_DECLARE like any
 * other driver. What this header adds is the piece the device model has no
 * opinion about: the CLASS interface, i.e. what a bound network card can do --
 * send a frame, drain received ones, take an interrupt. `struct netdev` is to
 * NICs what a block-device vtable is to disks.
 *
 * A probe binds by calling dev_set_drvdata(dev, &its_netdev) and returning 0.
 */

/* Handed one received frame, already stripped of any device-private header.
 * The buffer is only valid for the duration of the call. */
typedef void (*net_rx_cb)(const uint8_t *frame, uint16_t len);

/* A bound network interface. A driver returns a pointer to its own static
 * instance; there is one active NIC. */
struct netdev {
    const char *name;                    /* "virtio-net", "e1000", "rtl8139"... */
    uint8_t     mac[6];
    int         irq_line;                /* PCI IRQ line (GSI), or -1 if unknown */

    int  (*tx)(const void *frame, uint16_t len);   /* one complete Ethernet frame */
    int  (*rx_poll)(net_rx_cb cb);                 /* drain RX; returns frames delivered */

    /* Optional. irq_enable() unmasks the device's RX interrupt and remembers cb;
     * irq() is called from the vector-65 ISR to ack the device and drain. A
     * driver that leaves both NULL is polled-only and still works -- net_poll()
     * from the WM loop is the backstop for every card. */
    void (*irq_enable)(net_rx_cb cb);
    void (*irq)(void);
};

/* Bind a NIC: walk the device registry that pci_init() built, in NIC-line
 * priority order, and bring up the first card one of our drivers claims.
 *
 * This exists as a separate pass rather than relying on dev_probe_all() purely
 * because of WHEN the network has to come up: kmain calls net_init() before
 * smp_init(), and dev_probe_all() has to run after it (wiring an interrupt
 * needs a live LAPIC). The probes are idempotent and this pass records the
 * binding in the device model, so the later dev_probe_all() sees the card as
 * already bound, skips it, and dev_dump() still reports the right driver.
 *
 * Returns 0 if a card was bound, -1 if the machine has no NIC this kernel can
 * drive -- a normal outcome, not a failure: the caller leaves the stack down
 * and the system boots without networking.
 */
int netdev_init(void);

const char    *netdev_name(void);     /* "none" when nothing bound */
const uint8_t *netdev_mac(void);      /* always a valid 6-byte pointer */
int  netdev_present(void);
int  netdev_tx(const void *frame, uint16_t len);
int  netdev_rx_poll(net_rx_cb cb);
void netdev_irq_enable(net_rx_cb cb);
int  netdev_irq_line(void);
void netdev_irq(void);

/* Driver probes, one per translation unit. netdev.c owns the `struct driver`
 * objects (and the NIC-line priority order) so that the match tables can live
 * in one file the host test reads directly -- see net_ids.inc. */
int e1000_probe(struct device *dev);
int rtl8139_probe(struct device *dev);
int rtl8169_probe(struct device *dev);
int virtio_net_probe(struct device *dev);

#endif /* LOGIT_NETDEV_H */
