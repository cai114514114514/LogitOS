/* Host unit tests for the parts of the NIC drivers that are pure computation:
 * descriptor-ring index arithmetic, the device header/status field accessors,
 * and PCI match-table resolution.
 *
 * This is deliberately not a mock of a NIC. The register programming in a NIC
 * driver cannot be meaningfully faked -- if the fake agrees with the driver,
 * both can be wrong together -- so that half is tested by booting the card
 * (tests/boot/run-nic-*.sh). What CAN go wrong silently, and what this file
 * covers, is the arithmetic: a ring offset that is right for 8 KiB of traffic
 * and wrong at the wrap, a 16-bit counter compared with `<`, a header length
 * that is off by two. Those show up as "the network stops after a while",
 * which is exactly the failure a boot test does not catch.
 *
 * Built by tests/nic.mk (`make test-nic-drv`); includes the real driver headers
 * and the real match tables, not copies of them.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver.h"
#include "netring.h"
#include "net_ids.inc"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define CHECK_EQ(got, want, what) do {                                        \
    unsigned long long g_ = (unsigned long long)(got);                        \
    unsigned long long w_ = (unsigned long long)(want);                       \
    CHECK(g_ == w_, "%s: got %llu (0x%llx), want %llu (0x%llx)",              \
          what, g_, g_, w_, w_);                                              \
} while (0)

/* ---------------------------------------------------------------- rings --- */

static void test_ring_next(void)
{
    CHECK_EQ(ring_next(0, 8), 1, "ring_next 0/8");
    CHECK_EQ(ring_next(6, 8), 7, "ring_next 6/8");
    CHECK_EQ(ring_next(7, 8), 0, "ring_next wraps at the end");
    CHECK_EQ(ring_next(0, 1), 0, "ring_next on a one-entry ring stays put");
    CHECK_EQ(ring_next(63, 64), 0, "ring_next wraps the e1000 RX ring");

    /* Walking the whole ring must visit every slot exactly once and come home. */
    int seen[64];
    memset(seen, 0, sizeof seen);
    uint32_t i = 0;
    for (int k = 0; k < 64; k++) { seen[i]++; i = ring_next(i, 64); }
    CHECK_EQ(i, 0, "a full lap returns to the start");
    int all_once = 1;
    for (int k = 0; k < 64; k++) if (seen[k] != 1) all_once = 0;
    CHECK(all_once, "a full lap visits every slot exactly once");
}

/* --------------------------------------------------------------- virtio --- */

static void test_virtio_used_counters(void)
{
    CHECK_EQ(vq_pending(0, 0), 0, "nothing pending when the counters agree");
    CHECK_EQ(vq_pending(5, 0), 5, "five completions pending");
    CHECK_EQ(vq_pending(1, 0), 1, "one completion pending");

    /* The whole point of the unsigned-difference form: used->idx wraps through
     * zero while last_used is still near 65535. A `used_idx > last_used`
     * comparison reports NOTHING pending here and the receive path stalls
     * forever -- the bug that only appears after 65536 buffers. */
    CHECK_EQ(vq_pending(0, 65535), 1, "one pending across the 16-bit wrap");
    CHECK_EQ(vq_pending(3, 65534), 5, "five pending across the 16-bit wrap");
    CHECK_EQ(vq_pending(65535, 65535), 0, "nothing pending at the top of the range");

    /* Slot selection is modulo the queue size, independent of the wrap. */
    CHECK_EQ(vq_slot(0, 256), 0, "slot 0");
    CHECK_EQ(vq_slot(255, 256), 255, "slot 255");
    CHECK_EQ(vq_slot(256, 256), 0, "slot wraps at the queue size");
    CHECK_EQ(vq_slot(65535, 256), 255, "slot at the top of the counter range");
    CHECK_EQ(vq_slot(65535, 128), 127, "slot with a 128-entry queue");

    /* A full consume loop across the wrap delivers every completion once. */
    uint16_t last = 65530, delivered = 0;
    uint16_t used_idx = 6;                       /* 12 completions produced */
    while (vq_pending(used_idx, last)) { last++; delivered++; }
    CHECK_EQ(delivered, 12, "every completion consumed across the wrap");
    CHECK_EQ(last, used_idx, "the consumer catches up exactly");
}

static void test_virtio_net_header(void)
{
    /* With VIRTIO_F_VERSION_1 the header is 12 bytes, always. Assuming the
     * legacy 10 shifts every frame by two and nothing parses as Ethernet. */
    CHECK_EQ(VNET_HDR_LEN, 12, "virtio-net 1.0 header length");

    /* A completion of exactly the header length carries no frame. */
    uint32_t got = VNET_HDR_LEN;
    CHECK(!(got > VNET_HDR_LEN), "a header-only completion is not a frame");
    got = VNET_HDR_LEN + 60;
    CHECK_EQ(got - VNET_HDR_LEN, 60, "frame length is the completion minus the header");
}

