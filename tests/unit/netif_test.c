/* The interface table, on the host.
 *
 * What it is measuring: c/drivers/net/netdev.c used to hold `static struct
 * netdev *g_nic;` and `return 0;` at the first successful probe. Four NIC
 * drivers exist in this tree, and on a machine with two cards the second was
 * not merely unused -- it was never probed, never named, and could not be
 * referred to by anything. There was no test, because there was nothing to
 * refer to.
 *
 * White-box in this tree's usual shape: it #includes netdev.c and stubs the
 * SEVEN symbols genuinely below it -- dev_count/dev_at/dev_match_table from
 * the device model, and the four driver probes. The device registry is
 * synthetic (four cards, one per driver), so netdev_init() runs its real
 * priority loop over real match tables read from net_ids.inc, and what is
 * checked is the binding this kernel would actually perform.
 *
 * THE PRIORITY ORDER IS PART OF THE TEST, and it is the reason the synthetic
 * registry lists the cards in the WRONG order on purpose: the e1000 is at
 * device index 0 and the virtio-net at index 1, so an implementation that
 * bound in enumeration order would still bind something plausible, name it
 * eth0, and pass any test that only asked "is there a NIC".
 *
 * NEGATIVE CONTROL: -DNETIF_NEGCTL_SINGLE puts the `return 0;` back.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "driver.h"
#include "netdev.h"

/* --- the synthetic device registry --------------------------------------- */

#define E1000_VD   0x8086, 0x100E
#define VIRTIO_VD  0x1AF4, 0x1000
#define RTL8139_VD 0x10EC, 0x8139
#define RTL8169_VD 0x10EC, 0x8168

static struct device devs[4] = {
    /* index 0 first, and it is deliberately NOT the highest-priority card. */
    { .name = "0000:00:03.0", .bus_type = DEV_BUS_PCI, .vendor = 0x8086,
      .device = 0x100E, .irq_line = 11 },
    { .name = "0000:00:04.0", .bus_type = DEV_BUS_PCI, .vendor = 0x1AF4,
      .device = 0x1000, .irq_line = 10 },
    { .name = "0000:00:05.0", .bus_type = DEV_BUS_PCI, .vendor = 0x10EC,
      .device = 0x8139, .irq_line = 9 },
    { .name = "0000:00:06.0", .bus_type = DEV_BUS_PCI, .vendor = 0x10EC,
      .device = 0x8168, .irq_line = 5 },
};
static int ndev = 4;

int dev_count(void) { return ndev; }
struct device *dev_at(int i) { return (i >= 0 && i < ndev) ? &devs[i] : NULL; }

/* The real dev_match_table lives in c/drivers/core/device.c and is covered by
 * test-nic-drv against these same tables. Here it only has to answer
 * vendor+device, which is what all four NIC tables use. */
static struct dev_match matched;
const struct dev_match *dev_match_table(const struct dev_match *tbl,
                                        const struct device *dev)
{
    for (; tbl && (tbl->vendor || tbl->device || tbl->class_code); tbl++)
        if (tbl->vendor == dev->vendor && tbl->device == dev->device) {
            matched = *tbl;
            return &matched;
        }
    return NULL;
}

/* --- four fake cards ------------------------------------------------------ */

#define NFAKE 4
static struct { int tx, rx, irq, irq_en; } counted[NFAKE];
static int last_tx = -1;

static int tx0(const void *f, uint16_t l) { (void)f;(void)l; counted[0].tx++; last_tx = 0; return 0; }
static int tx1(const void *f, uint16_t l) { (void)f;(void)l; counted[1].tx++; last_tx = 1; return 0; }
static int tx2(const void *f, uint16_t l) { (void)f;(void)l; counted[2].tx++; last_tx = 2; return 0; }
static int tx3(const void *f, uint16_t l) { (void)f;(void)l; counted[3].tx++; last_tx = 3; return 0; }
/* Each card reports a DIFFERENT number of frames drained, so the sum names
 * which cards were polled -- 1+2+4 is not reachable by any other subset. */
static int rx0(net_rx_cb cb) { (void)cb; counted[0].rx++; return 1; }
static int rx1(net_rx_cb cb) { (void)cb; counted[1].rx++; return 2; }
static int rx2(net_rx_cb cb) { (void)cb; counted[2].rx++; return 4; }
static int rx3(net_rx_cb cb) { (void)cb; counted[3].rx++; return 8; }
static void irq0(void) { counted[0].irq++; }
static void irq1(void) { counted[1].irq++; }
static void irq2(void) { counted[2].irq++; }
static void irq3(void) { counted[3].irq++; }
static void en0(net_rx_cb c) { (void)c; counted[0].irq_en++; }
static void en1(net_rx_cb c) { (void)c; counted[1].irq_en++; }
static void en2(net_rx_cb c) { (void)c; counted[2].irq_en++; }
static void en3(net_rx_cb c) { (void)c; counted[3].irq_en++; }

