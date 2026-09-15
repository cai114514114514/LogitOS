/* SPDX-License-Identifier: MIT */
/* Production xHCI initialization with only PCI/MMIO/DMA leaves modelled.
 * This is an ownership-order test, not evidence of physical USB delivery. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t test_read(const volatile void *p, unsigned n);
void test_write(volatile void *p, unsigned n, uint64_t value);

#include "xhci_driver.inc"
#include "xhci_probe_driver.inc"

static unsigned char regs[65536] __attribute__((aligned(4096)));
static struct device device;
static uint16_t pci_command;
static unsigned pci_writes, map_calls, mmio_reads, mmio_writes;
static unsigned alloc_calls, free_calls, live, published, unsafe_free;
static unsigned bme_writes, early_bme, legacy_ctl_writes;
static int bme_authorized;
static int release_bios, legacy_complete, reset_complete;
static int dcbaa_programmed, crcr_programmed, erst_programmed;
static int erst_latched, erst_latched_with_bme, erdp_after_erst;
static int reject_event_ring, hce_on_run;
static int start_stuck, halt_stuck;
enum pci_fault {
    PCI_OK,
    PCI_DROP_PREPARE,
    PCI_RESTORE_UNACKNOWLEDGED,
    PCI_DROP_ISOLATE_AFTER_MASTER,
};
static enum pci_fault pci_fault;
static int saw_master;
static int bar_present;
static unsigned hc_register_calls;
static int checks, failures;

static void check(int yes, const char *why)
{
    checks++;
    if (!yes) {
        failures++;
        printf("FAIL: %s\n", why);
    }
}

static int in_regs(const volatile void *p)
{
    uintptr_t q = (uintptr_t)p, lo = (uintptr_t)regs;
    return q >= lo && q < lo + sizeof regs;
}

uint64_t test_read(const volatile void *p, unsigned n)
{
    uint64_t value = 0;
    if (in_regs(p)) {
        mmio_reads++;
        if ((pci_command & PCI_CMD_MASTER) && !bme_authorized) early_bme = 1;
    }
    memcpy(&value, (const void *)p, n);
    return value;
}

static void update_ring_milestones(size_t off, uint64_t value)
{
    if (!reset_complete || !value) return;
    if (off == 0x40 + XOP_DCBAAP) dcbaa_programmed = 1;
    if (off == 0x40 + XOP_CRCR) crcr_programmed = 1;
    if (off == 0x1000 + XRT_IR0 + XIR_ERSTBA)
        erst_programmed = 1;
}

void test_write(volatile void *p, unsigned n, uint64_t value)
{
    if (!in_regs(p)) {
        memcpy((void *)p, &value, n);
        return;
    }
    mmio_writes++;
    if ((pci_command & PCI_CMD_MASTER) && !bme_authorized) early_bme = 1;
    size_t off = (const volatile unsigned char *)p - regs;
    update_ring_milestones(off, value);
    if (reset_complete && off == 0x1000 + XRT_IR0 + XIR_ERSTBA + 4) {
        erst_latched = 1;
        erst_latched_with_bme = !!(pci_command & PCI_CMD_MASTER);
        if (reject_event_ring)
            *(uint32_t *)(regs + 0x40 + XOP_USBSTS) |= STS_HCE;
    }
    if (reset_complete && off == 0x1000 + XRT_IR0 + XIR_ERDP && value)
        erdp_after_erst = erst_latched;

    if (off == 0x100 && n == 4) {
        if ((value & (1u << 24)) && release_bios) {
            value &= ~(1u << 16);
            legacy_complete = 1;
        }
    } else if (off == 0x104 && n == 4) {
        legacy_ctl_writes++;
    } else if (off == 0x40 + XOP_USBCMD && n == 4) {
        uint32_t command = (uint32_t)value;
        if (command & CMD_HCRST) {
            command = 0;
            *(uint32_t *)(regs + 0x40 + XOP_USBSTS) = STS_HCH;
            *(uint64_t *)(regs + 0x40 + XOP_CRCR) = 0;
            reset_complete = 1;
        } else if (command & CMD_RS) {
            *(uint32_t *)(regs + 0x40 + XOP_USBSTS) =
                hce_on_run ? STS_HCE : (start_stuck ? STS_HCH : 0);
        } else if (!halt_stuck) {
            *(uint32_t *)(regs + 0x40 + XOP_USBSTS) = STS_HCH;
            *(uint32_t *)(regs + 0x40 + XOP_CRCR) &= ~CRCR_CRR;
        } else {
            *(uint32_t *)(regs + 0x40 + XOP_USBSTS) = 0;
        }
        value = command;
    }
    memcpy((void *)p, &value, n);
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    (void)bus; (void)slot; (void)func;
    return off == PCI_CFG_COMMAND ? pci_command : UINT16_MAX;
}

void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t func,
                     uint16_t off, uint16_t value)
{
    (void)bus; (void)slot; (void)func;
    if (off != PCI_CFG_COMMAND) return;
    pci_writes++;

    if (pci_fault == PCI_DROP_PREPARE && pci_writes == 1) return;
    if (pci_fault == PCI_RESTORE_UNACKNOWLEDGED) {
        if (pci_writes == 1) {
            pci_command = (uint16_t)(value & ~PCI_CMD_MEM);
            return;
        }
        if (pci_writes == 2) return;
    }
    if (pci_fault == PCI_DROP_ISOLATE_AFTER_MASTER && saw_master &&
        !(value & PCI_CMD_MASTER))
        return;

    pci_command = value;
    if (value & PCI_CMD_MASTER) {
        saw_master = 1;
        bme_writes++;
        uint32_t command = *(uint32_t *)(regs + 0x40 + XOP_USBCMD);
        uint32_t status = *(uint32_t *)(regs + 0x40 + XOP_USBSTS);
        if (!legacy_complete || !reset_complete || !dcbaa_programmed ||
            !crcr_programmed || alloc_calls < 3 || (command & CMD_RS) ||
            !(status & STS_HCH))
            early_bme = 1;
        else
            bme_authorized = 1;
    } else {
        bme_authorized = 0;
    }
}

uint64_t dev_bar_map(struct device *d, int bar)
{
    (void)d; (void)bar;
    map_calls++;
    return bar_present ? (uint64_t)(uintptr_t)regs : 0;
}

void dev_enable(struct device *d, int master) { (void)d; (void)master; }
void dev_disable(struct device *d) { (void)d; }
void kprintf(const char *fmt, ...) { (void)fmt; }
uint64_t timer_ms(void) { static uint64_t now; return ++now; }
void dma_wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
void dma_rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }

void dma_device_init(struct dma_device *d, const char *name, uint64_t mask)
{
    memset(d, 0, sizeof *d);
    d->name = name;
    d->mask = mask;
}

struct dma_buffer *dma_alloc_coherent(struct dma_device *d, size_t bytes,
                                      size_t align, size_t boundary)
{
    (void)align; (void)boundary;
    if (d->blocked) return NULL;
    alloc_calls++;
    struct dma_buffer *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->size = (bytes + 4095) & ~(size_t)4095;
    b->pages = b->size / 4096;
    b->cpu = aligned_alloc(4096, b->size);
    if (!b->cpu) { free(b); return NULL; }
    memset(b->cpu, 0, b->size);
    b->dma.value = UINT64_C(0x1230000000) + alloc_calls * 0x10000u;
    b->phys = b->dma.value;
    b->dev = d;
    b->state = DMA_READY;
    b->next = d->buffers;
    d->buffers = b;
    live++;
    return b;
}

int dma_free_coherent(struct dma_buffer *b)
{
    free_calls++;
    if (!b || b->state == DMA_DEVICE_OWNED || b->state == DMA_QUARANTINED) {
        unsafe_free++;
        return -1;
    }
    struct dma_buffer **link = &b->dev->buffers;
    while (*link && *link != b) link = &(*link)->next;
    if (!*link) return -1;
    *link = b->next;
    free(b->cpu);
    free(b);
    live--;
    return 0;
}

uint64_t dma_buffer_submit(struct dma_buffer *b)
{
    if (!b || b->dev->blocked) return 0;
    b->state = DMA_DEVICE_OWNED;
    published++;
    return b->token = published;
}

int dma_buffer_complete(struct dma_buffer *b, uint64_t token)
{
    if (!b || b->state != DMA_DEVICE_OWNED || b->token != token) return -1;
    b->state = DMA_COMPLETED;
    return 0;
}

void dma_device_quiesced(struct dma_device *d)
{
    for (struct dma_buffer *b = d->buffers; b; b = b->next)
        b->state = DMA_QUIESCED;
}

void dma_device_quarantine(struct dma_device *d)
{
    d->blocked = 1;
    for (struct dma_buffer *b = d->buffers; b; b = b->next)
        b->state = DMA_QUARANTINED;
}

int dma_device_resume(struct dma_device *d) { return d && !d->blocked ? 0 : -1; }
void *kmalloc(size_t n) { return calloc(1, n); }
void kfree(void *p) { free(p); }
int usb_hc_register(struct usb_hc *hc, struct device *pci,
                    const struct usb_hc_ops *ops, void *priv)
{
    (void)hc; (void)pci; (void)ops; (void)priv;
    hc_register_calls++;
    return -1;
}
void usb_hc_unregister(struct usb_hc *hc) { (void)hc; }

static void fresh_controller(int bios_owned)
{
    memset(&g_xhci, 0, sizeof g_xhci);
    ring_pool = NULL;
    ring_pool_used = 0;
    memset(regs, 0, sizeof regs);
    memset(&device, 0, sizeof device);
    device.bus_type = DEV_BUS_PCI;
    device.res[0].flags = DEV_RES_MEM;
    device.res[0].size = sizeof regs;
    memcpy(device.name, "xhci-test", 10);
    *(uint32_t *)(regs + XCAP_CAPLENGTH) = 0x01000040u;
    *(uint32_t *)(regs + XCAP_HCSPARAMS1) = 8u | (4u << 24);
    *(uint32_t *)(regs + XCAP_HCCPARAMS1) = 1u | (0x40u << 16);
    *(uint32_t *)(regs + XCAP_RTSOFF) = 0x1000;
    *(uint32_t *)(regs + XCAP_DBOFF) = 0x2000;
    *(uint32_t *)(regs + 0x40 + XOP_USBSTS) = STS_HCH;
    *(uint32_t *)(regs + 0x100) = 1u | (bios_owned ? 1u << 16 : 0);
    *(uint32_t *)(regs + 0x104) = 0x0000ffffu;
    pci_command = 0;
    pci_writes = map_calls = mmio_reads = mmio_writes = 0;
    alloc_calls = free_calls = live = published = unsafe_free = 0;
    bme_writes = early_bme = legacy_ctl_writes = 0;
    bme_authorized = 0;
    release_bios = legacy_complete = reset_complete = 0;
    dcbaa_programmed = crcr_programmed = erst_programmed = 0;
    erst_latched = erst_latched_with_bme = erdp_after_erst = 0;
    reject_event_ring = hce_on_run = 0;
    start_stuck = halt_stuck = saw_master = 0;
    pci_fault = PCI_OK;
    bar_present = 1;
    hc_register_calls = 0;
}

static void recover_if_owned(void)
{
    if (!g_xhci.cap || !g_xhci.dma.buffers) return;
    pci_fault = PCI_OK;
    start_stuck = halt_stuck = 0;
    (void)xhci_shutdown();
}

static void happy_order(int compact)
{
    fresh_controller(1);
    /* Model firmware leaving BME set: the first checked Command transition
     * must remove it before the first ownership/MMIO transaction. */
    pci_command = PCI_CMD_MASTER;
    release_bios = 1;
    int rc = xhci_init(&device);
    int ordered = rc == 0 && !early_bme && bme_writes == 1 &&
        legacy_complete && reset_complete && dcbaa_programmed && crcr_programmed;
    check(ordered, "BME starts only after handoff reset and base ring programming");
    check(rc == 0 && erst_programmed && erst_latched_with_bme,
          "event table is latched only while bus mastering is enabled");
    check(rc == 0 && erdp_after_erst,
          "ERSTBA is latched before ERDP is published");
    if (!compact) {
        check((*(uint32_t *)(regs + 0x104) & 0x1fffffffu) == 0,
              "legacy SMI enables clear only after ownership");
        check(live > 0 && published > 0,
              "running controller retains submitted ring DMA");
    }
    if (rc == 0) {
        int stop = xhci_shutdown();
        if (!compact)
            check(stop == 0 && live == 0 && !(pci_command & PCI_CMD_MASTER),
                  "shutdown confirms stop and BME isolation before release");
    } else recover_if_owned();
}

