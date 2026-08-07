#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "driver.h"
#include "pci.h"
#include "kprintf.h"

/* The NIC line: which drivers exist, in what order they get a look at a card,
 * and where the link layer's calls go once one is bound.
 *
 * Matching itself is the device model's job -- these are `struct driver`s with
 * `struct dev_match` tables, DRIVER_DECLARE'd like every other driver, and
 * dev_match_table() decides what claims what. The only thing this file adds on
 * top is an ordered early pass, for the timing reason in netdev.h.
 */

#include "net_ids.inc"

static struct driver virtio_net_driver = {
    .name = "virtio-net", .bus_type = DEV_BUS_PCI,
    .match = virtio_net_ids, .probe = virtio_net_probe,
};
static struct driver e1000_driver = {
    .name = "e1000", .bus_type = DEV_BUS_PCI,
    .match = e1000_ids, .probe = e1000_probe,
};
static struct driver rtl8139_driver = {
    .name = "rtl8139", .bus_type = DEV_BUS_PCI,
    .match = rtl8139_ids, .probe = rtl8139_probe,
};
static struct driver rtl8169_driver = {
    .name = "rtl8169", .bus_type = DEV_BUS_PCI,
    .match = rtl8169_ids, .probe = rtl8169_probe,
};

DRIVER_DECLARE(virtio_net_driver);
DRIVER_DECLARE(e1000_driver);
DRIVER_DECLARE(rtl8139_driver);
DRIVER_DECLARE(rtl8169_driver);

/* NIC-line priority, by how well the device is likely to work rather than
 * alphabetically: a paravirtual NIC beats an emulated one on any hypervisor
 * that offers both, and both beat a card we have never run. dev_probe_all()
 * uses registration order instead, which is not under our control -- so the
 * early pass keeps its own list. */
static struct driver *const nic_order[] = {
    &virtio_net_driver, &e1000_driver, &rtl8139_driver, &rtl8169_driver,
};
#define NDRV ((int)(sizeof nic_order / sizeof nic_order[0]))

static struct netdev *g_nic;
static const uint8_t zero_mac[6];

int netdev_init(void)
{
    for (int i = 0; i < NDRV; i++) {
        struct driver *drv = nic_order[i];
        for (int d = 0; d < dev_count(); d++) {
            struct device *dev = dev_at(d);
            if (!dev || dev->drv) continue;                 /* already claimed */
            const struct dev_match *m = dev_match_table(drv->match, dev);
            if (!m) continue;
            kprintf("[net] %s: found %x:%x at %s\n",
                    drv->name, dev->vendor, dev->device, dev->name);
            dev->drv = drv; dev->match = m;                 /* visible to probe() */
            if (drv->probe(dev) != 0) {
                /* Matched but would not come up. Leave the device free and keep
                 * looking: a machine with a broken card and a working one should
                 * use the working one. */
                dev->drv = NULL; dev->match = NULL; dev->drvdata = NULL;
                kprintf("[net] %s: probe failed, trying other drivers\n", drv->name);
                continue;
            }
            g_nic = (struct netdev *)dev_get_drvdata(dev);
            if (!g_nic) {                                   /* driver bug, not a device fault */
                dev->drv = NULL; dev->match = NULL;
                kprintf("[net] %s: probe returned 0 without a netdev\n", drv->name);
                continue;
            }
            kprintf("[net] NIC bound: %s  MAC %x:%x:%x:%x:%x:%x  irq=%d\n",
                    g_nic->name, g_nic->mac[0], g_nic->mac[1], g_nic->mac[2],
                    g_nic->mac[3], g_nic->mac[4], g_nic->mac[5], g_nic->irq_line);
            return 0;
        }
    }
    /* Not an error condition -- it is what a machine with an unsupported card
     * looks like, and the only correct response is to boot without networking.
     * net_init() returns -1 from here, net_up() stays 0, and every net syscall
     * refuses cleanly instead of touching a device that is not there.
     * dev_dump() will still list the card, by class, as unclaimed. */
    kprintf("[net] no supported NIC found; networking disabled\n");
    return -1;
}

int netdev_present(void) { return g_nic != NULL; }
const char *netdev_name(void) { return g_nic ? g_nic->name : "none"; }
const uint8_t *netdev_mac(void) { return g_nic ? g_nic->mac : zero_mac; }

int netdev_tx(const void *frame, uint16_t len)
{
    if (!g_nic || !g_nic->tx) return -1;
    return g_nic->tx(frame, len);
}

int netdev_rx_poll(net_rx_cb cb)
{
    if (!g_nic || !g_nic->rx_poll) return 0;
    return g_nic->rx_poll(cb);
}

void netdev_irq_enable(net_rx_cb cb)
{
    /* A driver with no interrupt path is polled-only; net_poll() from the WM
     * loop already covers it, so this is a no-op rather than a failure. */
    if (g_nic && g_nic->irq_enable) g_nic->irq_enable(cb);
}

int netdev_irq_line(void)
{
    /* -1 when there is no NIC or the driver is polled-only: smp.c only routes
     * the I/O APIC entry for a line in (0, 24), so this disables the routing
     * without smp.c needing to know anything about NICs. */
    if (!g_nic || !g_nic->irq) return -1;
    return g_nic->irq_line;
}

void netdev_irq(void)
{
    if (g_nic && g_nic->irq) g_nic->irq();
}

/* ------------------------------------------------------------------------
 * Legacy facade.
 *
 * `c/net/link/eth.c`, `c/net/core/net.c`, `c/kernel/cpu/interrupts.c` and
 * `c/kernel/cpu/smp.c` all call the NIC by the name `e1000_*` -- that is the
 * seam this whole file exists to generalise, and those four files are owned by
 * other people. So the e1000_* symbols stay, and now mean "the bound NIC,
 * whatever it is". They are pure forwarding; nothing below knows about Intel.
 *
 * The rename those callers want, when someone owns them:
 *     e1000_init()        -> netdev_init()
 *     e1000_mac()         -> netdev_mac()
 *     e1000_tx()          -> netdev_tx()
 *     e1000_rx_poll()     -> netdev_rx_poll()
 *     e1000_irq_enable()  -> netdev_irq_enable()
 *     e1000_irq_line()    -> netdev_irq_line()
 *     e1000_irq()         -> netdev_irq()
 * plus `#include "e1000.h"` -> `#include "netdev.h"`. It is a mechanical
 * substitution in five lines of eth.c/net.c and two of interrupts.c/smp.c;
 * this block can then be deleted.
 * ------------------------------------------------------------------------ */

int  e1000_init(void)                 { return netdev_init(); }
const uint8_t *e1000_mac(void)        { return netdev_mac(); }
int  e1000_tx(const void *f, uint16_t l) { return netdev_tx(f, l); }
int  e1000_rx_poll(net_rx_cb cb)      { return netdev_rx_poll(cb); }
void e1000_irq_enable(net_rx_cb cb)   { netdev_irq_enable(cb); }
int  e1000_irq_line(void)             { return netdev_irq_line(); }
void e1000_irq(void)                  { netdev_irq(); }
