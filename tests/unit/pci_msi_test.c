/* Host unit tests for dev_irq_request()'s three wiring paths.
 *
 * On-device coverage (tests/boot/run-devmodel-test.sh) proves MSI and legacy
 * INTx actually deliver, but QEMU offers no device with an MSI-X capability
 * that this tree has a driver for -- so the MSI-X path would otherwise ship
 * entirely unexecuted. What can be checked without hardware is the part that is
 * easy to get wrong and silent when wrong: WHICH BYTES GET WRITTEN WHERE.
 *
 * So: a synthetic configuration space, an MSI-X table that is just memory, and
 * assertions on the exact message address/data, the table entry layout, the
 * enable/mask bit order, and the INTx-disable bit. Plus the 64-bit and 32-bit
 * MSI capability layouts, which put the data word at different offsets -- an
 * off-by-four there writes a vector number into an address register.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pci.h"
#include "driver.h"
#include "irq.h"

static int checks, failures;
#define CHECK(cond, msg, ...) do {                                            \
    checks++;                                                                 \
    if (!(cond)) { failures++; printf("FAIL: " msg "\n", ##__VA_ARGS__); }    \
} while (0)

/* ------------------------------------------------- synthetic config space -- */
#define FBUS 1
static uint8_t *g_space;
static uint32_t *cfg(int bus, int slot, int func, int off)
{
    size_t o = ((size_t)bus << 20) | ((size_t)slot << 15) | ((size_t)func << 12) | (off & 0xFFC);
    return (uint32_t *)(g_space + o);
}
static uint32_t g_cf8;
void outl(uint16_t port, uint32_t val)
{
    if (port == 0xCF8) { g_cf8 = val; return; }
    if (port == 0xCFC && (g_cf8 >> 31)) {
        int bus = (g_cf8 >> 16) & 0xFF, slot = (g_cf8 >> 11) & 0x1F;
        int func = (g_cf8 >> 8) & 7, off = g_cf8 & 0xFC;
        if (bus < FBUS) *cfg(bus, slot, func, off) = val;
    }
}
uint32_t inl(uint16_t port)
{
    if (port != 0xCFC || !(g_cf8 >> 31)) return 0xFFFFFFFFu;
    int bus = (g_cf8 >> 16) & 0xFF, slot = (g_cf8 >> 11) & 0x1F;
    int func = (g_cf8 >> 8) & 7, off = g_cf8 & 0xFC;
    if (bus >= FBUS) return 0xFFFFFFFFu;
    return *cfg(bus, slot, func, off);
}
void outb(uint16_t p, uint8_t v)  { (void)p; (void)v; }
uint8_t inb(uint16_t p)           { (void)p; return 0xFF; }
void outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
uint16_t inw(uint16_t p)          { (void)p; return 0xFFFF; }

/* ------------------------------------------- interrupt-controller doubles -- */
#define FAKE_APIC_ID 3

static int g_lapic_ready = 1;
int      lapic_ready(void) { return g_lapic_ready; }
uint32_t lapic_id(void)    { return FAKE_APIC_ID; }
void     lapic_eoi(void)   { }
int      smp_irq_via_apic(void) { return 1; }
void     pic_eoi(int irq)  { (void)irq; }

static int g_ioapic_present = 1;
static struct { uint32_t gsi; uint8_t vec, apic; int level, active_low; int calls; } g_route;
int  ioapic_present(void) { return g_ioapic_present; }
void ioapic_route(uint32_t gsi, uint8_t vec, uint8_t apic_id, int level, int active_low)
{
    g_route.gsi = gsi; g_route.vec = vec; g_route.apic = apic_id;
    g_route.level = level; g_route.active_low = active_low; g_route.calls++;
}
void ioapic_route_isa(int isa, uint8_t vec, uint8_t apic) { (void)isa; (void)vec; (void)apic; }
uint32_t acpi_gsi_for_irq(int irq) { return (uint32_t)irq; }

