/* Host unit tests for the device model itself: match-table resolution and the
 * probe/bind pass. No hardware and no PCI config space -- devices are pushed
 * straight into the registry with dev_add(), which is what a bus driver does.
 *
 * The interesting cases are the ones that decide whether `drivers/` can grow:
 * class-code matching (so one driver covers every AHCI controller ever made),
 * wildcard precedence (so a vendor quirk driver can sit in front of a generic
 * one), and probe() declining (so declining is not the same as binding). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "driver.h"
#include "pci.h"

static int checks, failures;
#define CHECK(cond, msg, ...) do {                                            \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("FAIL: " msg "\n", ##__VA_ARGS__); }    \
} while (0)

static struct device *mk(uint16_t ven, uint16_t dev, uint8_t cls, uint8_t sub, uint8_t pif)
{
    struct device d;
    memset(&d, 0, sizeof d);
    d.bus_type = DEV_BUS_PCI;
    d.vendor = ven; d.device = dev;
    d.class_code = cls; d.subclass = sub; d.prog_if = pif;
    d.irq_vec = -1;
    snprintf(d.name, sizeof d.name, "0000:00:%02x.0", dev_count());
    return dev_add(&d);
}

/* -------------------------------------------------- match-table resolution */
static void test_match(void)
{
    struct device ahci;  memset(&ahci, 0, sizeof ahci);
    ahci.bus_type = DEV_BUS_PCI;
    ahci.vendor = 0x8086; ahci.device = 0x2922;
    ahci.class_code = 0x01; ahci.subclass = 0x06; ahci.prog_if = 0x01;

    const struct dev_match m_vd     = DEV_MATCH_VD(0x8086, 0x2922);
    const struct dev_match m_vd_bad = DEV_MATCH_VD(0x8086, 0x1111);
    const struct dev_match m_cls    = DEV_MATCH_CLASS(0x01, 0x06);
    const struct dev_match m_cls_b  = DEV_MATCH_CLASS(0x01, 0x08);
    const struct dev_match m_pif    = DEV_MATCH_PROGIF(0x01, 0x06, 0x01);
    const struct dev_match m_pif_b  = DEV_MATCH_PROGIF(0x01, 0x06, 0x02);
    const struct dev_match m_vc     = DEV_MATCH_VCLASS(0x8086, 0x01, 0x06);
    const struct dev_match m_vc_b   = DEV_MATCH_VCLASS(0x10EC, 0x01, 0x06);
    const struct dev_match m_end    = DEV_MATCH_END;

    CHECK(dev_match_one(&m_vd, &ahci) == 1,     "vendor:device should match");
    CHECK(dev_match_one(&m_vd_bad, &ahci) == 0, "wrong device id must not match");
    CHECK(dev_match_one(&m_cls, &ahci) == 1,    "class/subclass should match");
    CHECK(dev_match_one(&m_cls_b, &ahci) == 0,  "wrong subclass must not match");
    CHECK(dev_match_one(&m_pif, &ahci) == 1,    "prog-if should match");
    CHECK(dev_match_one(&m_pif_b, &ahci) == 0,  "wrong prog-if must not match");
    CHECK(dev_match_one(&m_vc, &ahci) == 1,     "vendor+class should match");
    CHECK(dev_match_one(&m_vc_b, &ahci) == 0,   "wrong vendor with right class must not match");
    CHECK(dev_match_one(&m_end, &ahci) == 0,    "the table terminator must never match");
    CHECK(dev_match_one(NULL, &ahci) == 0,      "NULL entry must not match");

    /* A table returns the FIRST matching entry, and carries its ->data through. */
    static const struct dev_match tbl[] = {
        DEV_MATCH_VD_DATA(0x8086, 0x1111, 0xAA),
        DEV_MATCH_VD_DATA(0x8086, 0x2922, 0xBB),
        DEV_MATCH_CLASS(0x01, 0x06),
        DEV_MATCH_END
    };
    const struct dev_match *hit = dev_match_table(tbl, &ahci);
    CHECK(hit == &tbl[1], "table should stop at the first match");
    CHECK(hit && hit->data == 0xBB, "matched entry must carry its ->data tag");

    static const struct dev_match none[] = { DEV_MATCH_VD(0x1234, 0x5678), DEV_MATCH_END };
    CHECK(dev_match_table(none, &ahci) == NULL, "no-match table must return NULL");

    /* A table that is nothing but a terminator must terminate. */
    static const struct dev_match empty[] = { DEV_MATCH_END };
    CHECK(dev_match_table(empty, &ahci) == NULL, "empty table must return NULL");
}

/* -------------------------------------------------------- probe and bind -- */
static int probe_calls_generic, probe_calls_quirk, probe_calls_decline;

static int generic_probe(struct device *d) { (void)d; probe_calls_generic++; return 0; }
static int quirk_probe(struct device *d)   { (void)d; probe_calls_quirk++;   return 0; }
static int decline_probe(struct device *d) { (void)d; probe_calls_decline++; return -1; }