static void event_ring_reject(void)
{
    fresh_controller(1);
    release_bios = 1;
    reject_event_ring = 1;
    int rc=xhci_init(&device);
    check(rc < 0 && erst_latched && bme_writes == 1 && live == 0 &&
          !(pci_command & PCI_CMD_MASTER) && !g_xhci.cap,
          "posted-write flush observes event-ring HCE before Run and isolates DMA");
    recover_if_owned();
}

static void delayed_run_hce(void)
{
    fresh_controller(1);
    release_bios = 1;
    hce_on_run = 1;
    int rc=xhci_init(&device);
    check(rc < 0 && erst_latched && erdp_after_erst && bme_writes == 1 &&
          live == 0 && !(pci_command & PCI_CMD_MASTER) && !g_xhci.cap,
          "Run-time HCE refuses start and isolates DMA");
    recover_if_owned();
}

static void bios_timeout(int compact)
{
    fresh_controller(1);
    release_bios = 0;
    uint32_t ctl = *(uint32_t *)(regs + 0x104);
    int rc = xhci_init(&device);
    unsigned maps = map_calls, allocs = alloc_calls;
    int again = xhci_init(&device);
    int refused = rc < 0 && again < 0 && g_xhci.quarantined &&
        !(pci_command & PCI_CMD_MASTER) && live == 0 && bme_writes == 0 &&
        legacy_ctl_writes == 0 && *(uint32_t *)(regs + 0x104) == ctl &&
        map_calls == maps && alloc_calls == allocs;
    check(refused, "BIOS ownership timeout refuses without SMI clear or re-probe");
    if (!compact) check(mmio_writes > 0, "timeout attempted the OS-owned semaphore");
    recover_if_owned();
}