/* Vector pool double: hands out 0x60 upward, so the test knows the number. */
static int g_next_vec = IRQ_VEC_BASE;
static int g_freed = -1;
int irq_alloc_vector(irq_handler_t fn, void *arg, const char *name)
{
    (void)fn; (void)arg; (void)name;
    return (g_next_vec < IRQ_VEC_BASE + IRQ_NVEC) ? g_next_vec++ : -1;
}
void     irq_free_vector(int vec)     { g_freed = vec; }
uint64_t irq_vector_count(int vec)    { (void)vec; return 0; }

/* ---------------------------------------------------------------- fixture -- */
static uint8_t *g_bar;              /* stands in for an MSI-X table BAR */
#define BAR_SIZE 0x1000

static struct device *mkdev(int slot, uint8_t cap_msi, uint8_t cap_msix,
                            uint8_t irq_pin, uint8_t irq_line)
{
    for (int o = 0; o < 0x100; o += 4) *cfg(0, slot, 0, o) = 0;
    *cfg(0, slot, 0, 0x00) = 0x11111234u;

    struct device d;
    memset(&d, 0, sizeof d);
    d.bus_type = DEV_BUS_PCI;
    d.bus = 0; d.slot = (uint8_t)slot; d.func = 0;
    d.vendor = 0x1234; d.device = 0x1111;
    d.cap_msi = cap_msi; d.cap_msix = cap_msix;
    d.irq_pin = irq_pin; d.irq_line = irq_line;
    d.irq_mode = DEV_IRQ_NONE; d.irq_vec = -1;
    d.res[1].start = (uint64_t)(uintptr_t)g_bar;
    d.res[1].size  = BAR_SIZE;
    d.res[1].flags = DEV_RES_MEM;
    snprintf(d.name, sizeof d.name, "0000:00:%02x.0", slot);
    return dev_add(&d);
}

static void handler(void *arg) { (void)arg; }

/* ================================================================ tests == */

static void test_msix(void)
{
    struct device *d = mkdev(1, 0, 0x50, 1, 11);
    /* MSI-X capability: id 0x11, message control says 4 vectors (tsize-1 = 3),
     * table at BAR 1 offset 0x200. */
    *cfg(0, 1, 0, 0x50) = 0x00030011u;
    *cfg(0, 1, 0, 0x54) = 0x00000200u | 1;          /* offset 0x200, BIR 1 */
    memset(g_bar, 0xAA, BAR_SIZE);

    int vec = dev_irq_request(d, handler, NULL, "t");
    CHECK(vec >= IRQ_VEC_BASE, "dev_irq_request returned %d", vec);
    CHECK(d->irq_mode == DEV_IRQ_MSIX, "mode %d (want MSIX)", d->irq_mode);
    CHECK(d->irq_vec == vec, "dev->irq_vec %d != %d", d->irq_vec, vec);

    volatile uint32_t *e = (volatile uint32_t *)(g_bar + 0x200);
    CHECK(e[0] == (0xFEE00000u | (FAKE_APIC_ID << 12)),
          "message address %08x (want %08x)", e[0], 0xFEE00000u | (FAKE_APIC_ID << 12));
    CHECK(e[1] == 0, "message address high must be 0 (got %08x)", e[1]);
    CHECK(e[2] == (uint32_t)vec, "message data %08x (want the vector %d)", e[2], vec);
    CHECK(e[3] == 0, "vector control must be unmasked (got %08x)", e[3]);

    uint16_t mc = (uint16_t)(*cfg(0, 1, 0, 0x50) >> 16);
    CHECK((mc & 0x8000) != 0, "MSI-X enable bit not set (mc=%04x)", mc);
    CHECK((mc & 0x4000) == 0, "function mask must be cleared at the end (mc=%04x)", mc);
    CHECK((mc & 0x07FF) == 3, "table size field must be preserved (mc=%04x)", mc);

    uint16_t cmd = (uint16_t)*cfg(0, 1, 0, 0x04);
    CHECK((cmd & PCI_CMD_INTX_DIS) != 0, "INTx must be disabled when MSI-X is on (cmd=%04x)", cmd);
    CHECK(g_route.calls == 0, "MSI-X must not touch the I/O APIC");

    /* Asking again must not re-wire; it returns the vector already held. */
    CHECK(dev_irq_request(d, handler, NULL, "t") == vec, "second request must be idempotent");

    dev_irq_release(d);
    mc = (uint16_t)(*cfg(0, 1, 0, 0x50) >> 16);
    CHECK((mc & 0x8000) == 0, "release must clear the MSI-X enable bit (mc=%04x)", mc);
    CHECK(d->irq_mode == DEV_IRQ_NONE && d->irq_vec == -1, "release must reset the device fields");
    CHECK(g_freed == vec, "release must return the vector to the pool (freed %d)", g_freed);

    /* A table that does not fit inside its own BAR must be refused, not written
     * past the end of the mapping. */
    struct device *bad = mkdev(2, 0, 0x50, 0, 0);
    *cfg(0, 2, 0, 0x50) = 0x00000011u;
    *cfg(0, 2, 0, 0x54) = (BAR_SIZE - 8) | 1;       /* 16-byte entry would overrun */
    CHECK(dev_irq_request(bad, handler, NULL, "t") == -1,
          "an MSI-X table overrunning its BAR must be refused");
    CHECK(bad->irq_mode == DEV_IRQ_NONE, "refused request must leave mode NONE");
}

