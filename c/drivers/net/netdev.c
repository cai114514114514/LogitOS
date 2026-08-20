#include <stdint.h>
#include <stddef.h>
#include "netdev.h"
#include "driver.h"
#include "pci.h"
#include "kprintf.h"
#include "route.h"

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

/* ------------------------------------------------------------------------
 * The interface table.
 *
 * This used to be `static struct netdev *g_nic;` -- one pointer, whichever
 * card probed first, every other card in the machine invisible. `g_nic` is
 * still here and still means "the primary NIC", because every forwarder at the
 * bottom of this file and the whole legacy e1000_* facade are defined in terms
 * of it and none of them has an interface to name; it is now derived from the
 * table rather than being the table.
 * ------------------------------------------------------------------------ */

static struct netif ifs[NETIF_MAX];
static int nif;                 /* registered interfaces; indices are 1..nif */
static int lo_if, primary_if;   /* 0 = not registered */
static struct netdev *g_nic;    /* the primary interface's device */
static const uint8_t zero_mac[6];

/* route.c memoises a configuration per interface index and refuses an index it
 * cannot store, so an interface table larger than its array would silently
 * lose the last cards' routes. Checked here, where both headers are in scope,
 * rather than hoped at from either side. */
_Static_assert(NETIF_MAX <= RT_NIF, "NETIF_MAX outgrew RT_NIF (route.h)");
/* c/net/ip/ip.c falls back to these constants when the driver layer is not
 * linked (a host test). They hold because netdev_init registers loopback
 * first, which this asserts is still what index 1 means. */
_Static_assert(RT_OIF_LO == 1 && RT_OIF_NIC0 == 2, "route.h index constants moved");

int netif_count(void) { return nif; }

struct netif *netif_by_index(int idx)
{
    if (idx < 1 || idx > nif) return NULL;
    return &ifs[idx - 1];
}

struct netif *netif_by_name(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < nif; i++) {
        const char *a = ifs[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return &ifs[i];
    }
    return NULL;
}

int netif_register(const char *name, struct netdev *dev, uint32_t flags)
{
    if (nif >= NETIF_MAX) return -1;
    struct netif *n = &ifs[nif];
    int i = 0;
    for (; name && name[i] && i < NETIF_NAMELEN - 1; i++) n->name[i] = name[i];
    n->name[i] = 0;
    n->index = nif + 1;
    n->dev = dev;
    n->flags = flags;
    n->naddr = 0;
    /* The MAC is COPIED, not aliased through ->dev: the loopback interface has
     * no device and must still answer netif queries with something, and a
     * caller holding a struct netif should not have to know which interfaces
     * have a card behind them to read six bytes. */
    for (int k = 0; k < 6; k++) n->mac[k] = dev ? dev->mac[k] : 0;
    nif++;
    return n->index;
}

int netif_addr_add(int idx, uint32_t addr, uint32_t mask)
{
    struct netif *n = netif_by_index(idx);
    if (!n) return -1;
    for (int i = 0; i < n->naddr; i++)
        if (n->addr[i].addr == addr) { n->addr[i].mask = mask; return 0; }
    if (n->naddr >= NETIF_NADDR) return -1;
    n->addr[n->naddr].addr = addr;
    n->addr[n->naddr].mask = mask;
    n->naddr++;
    return 0;
}

uint32_t netif_src_for(int oif, uint32_t dst)
{
    struct netif *n = netif_by_index(oif);
    if (!n || n->naddr <= 0) return 0;
    int best = -1, bestlen = -1;
    for (int i = 0; i < n->naddr; i++) {
        int plen = route_mask_plen(n->addr[i].mask);
        if (plen < 0) continue;                  /* a mask with a hole in it */
        if (((dst ^ n->addr[i].addr) & route_plen_mask(plen)) != 0) continue;
        if (plen > bestlen) { bestlen = plen; best = i; }
    }
    return best >= 0 ? n->addr[best].addr : n->addr[0].addr;
}

int netif_is_local(uint32_t a)
{
    for (int i = 0; i < nif; i++)
        for (int k = 0; k < ifs[i].naddr; k++)
            if (ifs[i].addr[k].addr == a) return 1;
    return 0;
}

int netdev_primary_ifindex(void)  { return primary_if; }
int netdev_loopback_ifindex(void) { return lo_if; }

static void ip4_print(const char *tag, uint32_t a)
{
    kprintf("%s%u.%u.%u.%u", tag, (a >> 24) & 255, (a >> 16) & 255,
            (a >> 8) & 255, a & 255);
}