static void dropped_prepare(int compact)
{
    fresh_controller(1);
    release_bios = 1;
    pci_fault = PCI_DROP_PREPARE;
    int rc = xhci_init(&device);
    int refused = rc < 0 && !g_xhci.pci && map_calls == 0 &&
        mmio_reads == 0 && mmio_writes == 0 && alloc_calls == 0 &&
        pci_command == 0;
    check(refused, "dropped MEM-only Command write refuses before BAR access");
    if (!compact) check(free_calls == 0 && unsafe_free == 0,
                        "pre-MMIO failure has no DMA object to release");
    recover_if_owned();
}

static void failed_restore(void)
{
    fresh_controller(1);
    pci_command = PCI_CMD_MASTER;
    pci_fault = PCI_RESTORE_UNACKNOWLEDGED;
    int rc = xhci_init(&device);
    unsigned maps = map_calls, allocs = alloc_calls, frees = free_calls;
    int again = xhci_init(&device);
    check(rc < 0 && again < 0 && g_xhci.quarantined && g_xhci.pci &&
          map_calls == maps && alloc_calls == allocs && free_calls == frees &&
          live == 0 && !(pci_command & PCI_CMD_MASTER),
          "unacknowledged Command restore isolates and permanently blocks probe");
}

static void malformed_extcap(void)
{
    fresh_controller(0);
    *(uint32_t *)(regs + XCAP_HCCPARAMS1) = 1u | (0x5000u << 16);
    int rc = xhci_init(&device);
    unsigned maps = map_calls;
    int again = xhci_init(&device);
    check(rc < 0 && again < 0 && g_xhci.quarantined && live == 0 &&
          !reset_complete && bme_writes == 0 && legacy_ctl_writes == 0 &&
          map_calls == maps,
          "out-of-BAR extended capability refuses before handoff or reset");
}