static struct netdev fake[NFAKE] = {
    { .name = "e1000",      .mac = {0x52,0x54,0,0,0,0xE1}, .irq_line = 11,
      .tx = tx0, .rx_poll = rx0, .irq = irq0, .irq_enable = en0 },
    { .name = "virtio-net", .mac = {0x52,0x54,0,0,0,0x71}, .irq_line = 10,
      .tx = tx1, .rx_poll = rx1, .irq = irq1, .irq_enable = en1 },
    { .name = "rtl8139",    .mac = {0x52,0x54,0,0,0,0x39}, .irq_line = 9,
      .tx = tx2, .rx_poll = rx2, .irq = irq2, .irq_enable = en2 },
    { .name = "rtl8169",    .mac = {0x52,0x54,0,0,0,0x69}, .irq_line = 5,
      .tx = tx3, .rx_poll = rx3, .irq = irq3, .irq_enable = en3 },
};

static int bind_fake(struct device *dev, int which)
{
    dev_set_drvdata(dev, &fake[which]);
    return 0;
}
int e1000_probe(struct device *d)      { return bind_fake(d, 0); }
int virtio_net_probe(struct device *d) { return bind_fake(d, 1); }
int rtl8139_probe(struct device *d)    { return bind_fake(d, 2); }
int rtl8169_probe(struct device *d)    { return bind_fake(d, 3); }

#include "route.c"
#include "netdev.c"

/* ------------------------------------------------------------------------ */

static int checks, fails;
#define CHECK(cond, msg) do { checks++; \
    if (cond) printf("ok   %s\n", (msg)); \
    else { printf("FAIL %s (%s:%d)\n", (msg), __FILE__, __LINE__); fails++; } } while (0)

#define IPV4T(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)| \
                        ((uint32_t)(c)<<8)|(uint32_t)(d))