void netif_dump(void)
{
    for (int i = 0; i < nif; i++) {
        struct netif *n = &ifs[i];
        kprintf("[net] if %d %s flags%s%s%s%s",
                n->index, n->name,
                (n->flags & NETIF_F_UP) ? " up" : "",
                (n->flags & NETIF_F_RUNNING) ? " running" : "",
                (n->flags & NETIF_F_LOOPBACK) ? " loopback" : "",
                (n->flags & NETIF_F_BROADCAST) ? " broadcast" : "");
        if (n->dev)
            kprintf("  mac %x:%x:%x:%x:%x:%x drv %s irq=%d",
                    n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4],
                    n->mac[5], n->dev->name, n->dev->irq_line);
        for (int k = 0; k < n->naddr; k++) {
            ip4_print("  ", n->addr[k].addr);
            kprintf("/%d", route_mask_plen(n->addr[k].mask));
        }
        kprintf("%s\n", n->index == primary_if ? "  [primary]" : "");
    }
}

int netdev_init(void)
{
    /* LOOPBACK FIRST, and it is not decoration. It fixes a real leak: before
     * the table existed, 127.0.0.1 failed ip.c's on-subnet test, so an ICMP
     * echo to it was ARP'd to the default gateway and put ON THE WIRE. It is
     * registered before any card so that its index is RT_OIF_LO by
     * construction (asserted above).
     *
     * 127.0.0.1/8 rather than /32: the whole 127/8 block is loopback (RFC
     * 1122 s3.2.1.3), so a /32 would leave 127.0.0.2 routed to the gateway --
     * the exact bug, still there, one address to the left. */
    if (!lo_if) {
        lo_if = netif_register("lo", NULL,
                               NETIF_F_UP | NETIF_F_RUNNING | NETIF_F_LOOPBACK);
        if (lo_if > 0) netif_addr_add(lo_if, 0x7F000001u, 0xFF000000u);
        /* The ROUTE is not installed here. Route installation lives at exactly
         * one site -- route_sync() in c/net/ip/ip.c -- because two installers
         * of the same row is how the two disagree later; this file's job ends
         * at "the interface exists and holds this address". */
    }

    int nfound = 0;
    for (int i = 0; i < NDRV; i++) {
        struct driver *drv = nic_order[i];
        for (int d = 0; d < dev_count(); d++) {
            struct device *dev = dev_at(d);
            if (!dev || dev->drv) continue;                 /* already claimed */
            const struct dev_match *m = dev_match_table(drv->match, dev);
            if (!m) continue;
            if (nif >= NETIF_MAX) {
                /* Refuse out loud rather than bind a card the table cannot
                 * name: a bound-but-unregistered NIC would receive frames that
                 * no routing decision could ever have chosen it for. */
                kprintf("[net] %s: %x:%x found but the interface table is full "
                        "(NETIF_MAX=%d); left unclaimed\n",
                        drv->name, dev->vendor, dev->device, NETIF_MAX);
                continue;
            }
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
            struct netdev *nd = (struct netdev *)dev_get_drvdata(dev);
            if (!nd) {                                      /* driver bug, not a device fault */
                dev->drv = NULL; dev->match = NULL;
                kprintf("[net] %s: probe returned 0 without a netdev\n", drv->name);
                continue;
            }
            /* "eth0", "eth1", ... -- numbered by BIND order, which is NIC-line
             * priority order (nic_order above), so eth0 is the best card in
             * the machine and not whichever slot the BIOS enumerated first. */
            char nm[NETIF_NAMELEN] = { 'e', 't', 'h', 0, 0 };
            nm[3] = (char)('0' + (nfound < 9 ? nfound : 9));
            int idx = netif_register(nm, nd,
                                     NETIF_F_UP | NETIF_F_RUNNING | NETIF_F_BROADCAST);
            if (idx < 0) continue;                          /* checked above; belt and braces */
            nfound++;
            if (!primary_if) { primary_if = idx; g_nic = nd; }
            kprintf("[net] NIC bound: %s = %s  MAC %x:%x:%x:%x:%x:%x  irq=%d\n",
                    nm, nd->name, nd->mac[0], nd->mac[1], nd->mac[2],
                    nd->mac[3], nd->mac[4], nd->mac[5], nd->irq_line);
            /* NO `return 0` HERE, and that single deleted line is most of this
             * change: the old loop stopped at the first card, so a second NIC
             * was not merely unused, it was never probed and never seen. */
#ifdef NETIF_NEGCTL_SINGLE
            /* NEGATIVE CONTROL (tests/route.mk test-netif-negctl): that line,
             * put back. Nothing else changes -- `lo` is still registered, the
             * first card still binds, and every single-NIC machine (which is
             * every machine this tree has ever been booted on) behaves
             * identically. That is exactly why the limitation survived: it is
             * invisible until the second card is in the slot.
             * tests/unit/netif_test.c MUST fail against this build. */
            netif_dump();
            return 0;
#endif
        }
    }

    if (!primary_if) {
        /* Not an error condition -- it is what a machine with an unsupported card
         * looks like, and the only correct response is to boot without networking.
         * net_init() returns -1 from here, net_up() stays 0, and every net syscall
         * refuses cleanly instead of touching a device that is not there.
         * dev_dump() will still list the card, by class, as unclaimed.
         * Note that `lo` is registered and routable either way: a machine with
         * no NIC still has a loopback interface, which is the whole reason
         * loopback is an interface and not a branch inside ip_send. */
        kprintf("[net] no supported NIC found; networking disabled\n");
        netif_dump();
        return -1;
    }
    netif_dump();
    return 0;
}