static void failed_post_master_isolation(int compact)
{
    fresh_controller(1);
    release_bios = 1;
    start_stuck = 1;
    pci_fault = PCI_DROP_ISOLATE_AFTER_MASTER;
    int rc = xhci_init(&device);
    unsigned retained = live, maps = map_calls, allocs = alloc_calls;
    int again = xhci_init(&device);
    int held = rc < 0 && again < 0 && retained > 0 && live == retained &&
        g_xhci.quarantined && g_xhci.dma.blocked &&
        (pci_command & PCI_CMD_MASTER) && free_calls == 0 && unsafe_free == 0 &&
        map_calls == maps && alloc_calls == allocs;
    check(held, "failed Command isolation retains unknown DMA and blocks re-probe");
    pci_fault = PCI_OK;
    start_stuck = 0;
    int recovered = xhci_shutdown();
    if (!compact)
        check(recovered == 0 && live == 0 && !(pci_command & PCI_CMD_MASTER),
              "later stop and isolation acknowledgement releases quarantine");
}

static void failed_usb_registration(int compact)
{
    fresh_controller(1);
    release_bios = 1;
    int rc = xhci_probe(&device);
    int stopped = rc < 0 && hc_register_calls == 1 && live == 0 &&
        !g_xhci.pci && !g_xhci.cap && !(pci_command & PCI_CMD_MASTER);
    check(stopped, "USB registration failure stops and isolates initialized xHCI");
    if (!compact)
        check(free_calls > 0 && unsafe_free == 0,
              "late probe failure releases DMA only after shutdown proof");
    recover_if_owned();
}

int main(void)
{
#if defined(TEST_NEG_IGNORE_BIOS)
    bios_timeout(1);
#elif defined(TEST_NEG_EARLY_BME)
    happy_order(1);
#elif defined(TEST_NEG_LATE_BME)
    happy_order(1);
#elif defined(TEST_NEG_ERDP_FIRST)
    happy_order(1);
#elif defined(TEST_NEG_EVENT_RING_READBACK)
    event_ring_reject();
#elif defined(TEST_NEG_RUN_HCE)
    delayed_run_hce();
#elif defined(TEST_NEG_COMMAND_READBACK)
    dropped_prepare(1);
#elif defined(TEST_NEG_RESTORE_FAILURE)
    failed_post_master_isolation(1);
#elif defined(TEST_NEG_REGISTER_FAILURE)
    failed_usb_registration(1);
#else
    happy_order(0);
    event_ring_reject();
    delayed_run_hce();
    bios_timeout(0);
    dropped_prepare(0);
    failed_restore();
    malformed_extcap();
    failed_post_master_isolation(0);
    failed_usb_registration(0);
#endif
    printf("xHCI lifecycle: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