static const struct dev_match generic_ids[] = { DEV_MATCH_CLASS(0x01, 0x06), DEV_MATCH_END };
static const struct dev_match quirk_ids[]   = { DEV_MATCH_VD(0x8086, 0x2922), DEV_MATCH_END };
static const struct dev_match net_ids[]     = { DEV_MATCH_CLASS(0x02, 0x00), DEV_MATCH_END };

static struct driver drv_decline = { .name = "decline", .bus_type = DEV_BUS_PCI,
                                     .match = quirk_ids,   .probe = decline_probe };
static struct driver drv_quirk   = { .name = "quirk",   .bus_type = DEV_BUS_PCI,
                                     .match = quirk_ids,   .probe = quirk_probe };
static struct driver drv_generic = { .name = "generic", .bus_type = DEV_BUS_PCI,
                                     .match = generic_ids, .probe = generic_probe };
static struct driver drv_net     = { .name = "net",     .bus_type = DEV_BUS_PCI,
                                     .match = net_ids,    .probe = generic_probe };
static struct driver drv_platform = { .name = "plat",   .bus_type = DEV_BUS_PLATFORM,
                                      .match = generic_ids, .probe = generic_probe };

static void test_probe(void)
{
    struct device *ahci = mk(0x8086, 0x2922, 0x01, 0x06, 0x01);   /* quirk + generic */
    struct device *ahci2 = mk(0x1B36, 0x0010, 0x01, 0x06, 0x01);  /* generic only */
    struct device *nic  = mk(0x10EC, 0x8139, 0x02, 0x00, 0x00);   /* net */
    struct device *odd  = mk(0xDEAD, 0xBEEF, 0x0D, 0x00, 0x00);   /* nothing */
    CHECK(ahci && ahci2 && nic && odd, "dev_add returned NULL");

    /* Registration order is precedence order. drv_decline matches the same
     * device as drv_quirk but declines, so drv_quirk must still get it. */
    driver_register(&drv_decline);
    driver_register(&drv_quirk);
    driver_register(&drv_generic);
    driver_register(&drv_net);
    driver_register(&drv_platform);
    driver_register(&drv_quirk);        /* double registration must be a no-op */

    int bound = dev_probe_all();
    CHECK(bound == 3, "expected 3 devices bound, got %d", bound);
    CHECK(probe_calls_decline == 1, "declining driver probed %d times (want 1)", probe_calls_decline);
    CHECK(ahci->drv == &drv_quirk, "specific driver must win over the generic one (got %s)",
          ahci->drv ? ahci->drv->name : "none");
    CHECK(ahci2->drv == &drv_generic, "class-only device should bind the generic driver (got %s)",
          ahci2->drv ? ahci2->drv->name : "none");
    CHECK(nic->drv == &drv_net, "NIC bound to %s", nic->drv ? nic->drv->name : "none");
    CHECK(odd->drv == NULL, "unmatched device must stay unclaimed");
    CHECK(probe_calls_generic == 2, "generic probe ran %d times (want 2)", probe_calls_generic);

    /* A PLATFORM-bus driver must not be offered a PCI device even though its
     * match table would accept one. */
    CHECK(ahci2->drv != &drv_platform, "bus_type must gate probing");

    /* Re-running must not re-probe anything already bound. */
    int again = dev_probe_all();
    CHECK(again == 0, "second probe pass bound %d devices (want 0)", again);
    CHECK(probe_calls_quirk == 1, "quirk probe ran %d times across two passes", probe_calls_quirk);

    /* Unbind frees the device for a later pass. */
    dev_unbind(ahci);
    CHECK(ahci->drv == NULL, "dev_unbind must clear ->drv");
    again = dev_probe_all();
    CHECK(again == 1 && ahci->drv == &drv_quirk, "unbound device should rebind (again=%d)", again);
}

static void test_lookup(void)
{
    struct device *d = dev_find_class(0x01, 0x06, NULL);
    CHECK(d != NULL, "dev_find_class found nothing");
    struct device *e = dev_find_class(0x01, 0x06, d);
    CHECK(e != NULL && e != d, "dev_find_class must iterate to the second match");
    CHECK(dev_find_class(0x01, 0x06, e) == NULL, "iteration must end");

    CHECK(dev_find_class(0x01, DEV_ANYC, NULL) == d, "subclass wildcard should match");
    CHECK(dev_find_id(0x10EC, 0x8139, NULL) != NULL, "dev_find_id by vendor:device");
    CHECK(dev_find_id(0x10EC, 0x0000, NULL) == NULL, "dev_find_id must not match loosely");

    /* dev_at / dev_count agree with what was added. */
    CHECK(dev_count() == 4, "dev_count %d (want 4)", dev_count());
    CHECK(dev_at(0) != NULL && dev_at(4) == NULL && dev_at(-1) == NULL, "dev_at bounds");
}

int main(void)
{
    test_match();
    test_probe();
    test_lookup();
    dev_dump();
    printf("\nDevice-model tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

/* device.c calls into the PCI bus driver for dev_enable/dev_bar_map; the model
 * tests never touch a real device, so satisfy the link with no-ops rather than
 * dragging pci.c (and its port I/O) in. */
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