int main(void)
{
#ifdef NETIF_NEGCTL_SINGLE
    printf("netif_test: NEGATIVE CONTROL build (NETIF_NEGCTL_SINGLE)\n");
#endif
    int rc = netdev_init();
    CHECK(rc == 0, "netdev_init bound a NIC");

    /* ---- loopback is a real interface, and it is first ------------------ */
    struct netif *lo = netif_by_index(RT_OIF_LO);
    CHECK(lo != NULL, "index RT_OIF_LO exists");
    CHECK(lo && strcmp(lo->name, "lo") == 0, "index 1 is 'lo' -- registered before any card");
    CHECK(lo && (lo->flags & NETIF_F_LOOPBACK), "lo carries NETIF_F_LOOPBACK");
    CHECK(lo && lo->dev == NULL, "lo has no device: an interface is not a card");
    CHECK(lo && lo->naddr == 1 && lo->addr[0].addr == IPV4T(127,0,0,1) &&
          lo->addr[0].mask == IPV4T(255,0,0,0),
          "lo holds 127.0.0.1/8 -- the whole block, not a /32");
    CHECK(netdev_loopback_ifindex() == RT_OIF_LO,
          "netdev_loopback_ifindex agrees with route.h's constant");

    /* ---- every card is bound, in PRIORITY order ------------------------- */
    CHECK(netif_count() == NETIF_MAX,
          "the table is full: lo plus three of the four cards");
    struct netif *e0 = netif_by_name("eth0");
    struct netif *e1 = netif_by_name("eth1");
    struct netif *e2 = netif_by_name("eth2");
    CHECK(e0 && e0->dev && strcmp(e0->dev->name, "virtio-net") == 0,
          "eth0 is the virtio NIC -- NIC-line priority, not enumeration order");
    CHECK(e1 && e1->dev && strcmp(e1->dev->name, "e1000") == 0,
          "eth1 is the e1000, which enumerated FIRST and is second in priority");
    CHECK(e2 && e2->dev && strcmp(e2->dev->name, "rtl8139") == 0,
          "eth2 is the rtl8139");
    CHECK(netif_by_name("eth3") == NULL,
          "the fourth card is refused: the table is full and says so");
    CHECK(devs[3].drv == NULL,
          "and the refused card is left UNCLAIMED for dev_probe_all to report");
    CHECK(e0 && e0->index == RT_OIF_NIC0,
          "the primary NIC's index is route.h's RT_OIF_NIC0");
    CHECK(netdev_primary_ifindex() == RT_OIF_NIC0, "netdev_primary_ifindex agrees");
    CHECK(e0 && memcmp(e0->mac, fake[1].mac, 6) == 0,
          "the interface copied the card's MAC");
    CHECK(memcmp(netdev_mac(), fake[1].mac, 6) == 0,
          "netdev_mac() is the PRIMARY's MAC, unchanged in meaning");
    CHECK(strcmp(netdev_name(), "virtio-net") == 0, "netdev_name() is the primary's");
    CHECK(netdev_present() == 1, "netdev_present() still means 'a NIC is bound'");

    /* ---- transmit -------------------------------------------------------- */
    uint8_t frame[64] = { 0 };
    last_tx = -1;
    CHECK(netdev_tx(frame, sizeof frame) == 0 && last_tx == 1,
          "netdev_tx still goes to the primary card");
    last_tx = -1;
    CHECK(netdev_tx_if(3, frame, sizeof frame) == 0 && last_tx == 0,
          "netdev_tx_if(eth1) reaches the SECOND card -- unreachable before");
    CHECK(netdev_tx_if(RT_OIF_LO, frame, sizeof frame) == -1,
          "netdev_tx_if refuses loopback: it has no card to hand bytes to");
    CHECK(netdev_tx_if(99, frame, sizeof frame) == -1,
          "netdev_tx_if refuses an index that is not an interface");
    CHECK(netdev_tx_if(0, frame, sizeof frame) == -1,
          "netdev_tx_if refuses index 0, which is never valid");

    /* ---- receive: EVERY ring is drained ---------------------------------- */
    for (int i = 0; i < NFAKE; i++) counted[i].rx = 0;
    int n = netdev_rx_poll(NULL);
    CHECK(n == 1 + 2 + 4,
          "netdev_rx_poll drains all three cards (1+2+4; no other subset sums to 7)");
    CHECK(counted[3].rx == 0, "and does not touch the card that never bound");

    netdev_irq();
    CHECK(counted[0].irq == 1 && counted[1].irq == 1 && counted[2].irq == 1,
          "netdev_irq acks every card -- PCI INTx lines are shared");
    netdev_irq_enable(NULL);
    CHECK(counted[0].irq_en == 1 && counted[1].irq_en == 1 && counted[2].irq_en == 1,
          "netdev_irq_enable arms every card");
    CHECK(netdev_irq_line() == 10,
          "netdev_irq_line is still the primary's line (smp.c wires one entry)");

    /* ---- addresses are the INTERFACE's, plural --------------------------- */
    CHECK(netif_addr_add(RT_OIF_NIC0, IPV4T(10,0,2,15), IPV4T(255,255,255,0)) == 0,
          "an address can be given to an interface");
    CHECK(netif_addr_add(RT_OIF_NIC0, IPV4T(192,168,50,7), IPV4T(255,255,255,0)) == 0,
          "and a SECOND one, which net_cfg.ip cannot represent at all");
    CHECK(e0 && e0->naddr == 2, "the interface holds both");
    CHECK(netif_addr_add(RT_OIF_NIC0, IPV4T(10,0,2,15), IPV4T(255,255,0,0)) == 0 &&
          e0->naddr == 2 && e0->addr[0].mask == IPV4T(255,255,0,0),
          "re-adding an address updates its mask instead of consuming a slot");
    netif_addr_add(RT_OIF_NIC0, IPV4T(10,0,2,15), IPV4T(255,255,255,0));

    CHECK(netif_src_for(RT_OIF_NIC0, IPV4T(192,168,50,99)) == IPV4T(192,168,50,7),
          "netif_src_for picks the address whose own prefix contains the dst");
    CHECK(netif_src_for(RT_OIF_NIC0, IPV4T(10,0,2,9)) == IPV4T(10,0,2,15),
          "and the other one for the other subnet");
    CHECK(netif_src_for(RT_OIF_NIC0, IPV4T(8,8,8,8)) == IPV4T(10,0,2,15),
          "a destination in neither prefix falls back to the first address");
    CHECK(netif_src_for(RT_OIF_LO, IPV4T(127,0,0,9)) == IPV4T(127,0,0,1),
          "loopback sources from 127.0.0.1");
    CHECK(netif_src_for(99, IPV4T(8,8,8,8)) == 0,
          "an unknown interface has no source address, and says 0 rather than guessing");

    CHECK(netif_is_local(IPV4T(127,0,0,1)) == 1, "127.0.0.1 is ours");
    CHECK(netif_is_local(IPV4T(192,168,50,7)) == 1,
          "so is the SECOND address of the primary card");
    CHECK(netif_is_local(IPV4T(8,8,8,8)) == 0, "8.8.8.8 is not");

    /* NETIF_NADDR is 4 and two are used; the fifth must be refused. */
    netif_addr_add(RT_OIF_NIC0, IPV4T(172,16,0,1), IPV4T(255,255,0,0));
    netif_addr_add(RT_OIF_NIC0, IPV4T(172,17,0,1), IPV4T(255,255,0,0));
    CHECK(netif_addr_add(RT_OIF_NIC0, IPV4T(172,18,0,1), IPV4T(255,255,0,0)) == -1,
          "a full address list refuses rather than overwriting an address");
    CHECK(netif_addr_add(77, IPV4T(1,2,3,4), IPV4T(255,0,0,0)) == -1,
          "an address cannot be given to an interface that does not exist");

    printf("\nnetif_test: %d checks, %d failed\n", checks, fails);
    if (fails) { printf("netif_test: FAILURES\n"); return 1; }
    printf("netif_test: ALL PASS\n");
    return 0;
}