static void test_msi_64bit(void)
{
    struct device *d = mkdev(3, 0x60, 0, 1, 11);
    /* MSI capability, 64-bit address + per-vector masking, 4 messages capable. */
    *cfg(0, 3, 0, 0x60) = 0x01840005u;              /* mc = 0x0184 */

    int vec = dev_irq_request(d, handler, NULL, "t");
    CHECK(d->irq_mode == DEV_IRQ_MSI, "mode %d (want MSI)", d->irq_mode);
    CHECK(*cfg(0, 3, 0, 0x64) == (0xFEE00000u | (FAKE_APIC_ID << 12)),
          "MSI address low %08x", *cfg(0, 3, 0, 0x64));
    CHECK(*cfg(0, 3, 0, 0x68) == 0, "MSI address high must be 0");
    CHECK((*cfg(0, 3, 0, 0x6C) & 0xFFFF) == (uint32_t)vec,
          "64-bit MSI puts the data word at cap+0x0C (got %08x, vec %d)",
          *cfg(0, 3, 0, 0x6C), vec);
    CHECK(*cfg(0, 3, 0, 0x70) == 0, "per-vector mask bits must be cleared at cap+0x10");

    uint16_t mc = (uint16_t)(*cfg(0, 3, 0, 0x60) >> 16);
    CHECK((mc & 1) != 0, "MSI enable bit not set (mc=%04x)", mc);
    CHECK((mc & 0x0070) == 0, "MME must request exactly one vector (mc=%04x)", mc);
    CHECK((mc & 0x000E) == 0x0004, "MMC (capability) field must be preserved (mc=%04x)", mc);
    CHECK(((uint16_t)*cfg(0, 3, 0, 0x04) & PCI_CMD_INTX_DIS) != 0, "INTx must be disabled for MSI");

    dev_irq_release(d);
    mc = (uint16_t)(*cfg(0, 3, 0, 0x60) >> 16);
    CHECK((mc & 1) == 0, "release must clear the MSI enable bit (mc=%04x)", mc);
}

static void test_msi_32bit(void)
{
    struct device *d = mkdev(4, 0x40, 0, 1, 11);
    *cfg(0, 4, 0, 0x40) = 0x00000005u;              /* mc = 0: 32-bit, no PVM */

    int vec = dev_irq_request(d, handler, NULL, "t");
    CHECK(d->irq_mode == DEV_IRQ_MSI, "mode %d (want MSI)", d->irq_mode);
    CHECK(*cfg(0, 4, 0, 0x44) == (0xFEE00000u | (FAKE_APIC_ID << 12)), "MSI address low");
    CHECK((*cfg(0, 4, 0, 0x48) & 0xFFFF) == (uint32_t)vec,
          "32-bit MSI puts the data word at cap+0x08 (got %08x, vec %d)",
          *cfg(0, 4, 0, 0x48), vec);
    dev_irq_release(d);
}