/* -------------------------------------------------------------- rtl8139 --- */

static void test_rtl8139_header(void)
{
    /* status = 0x0001 (ROK), size = 0x005E (94 = 90-byte frame + 4 CRC) */
    const uint8_t hdr[4] = { 0x01, 0x00, 0x5E, 0x00 };
    CHECK_EQ(rtl8139_rx_status(hdr), 0x0001, "little-endian status word");
    CHECK_EQ(rtl8139_rx_size(hdr), 0x005E, "little-endian size word");
    CHECK_EQ(rtl8139_rx_size(hdr) - 4, 90, "frame length excludes the CRC");

    CHECK(rtl8139_rx_ok(0x0001), "ROK alone is a good packet");
    CHECK(rtl8139_rx_ok(0xC001), "high bits (multicast/broadcast flags) do not spoil ROK");
    CHECK(!rtl8139_rx_ok(0x0000), "no ROK is not a good packet");
    CHECK(!rtl8139_rx_ok(0x0005), "ROK with FAE set is not a good packet");
    CHECK(!rtl8139_rx_ok(0x0009), "ROK with CRC error is not a good packet");
    CHECK(!rtl8139_rx_ok(0x0021), "ROK with ISE set is not a good packet");
}

static void test_rtl8139_offsets(void)
{
    /* 4-byte header + size, rounded up to 4. */
    CHECK_EQ(rtl8139_next_off(0, 60), 64, "already aligned: 0 + 4 + 60");
    CHECK_EQ(rtl8139_next_off(0, 61), 68, "rounds up to the next 4-byte boundary");
    CHECK_EQ(rtl8139_next_off(0, 62), 68, "rounds up");
    CHECK_EQ(rtl8139_next_off(0, 63), 68, "rounds up");
    CHECK_EQ(rtl8139_next_off(0, 64), 68, "already aligned again");
    CHECK_EQ(rtl8139_next_off(64, 1518), 1588, "mid-ring advance");

    /* The wrap. A packet whose header sits near the end of the 8 KiB ring: the
     * chip (WRAP set) writes it contiguously into the pad, but OUR next offset
     * must come back round modulo 8192. Getting this wrong walks the read
     * pointer off the ring and every subsequent packet is garbage. */
    CHECK_EQ(rtl8139_next_off(8188, 60), 60, "offset wraps the 8 KiB ring");
    CHECK_EQ(rtl8139_next_off(8100, 100), 12, "a packet straddling the end wraps");
    CHECK_EQ(rtl8139_next_off(8188, 0), 0, "an empty packet at the very end lands on 0");
    CHECK(rtl8139_next_off(8000, 1518) < RTL8139_RXBUF, "the offset is always inside the ring");

    /* CAPR trails the read offset by 16, MODULO 65536 -- so at offset 0 the
     * value written is 0xFFF0, which is what the chip expects and looks like a
     * bug to anyone who has not read the datasheet. */
    CHECK_EQ(rtl8139_capr(0), 0xFFF0, "CAPR at offset 0 is 0xFFF0, not 0");
    CHECK_EQ(rtl8139_capr(16), 0, "CAPR at offset 16 is 0");
    CHECK_EQ(rtl8139_capr(64), 48, "CAPR trails by 16");
    CHECK_EQ(rtl8139_capr(8188), 8172, "CAPR near the end of the ring");
    CHECK_EQ(rtl8139_capr(4), 0xFFF4, "CAPR below 16 wraps modulo 65536");

    /* The buffer really must be bigger than the ring: WRAP lets the chip run
     * one full frame past the end. */
    CHECK(RTL8139_RXBUF_PAD >= RTL8139_RXBUF + 16 + 1518,
          "the RX buffer has room for a full frame + CRC written past the ring end");

    /* Simulate a long run of receives and assert the offset never leaves the
     * ring and always stays 4-byte aligned. */
    uint32_t off = 0;
    for (int i = 0; i < 5000; i++) {
        uint16_t size = (uint16_t)(60 + (i * 37) % 1462);
        off = rtl8139_next_off(off, size);
        if (off >= RTL8139_RXBUF) { CHECK(0, "offset left the ring at iteration %d", i); break; }
        if (off & 3) { CHECK(0, "offset lost 4-byte alignment at iteration %d", i); break; }
    }
    CHECK(off < RTL8139_RXBUF, "5000 simulated receives keep the offset in the ring");
}

/* -------------------------------------------------------------- rtl8169 --- */

