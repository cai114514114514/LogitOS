/* Reference driver for QEMU's "edu" educational PCI device (1234:11e8).
 *
 * Two jobs, both deliberate:
 *
 *  1. IT IS THE WORKED EXAMPLE. This file is what a driver on the LogitOS
 *     device model looks like end to end: a match table, a probe(), a BAR map,
 *     dev_irq_request(), an ISR. It touches no PCI configuration space, knows no
 *     vendor ID beyond its own match table, and is registered by nothing but
 *     DRIVER_DECLARE. Adding it required editing no other file.
 *
 *  2. IT IS THE PROOF THAT INTERRUPTS ARRIVE. `edu` is the only device QEMU
 *     offers whose whole purpose is to be poked: writing its "raise" register
 *     makes it signal an interrupt immediately, with a payload we choose. So the
 *     boot test can assert that an MSI *actually reached a handler* rather than
 *     that some registers were written. It then releases the vector, forces the
 *     legacy INTx path with dev_irq_prefer(), and does it again -- because a
 *     fallback that has never run is not a fallback.
 *
 * If no `edu` device is present the driver simply never binds and the kernel is
 * unchanged. That is the property the whole model exists for.
 *
 * Register map (qemu/hw/misc/edu.c):
 *   0x00 identification (RO)      0x04 liveness check (write V, read ~V)
 *   0x20 status                   0x24 interrupt status (RO)
 *   0x60 raise interrupt (WO)     0x64 acknowledge interrupt (WO)  */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "pci.h"
#include "kprintf.h"
#include "pit.h"

#define EDU_VENDOR   0x1234
#define EDU_DEVICE   0x11E8

#define EDU_ID       0x00
#define EDU_LIVENESS 0x04
#define EDU_IRQ_STAT 0x24
#define EDU_IRQ_RAISE 0x60
#define EDU_IRQ_ACK   0x64

#define EDU_PAYLOAD  0x2A          /* arbitrary; echoed back through IRQ status */

struct edu {
    volatile uint32_t *mmio;
    volatile uint32_t  fired;      /* set by the ISR */
    volatile uint32_t  payload;
};

static struct edu g_edu;

static void edu_isr(void *arg)
{
    struct edu *e = (struct edu *)arg;
    uint32_t st = e->mmio[EDU_IRQ_STAT / 4];
    if (st) {
        e->mmio[EDU_IRQ_ACK / 4] = st;  /* de-assert the line */
        e->payload = st;                /* set BEFORE ->fired: the waiter reads
                                         * payload only after seeing fired */
    }
    /* The INTx path really does deliver twice on QEMU -- the RTE is programmed
     * edge/active-low (see pci_msi.c on the TCG remote-IRR bug), so both the
     * assert and the ack's de-assert produce an edge. The second entry finds
     * status 0, which is why payload is latched only on a nonzero read. */
    e->fired++;
}

/* Raise one interrupt and wait for edu_isr to run. Interrupts must be ON for
 * the wait: probe() is called from the boot thread, and on a machine where the
 * caller had IF=0 the handler could never run and we would report a false
 * negative. Bounded -- a missing interrupt must not hang the boot. */
static int edu_fire_and_wait(struct edu *e)
{
    uint64_t fl;
    __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
    e->fired = 0; e->payload = 0;
    __asm__ volatile ("sti");
    e->mmio[EDU_IRQ_RAISE / 4] = EDU_PAYLOAD;
    int ok = 0;
    for (long i = 0; i < 40000000 && !ok; i++) {
        __asm__ volatile ("" ::: "memory");
        ok = (e->fired != 0);
    }
    if (!(fl & 0x200)) __asm__ volatile ("cli");
    return ok;
}

static void edu_selftest(struct device *dev, struct edu *e, const char *label)
{
    const char *mode = dev->irq_mode == DEV_IRQ_MSIX ? "msix"
                     : dev->irq_mode == DEV_IRQ_MSI  ? "msi"
                     : dev->irq_mode == DEV_IRQ_INTX ? "intx" : "none";
    if (dev->irq_mode == DEV_IRQ_NONE) {
        kprintf("LOGIT_IRQ_FAIL %s no-vector\n", label);
        return;
    }
    int ok = edu_fire_and_wait(e);
    /* One machine-checkable line per path. The count comes from the vector
     * bookkeeping in c/drivers/core/irq.c, not from the driver's own flag, so
     * it asserts that the model dispatched -- not just that something ran. */
    kprintf("%s %s mode=%s vec=%d count=%d payload=%x\n",
            ok ? "LOGIT_IRQ_OK" : "LOGIT_IRQ_FAIL", label, mode,
            (int)dev->irq_vec, (int)dev_irq_count(dev), (unsigned)e->payload);
}

static int edu_probe(struct device *dev)
{
    dev_enable(dev, 1);
    uint64_t bar = dev_bar_map(dev, 0);
    if (!bar) { kprintf("[edu] %s: no BAR0\n", dev->name); return -1; }

    struct edu *e = &g_edu;
    e->mmio = (volatile uint32_t *)(uintptr_t)bar;

    /* Liveness check: the device returns the bitwise complement of whatever is
     * written here. If this fails we are pointed at the wrong memory. */
    e->mmio[EDU_LIVENESS / 4] = 0x12345678u;
    uint32_t back = e->mmio[EDU_LIVENESS / 4];
    if (back != ~0x12345678u) {
        kprintf("[edu] %s: liveness %x (want %x) -- not bound\n",
                dev->name, back, ~0x12345678u);
        return -1;
    }
    kprintf("[edu] %s: id=%x live, msi=%s msix=%s intx-pin=%d\n",
            dev->name, e->mmio[EDU_ID / 4],
            dev->cap_msi ? "yes" : "no", dev->cap_msix ? "yes" : "no",
            (int)dev->irq_pin);

    dev_set_drvdata(dev, e);

    /* Path 1: whatever the model prefers (MSI-X, else MSI). */
    if (dev_irq_request(dev, edu_isr, e, "edu") >= 0)
        edu_selftest(dev, e, "msi");
    else
        kprintf("LOGIT_IRQ_FAIL msi no-vector\n");

    /* Path 2: the same device, forced down to the legacy INTx line. */
    dev_irq_release(dev);
    int prev = dev_irq_prefer(DEV_IRQ_INTX);
    if (dev_irq_request(dev, edu_isr, e, "edu-intx") >= 0)
        edu_selftest(dev, e, "legacy");
    else
        kprintf("LOGIT_IRQ_FAIL legacy no-vector\n");
    dev_irq_prefer(prev);

    return 0;
}

static const struct dev_match edu_ids[] = {
    DEV_MATCH_VD(EDU_VENDOR, EDU_DEVICE),
    DEV_MATCH_END
};

static struct driver edu_driver = {
    .name     = "edu",
    .bus_type = DEV_BUS_PCI,
    .match    = edu_ids,
    .probe    = edu_probe,
    .remove   = NULL,
    .next     = NULL,
};
DRIVER_DECLARE(edu_driver);