static void test_intx_and_preference(void)
{
    /* No MSI, no MSI-X: the legacy line, routed through the I/O APIC. */
    struct device *d = mkdev(5, 0, 0, 1, 11);
    *cfg(0, 5, 0, 0x04) = PCI_CMD_INTX_DIS;         /* start with INTx suppressed */
    g_route.calls = 0;
    int vec = dev_irq_request(d, handler, NULL, "t");
    CHECK(d->irq_mode == DEV_IRQ_INTX, "mode %d (want INTX)", d->irq_mode);
    CHECK(g_route.calls == 1, "expected exactly one ioapic_route call, got %d", g_route.calls);
    CHECK(g_route.gsi == 11 && g_route.vec == vec && g_route.apic == FAKE_APIC_ID,
          "route gsi=%u vec=%d apic=%d", g_route.gsi, g_route.vec, g_route.apic);
    CHECK(g_route.level == 0 && g_route.active_low == 1,
          "PCI INTx must be routed edge/active-low here (level=%d low=%d)",
          g_route.level, g_route.active_low);
    CHECK(((uint16_t)*cfg(0, 5, 0, 0x04) & PCI_CMD_INTX_DIS) == 0,
          "INTx-disable must be cleared when using the line");

    /* No interrupt pin at all -> nothing can be wired. */
    struct device *nopin = mkdev(6, 0, 0, 0, 11);
    CHECK(dev_irq_request(nopin, handler, NULL, "t") == -1,
          "a device with no INTx pin and no MSI must fail");

    /* dev_irq_prefer forces the fallback even on a device that has MSI-X. */
    struct device *cap = mkdev(7, 0x60, 0x50, 1, 10);
    *cfg(0, 7, 0, 0x50) = 0x00000011u;
    *cfg(0, 7, 0, 0x54) = 1;                         /* table at BAR 1 offset 0 */
    *cfg(0, 7, 0, 0x60) = 0x00800005u;
    int prev = dev_irq_prefer(DEV_IRQ_INTX);
    g_route.calls = 0;
    dev_irq_request(cap, handler, NULL, "t");
    CHECK(cap->irq_mode == DEV_IRQ_INTX, "dev_irq_prefer(INTX) must force the line (mode %d)",
          cap->irq_mode);
    CHECK(g_route.gsi == 10, "forced INTx routed gsi %u (want 10)", g_route.gsi);
    dev_irq_prefer(DEV_IRQ_MSI);
    dev_irq_release(cap);
    dev_irq_request(cap, handler, NULL, "t");
    CHECK(cap->irq_mode == DEV_IRQ_MSI, "dev_irq_prefer(MSI) must skip MSI-X (mode %d)",
          cap->irq_mode);
    dev_irq_prefer(prev);
    dev_irq_release(cap);

    /* Without a LAPIC there is nothing for a message to be written to, so even
     * a device with MSI-X has to fall back to the line. */
    struct device *early = mkdev(8, 0x60, 0x50, 1, 9);
    *cfg(0, 8, 0, 0x50) = 0x00000011u;
    *cfg(0, 8, 0, 0x54) = 1;
    *cfg(0, 8, 0, 0x60) = 0x00800005u;
    g_lapic_ready = 0;
    dev_irq_request(early, handler, NULL, "t");
    CHECK(early->irq_mode == DEV_IRQ_INTX,
          "no LAPIC yet -> must not program MSI (mode %d)", early->irq_mode);
    g_lapic_ready = 1;
}

int main(void)
{
    g_space = calloc(1, (size_t)FBUS << 20);
    g_bar   = calloc(1, BAR_SIZE);
    if (!g_space || !g_bar) { printf("out of memory\n"); return 1; }

    test_msix();
    test_msi_64bit();
    test_msi_32bit();
    test_intx_and_preference();

    printf("\nMSI/MSI-X/INTx tests: %d checks, %d failed\n", checks, failures);
    free(g_space); free(g_bar);
    return failures ? 1 : 0;
}