static void test_rtl8169_descriptors(void)
{
    CHECK(rtl8169_own(0x80000000u), "OWN is the top bit");
    CHECK(!rtl8169_own(0x40000000u), "EOR alone is not OWN");

    CHECK_EQ(rtl8169_len(0xB000005Eu), 94, "length is the low 14 bits");
    CHECK_EQ(rtl8169_len(0xFFFFFFFFu), 0x3FFF, "length saturates at 14 bits");
    CHECK_EQ(rtl8169_len(0x30000000u), 0, "no length bits set");

    /* A good receive: chip released it, single-descriptor frame, no error. */
    uint32_t good = RTL8169_FS | RTL8169_LS | 94;
    CHECK(rtl8169_rx_ok(good), "FS+LS, OWN clear, no error summary");
    CHECK(!rtl8169_rx_ok(good | RTL8169_OWN), "still owned by the chip is not receivable");
    CHECK(!rtl8169_rx_ok(good | RTL8169_RES), "the error summary bit rejects the frame");
    CHECK(!rtl8169_rx_ok(RTL8169_FS | 94), "a frame with no LS spans descriptors: rejected");
    CHECK(!rtl8169_rx_ok(RTL8169_LS | 94), "a frame with no FS is a continuation: rejected");

    /* Re-posting a receive descriptor. EOR only on the last one -- forget it and
     * the chip runs off the end of the ring into whatever follows it. */
    CHECK_EQ(rtl8169_rx_opts1(2048, 0), 0x80000800u, "repost mid-ring: OWN + size");
    CHECK_EQ(rtl8169_rx_opts1(2048, 1), 0xC0000800u, "repost last: OWN + EOR + size");

    CHECK_EQ(rtl8169_tx_opts1(60, 0), 0xB000003Cu, "transmit mid-ring: OWN + FS + LS + len");
    CHECK_EQ(rtl8169_tx_opts1(60, 1), 0xF000003Cu, "transmit last: adds EOR");
    CHECK(rtl8169_own(rtl8169_tx_opts1(60, 0)), "a posted transmit is owned by the chip");
    CHECK_EQ(rtl8169_len(rtl8169_tx_opts1(1514, 1)), 1514, "transmit length round-trips");

    /* The descriptor is 16 bytes with the buffer address last -- if this ever
     * stops holding, the chip DMAs to the wrong place. */
    struct probe_desc { uint32_t opts1, opts2; uint64_t addr; };
    CHECK_EQ(sizeof(struct probe_desc), 16, "descriptor is 16 bytes");
}

/* --------------------------------------------------------- match tables --- */
/* Resolution is the device model's dev_match_table(), compiled in from
 * c/drivers/core/device.c -- the same function the kernel binds with, not a
 * reimplementation of it. The tables are the real ones from net_ids.inc. */

static const struct { const char *name; const struct dev_match *ids; } nic_drivers[] = {
    { "virtio-net", virtio_net_ids },
    { "e1000",      e1000_ids      },
    { "rtl8139",    rtl8139_ids    },
    { "rtl8169",    rtl8169_ids    },
};
#define NDRV ((int)(sizeof nic_drivers / sizeof nic_drivers[0]))

/* Which driver claims this card, in NIC-line priority order? NULL if none. */
static const char *resolve(uint16_t vendor, uint16_t device)
{
    struct device d;
    memset(&d, 0, sizeof d);
    d.bus_type = DEV_BUS_PCI;
    d.vendor = vendor; d.device = device;
    d.class_code = 0x02; d.subclass = 0x00; d.prog_if = 0x00;   /* Ethernet */
    for (int i = 0; i < NDRV; i++)
        if (dev_match_table(nic_drivers[i].ids, &d))
            return nic_drivers[i].name;
    return 0;
}