int netdev_present(void) { return g_nic != NULL; }
const char *netdev_name(void) { return g_nic ? g_nic->name : "none"; }
const uint8_t *netdev_mac(void) { return g_nic ? g_nic->mac : zero_mac; }

int netdev_tx(const void *frame, uint16_t len)
{
    if (!g_nic || !g_nic->tx) return -1;
    return g_nic->tx(frame, len);
}

int netdev_tx_if(int oif, const void *frame, uint16_t len)
{
    struct netif *n = netif_by_index(oif);
    if (!n || !n->dev || !n->dev->tx) return -1;
    return n->dev->tx(frame, len);
}

int netdev_rx_poll(net_rx_cb cb)
{
    /* Every bound card, not just the primary. A second NIC whose ring is never
     * drained fills up and then drops everything, silently, which is a worse
     * failure than not having bound it at all. */
    int n = 0;
    for (int i = 0; i < nif; i++)
        if (ifs[i].dev && ifs[i].dev->rx_poll) n += ifs[i].dev->rx_poll(cb);
    return n;
}

void netdev_irq_enable(net_rx_cb cb)
{
    /* A driver with no interrupt path is polled-only; net_poll() from the WM
     * loop already covers it, so this is a no-op rather than a failure. */
    for (int i = 0; i < nif; i++)
        if (ifs[i].dev && ifs[i].dev->irq_enable) ifs[i].dev->irq_enable(cb);
}

int netdev_irq_line(void)
{
    /* -1 when there is no NIC or the driver is polled-only: smp.c only routes
     * the I/O APIC entry for a line in (0, 24), so this disables the routing
     * without smp.c needing to know anything about NICs.
     *
     * STILL THE PRIMARY'S LINE ONLY. smp.c asks for one line and wires one
     * I/O APIC entry; a second card on a different GSI is therefore polled,
     * not interrupt-driven -- it works (net_poll from the WM loop is every
     * card's backstop) but its latency is the frame rate. Fixing it is a
     * change to c/kernel/cpu/smp.c, which this file does not own. */
    if (!g_nic || !g_nic->irq) return -1;
    return g_nic->irq_line;
}

void netdev_irq(void)
{
    /* Every card that can take one. PCI interrupt lines are shared, so a
     * vector-65 interrupt may belong to any of them and each driver's irq()
     * begins by reading its own device's cause register: the ones with nothing
     * pending cost a read and return. Calling only the primary would leave a
     * sharing card's interrupt unacked and the line asserted forever. */
    for (int i = 0; i < nif; i++)
        if (ifs[i].dev && ifs[i].dev->irq) ifs[i].dev->irq();
}

/* ------------------------------------------------------------------------
 * Legacy facade.
 *
 * `c/net/link/eth.c`, `c/net/core/net.c`, `c/kernel/cpu/interrupts.c` and
 * `c/kernel/cpu/smp.c` all called the NIC by the name `e1000_*` -- that is the
 * seam this whole file exists to generalise. So the e1000_* symbols stay, and
 * now mean "the bound NIC, whatever it is". They are pure forwarding; nothing
 * below knows about Intel.
 *
 * The two NET files have since been converted (eth.c -> netdev_tx, net.c ->
 * netdev_init/netdev_mac/netdev_irq_enable/netdev_rx_poll). What is left is
 * `c/kernel/cpu/interrupts.c` (vector 65 -> e1000_irq) and `c/kernel/cpu/smp.c`
 * (e1000_irq_line for the I/O APIC entry), which belong to the kernel lines.
 * When someone owns those two files:
 *     e1000_irq_line()    -> netdev_irq_line()
 *     e1000_irq()         -> netdev_irq()
 * plus `#include "e1000.h"` -> `#include "netdev.h"`. Two lines each; this
 * block can then be deleted.
 * ------------------------------------------------------------------------ */

int  e1000_init(void)                 { return netdev_init(); }
const uint8_t *e1000_mac(void)        { return netdev_mac(); }
int  e1000_tx(const void *f, uint16_t l) { return netdev_tx(f, l); }
int  e1000_rx_poll(net_rx_cb cb)      { return netdev_rx_poll(cb); }
void e1000_irq_enable(net_rx_cb cb)   { netdev_irq_enable(cb); }
int  e1000_irq_line(void)             { return netdev_irq_line(); }
void e1000_irq(void)                  { netdev_irq(); }
