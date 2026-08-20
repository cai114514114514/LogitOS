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

/* A bound network DEVICE -- the driver's half. A driver returns a pointer to
 * its own static instance. Note what is NOT here: a name the network layer can
 * route to, an index, addresses, or flags. That is `struct netif` below, and
 * the split is the point: a driver owns a card, the stack owns an interface,
 * and a card that has not been given an address is still a card. */
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

/* ------------------------------------------------------------------------
 * INTERFACES.
 *
 * What this replaces: `static struct netdev *g_nic;`. One pointer, assigned by
 * whichever driver probed first, with every other card in the machine left
 * invisible -- not merely unused: unrepresentable. Four NIC drivers exist in
 * this tree and a machine with two cards could only ever be told about one.
 *
 * An interface is not a card. It has a name and a stable index the routing
 * table refers to (c/net/core/route.h, `int oif`), a set of flags, and its OWN
 * addresses -- plural, because that is what an interface actually has, and
 * because c/net/ip/ip6_addr.c already settled this argument on the v6 side:
 * `net_cfg.ip` (one address for the whole machine) is the wrong shape and this
 * file is not going to invent a second wrong one for v4. The loopback
 * interface has no card at all (`dev == NULL`), which is the cheapest possible
 * demonstration that the two concepts had to be separated.
 *
 * WHAT IS DELIBERATELY NOT HERE: a per-interface transmit from above IP. The
 * routing table can already select an interface, but eth_send() takes no
 * interface argument and neither does arp_output(), so a frame still leaves by
 * netdev_tx() = the primary card. netdev_tx_if() exists and is correct;
 * threading an ifindex down through c/net/link is a change to files this one
 * does not own. Stated rather than papered over, because "the table picks eth1"
 * and "the frame leaves by eth1" are different claims.
 * ------------------------------------------------------------------------ */

/* 4 = lo plus three cards. QEMU can be given more, and the bound-NIC loop
 * stops rather than overwriting: an interface that could not be registered is
 * reported at boot, which is a better failure than a table that silently holds
 * a different card than the log says. */
#define NETIF_MAX    4
#define NETIF_NADDR  4       /* IPv4 addresses per interface */
#define NETIF_NAMELEN 8

#define NETIF_F_UP        0x1u   /* administratively up */
#define NETIF_F_RUNNING   0x2u   /* the driver bound and the card answered */
#define NETIF_F_LOOPBACK  0x4u   /* delivers to this machine, has no wire */
#define NETIF_F_BROADCAST 0x8u   /* the link can carry a broadcast frame */

struct netif_addr {
    uint32_t addr;      /* host order */
    uint32_t mask;      /* host order */
};

struct netif {
    char     name[NETIF_NAMELEN];   /* "lo", "eth0", "eth1" */
    int      index;                 /* 1-based; 0 means "no such interface" */
    uint8_t  mac[6];                /* all zero on loopback */
    uint32_t flags;                 /* NETIF_F_* */
    struct netif_addr addr[NETIF_NADDR];
    int      naddr;
    struct netdev *dev;             /* NULL for loopback */
};

/* Register an interface. `dev` may be NULL (loopback). Returns the new index,
 * or -1 when the table is full. netdev_init() calls this; nothing else needs
 * to, and a driver must not -- a driver's job ends at dev_set_drvdata(). */
int netif_register(const char *name, struct netdev *dev, uint32_t flags);

int netif_count(void);                        /* registered interfaces */
struct netif *netif_by_index(int idx);        /* NULL if absent */
struct netif *netif_by_name(const char *name);

/* Give an interface an address. Returns 0, or -1 if the interface is unknown
 * or already holds NETIF_NADDR addresses. Re-adding an address that is already
 * there updates its mask instead of consuming a slot. */
int netif_addr_add(int idx, uint32_t addr, uint32_t mask);

/* The source address to use when sending to `dst` out of interface `oif`: the
 * first address on that interface whose own prefix contains `dst`, else the
 * interface's first address, else 0.
 *
 * This is the v4 shadow of ip6_select_source() (RFC 6724 s5) and it implements
 * exactly one of that algorithm's rules -- longest matching prefix. The others
 * are inapplicable or unrepresentable here: v4 has no address scopes, no
 * tentative/deprecated states and no temporary addresses, so five of the eight
 * rules have nothing to read. Writing the full ladder over fields that do not
 * exist would be a re-implementation with no inputs. */
uint32_t netif_src_for(int oif, uint32_t dst);

/* 1 if `a` is an address this machine holds on ANY interface. The receive
 * path's `dst != net_cfg.ip` test is what this is for; see the note in
 * netdev.c about why ip_input does not call it yet. */
int netif_is_local(uint32_t a);

/* The indices route.c and ip.c need. Both return 0 when there is no such
 * interface -- 0 is never a valid index, so a caller that ignores the failure
 * gets RT_EINVAL out of route_v4_iface rather than a plausible wrong answer. */
int netdev_primary_ifindex(void);    /* the first NIC that bound, or 0 */
int netdev_loopback_ifindex(void);   /* always RT_OIF_LO once init has run */

/* Transmit out of a named interface. Loopback and unknown indices return -1:
 * this function moves bytes to a card, and the loopback interface has no card.
 * (eth_send() already loops a frame addressed to our own MAC back into
 * eth_input; that is where local delivery belongs, not here.) */
int netdev_tx_if(int oif, const void *frame, uint16_t len);

/* Print the interface table (boot evidence: which cards were found, which one
 * is primary, and what each holds). */
void netif_dump(void);

/* Driver probes, one per translation unit. netdev.c owns the `struct driver`
 * objects (and the NIC-line priority order) so that the match tables can live
 * in one file the host test reads directly -- see net_ids.inc. */
int e1000_probe(struct device *dev);
int rtl8139_probe(struct device *dev);
int rtl8169_probe(struct device *dev);
int virtio_net_probe(struct device *dev);

#endif /* LOGIT_NETDEV_H */