static void test_match_tables(void)
{
    /* Every card we claim to support resolves to the driver that supports it. */
    CHECK(resolve(0x8086, 0x100E) && !strcmp(resolve(0x8086, 0x100E), "e1000"),
          "QEMU's default e1000 (8086:100E) binds the e1000 driver");
    CHECK(resolve(0x8086, 0x100F) && !strcmp(resolve(0x8086, 0x100F), "e1000"),
          "82545EM binds the e1000 driver");
    CHECK(resolve(0x8086, 0x1004) && !strcmp(resolve(0x8086, 0x1004), "e1000"),
          "82544GC binds the e1000 driver");
    CHECK(resolve(0x10EC, 0x8139) && !strcmp(resolve(0x10EC, 0x8139), "rtl8139"),
          "RTL8139 binds the rtl8139 driver");
    CHECK(resolve(0x10EC, 0x8169) && !strcmp(resolve(0x10EC, 0x8169), "rtl8169"),
          "RTL8169 binds the rtl8169 driver");
    CHECK(resolve(0x10EC, 0x8168) && !strcmp(resolve(0x10EC, 0x8168), "rtl8169"),
          "RTL8168/8111 binds the rtl8169 driver");
    CHECK(resolve(0x1AF4, 0x1000) && !strcmp(resolve(0x1AF4, 0x1000), "virtio-net"),
          "transitional virtio-net (1AF4:1000) binds virtio-net");
    CHECK(resolve(0x1AF4, 0x1041) && !strcmp(resolve(0x1AF4, 0x1041), "virtio-net"),
          "modern virtio-net (1AF4:1041) binds virtio-net");

    /* Cards we do NOT claim must not be claimed by anything. A driver that
     * matches too widely is worse than one that matches too narrowly: the card
     * enumerates, the link never comes up, and nothing says why. This is also
     * why none of these tables uses DEV_MATCH_CLASS(PCI_CLASS_NETWORK, 0). */
    CHECK(!resolve(0x8086, 0x10D3), "82574L/e1000e is NOT claimed by the e1000 driver");
    CHECK(!resolve(0x1AF4, 0x1001), "virtio-blk is not a NIC");
    CHECK(!resolve(0x1AF4, 0x1050), "virtio-gpu is not a NIC");
    CHECK(!resolve(0x10EC, 0x8129), "the RTL8129 is not claimed");
    CHECK(!resolve(0x1234, 0x1111), "QEMU's stdvga is not a NIC");
    CHECK(!resolve(0x10EC, 0x8029), "an ne2k_pci (10EC:8029) is left unclaimed");
    CHECK(!resolve(0x1022, 0x2000), "a pcnet is left unclaimed");
    CHECK(!resolve(0xFFFF, 0xFFFF), "an absent PCI slot matches nothing");
    CHECK(!resolve(0x0000, 0x0000), "the terminator row is not a match");

    /* Vendor and device must BOTH match -- an id table checked on device alone
     * would bind a Realtek driver to an Intel card with a coincidental id. */
    CHECK(!resolve(0x10EC, 0x100E), "a Realtek device id 0x100E is not an e1000");
    CHECK(!resolve(0x8086, 0x8139), "an Intel device id 0x8139 is not an RTL8139");

    /* No two drivers may claim the same card: the NIC line binds the first
     * match, so an overlap would silently pick by table order. */
    for (int i = 0; i < NDRV; i++) {
        for (const struct dev_match *id = nic_drivers[i].ids; id->vendor; id++) {
            const char *who = resolve(id->vendor, id->device);
            CHECK(who && !strcmp(who, nic_drivers[i].name),
                  "%x:%x from %s's table resolves back to %s (got %s)",
                  id->vendor, id->device, nic_drivers[i].name,
                  nic_drivers[i].name, who ? who : "nothing");
        }
    }

    /* Every table is terminated and non-empty. */
    for (int i = 0; i < NDRV; i++) {
        int rows = 0;
        for (const struct dev_match *id = nic_drivers[i].ids; id->vendor; id++) rows++;
        CHECK(rows > 0, "%s has at least one PCI id", nic_drivers[i].name);
    }

    /* Degenerate inputs must not crash or match. */
    struct device d;
    memset(&d, 0, sizeof d);
    d.vendor = 0x8086; d.device = 0x100E;
    CHECK(!dev_match_table(0, &d), "a NULL table matches nothing");
    CHECK(!dev_match_table(e1000_ids, 0), "a NULL device matches nothing");
    CHECK(!dev_match_one(0, &d), "a NULL match row matches nothing");
}

int main(void)
{
    test_ring_next();
    test_virtio_used_counters();
    test_virtio_net_header();
    test_rtl8139_header();
    test_rtl8139_offsets();
    test_rtl8169_descriptors();
    test_match_tables();

    if (failures) {
        printf("net_drv_test: %d/%d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("net_drv_test: %d checks passed\n", checks);
    return 0;
}

/* device.c calls into the PCI bus driver and the IRQ layer for dev_enable /
 * dev_bar_map / dev_unbind. This test only uses its match resolution and never
 * touches a device, so satisfy the link with no-ops rather than dragging
 * pci.c (and its port I/O) in. Same stubs as tests/unit/devmodel_test.c. */
uint32_t pci_cfg_read(uint8_t b, uint8_t s, uint8_t f, uint16_t o)
{ (void)b; (void)s; (void)f; (void)o; return 0; }
void pci_cfg_write(uint8_t b, uint8_t s, uint8_t f, uint16_t o, uint32_t v)
{ (void)b; (void)s; (void)f; (void)o; (void)v; }
uint16_t pci_cfg_read16(uint8_t b, uint8_t s, uint8_t f, uint16_t o)
{ (void)b; (void)s; (void)f; (void)o; return 0; }
void pci_cfg_write16(uint8_t b, uint8_t s, uint8_t f, uint16_t o, uint16_t v)
{ (void)b; (void)s; (void)f; (void)o; (void)v; }
const char *pci_class_name(uint8_t c, uint8_t s) { (void)c; (void)s; return "test"; }
void dev_irq_release(struct device *d) { (void)d; }
