#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dma.h"
#include "driver.h"

unsigned char test_regs[65536] __attribute__((aligned(4096)));
static struct device devices[6];
static int ndevices;
static int checks, failures;
static int enables, disables, maps;
static int bar_available = 1;
static int fail_at = -1, allocation_calls, live, submitted;
static int ignore_initial_reset, ignore_cleanup_reset;
static int irq_release_result, irq_releases, snd_unregisters;
static int irq_reenter_on_release, model_stream_w1c;
static unsigned irq_reentries, playback_acks, capture_acks;
static unsigned playback_periods, capture_periods;
static uint64_t now_ns;
static uint16_t pci_command;
static int pci_master_stuck;
static int ignore_run_clear = -1;
static int ignore_ring_size_write;
static unsigned event_clock, corb_stop_seq, rirb_stop_seq;
static unsigned stream_stop_count, last_stream_stop_seq;
static unsigned reset_low_seq, master_enable_seq;

uint64_t test_read(const volatile void *p, unsigned n);
void test_write(volatile void *p, unsigned n, uint64_t v);

/* Compile the production driver after substituting only its six volatile MMIO
 * leaves.  Candidate policy, reset sequencing, ownership decisions and every
 * failure branch below remain the code linked into the kernel. */
#undef memset
#undef memcpy
#include "hda_driver.inc"

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

uint64_t test_read(const volatile void *p, unsigned n)
{
    uint64_t value = 0;
    memcpy(&value, (const void *)p, n);
    return value;
}

void test_write(volatile void *p, unsigned n, uint64_t value)
{
    size_t off = (const unsigned char *)p - test_regs;
    if (n == 1 && (off == CORBSIZE || off == RIRBSIZE)) {
        uint8_t *reg = (uint8_t *)p;
        if (ignore_ring_size_write) return;
        uint8_t caps = *reg & 0x70;
        uint8_t select = (uint8_t)value & 0x03;
        uint8_t supported = (uint8_t)(0x10u << select);
        if (caps & supported) *reg = (uint8_t)(caps | select);
        return;
    }
    if (n == 1 && (int)off == ignore_run_clear &&
        (*(uint8_t *)p & HDA_RUN) && !((uint8_t)value & HDA_RUN))
        return;
    if (n == 1 && (*(uint8_t *)p & HDA_RUN) &&
        !((uint8_t)value & HDA_RUN)) {
        unsigned seq = ++event_clock;
        if (off == CORBCTL && !corb_stop_seq) corb_stop_seq = seq;
        else if (off == RIRBCTL && !rirb_stop_seq) rirb_stop_seq = seq;
        else if (off >= SD_BASE && (off - SD_BASE) % 0x20 == SD_CTL) {
            stream_stop_count++;
            last_stream_stop_seq = seq;
        }
    }
    if (off == GCTL && value == 0 &&
        ((ignore_initial_reset && !submitted) ||
         (ignore_cleanup_reset && submitted)))
        return;
    if (off == GCTL && value == 0 && !reset_low_seq)
        reset_low_seq = ++event_clock;
    if (model_stream_w1c && n == 1 &&
        (off == g_hda.out_base + SD_STS || off == g_hda.in_base + SD_STS)) {
        uint8_t *reg = (uint8_t *)p;
        uint8_t ack = (uint8_t)value & 0x1c;
        if (*reg & ack) {
            unsigned idx = off == g_hda.out_base + SD_STS ?
                           (g_hda.out_base - SD_BASE) / 0x20 :
                           (g_hda.in_base - SD_BASE) / 0x20;
            if (off == g_hda.out_base + SD_STS) playback_acks++;
            else capture_acks++;
            *reg &= (uint8_t)~ack;
            if (!*reg) *(uint32_t *)(test_regs + INTSTS) &= ~(1u << idx);
        }
        return;
    }
    memcpy((void *)p, &value, n);
}

int dev_count(void) { return ndevices; }
struct device *dev_at(int i)
{ return i >= 0 && i < ndevices ? &devices[i] : NULL; }

void dev_enable(struct device *dev, int master)
{
    (void)dev;
    enables++;
    pci_command |= PCI_CMD_IO | PCI_CMD_MEM;
    if (master) {
        pci_command |= PCI_CMD_MASTER;
        if (!master_enable_seq) master_enable_seq = ++event_clock;
    }
}
void dev_disable(struct device *dev)
{
    (void)dev;
    disables++;
    pci_command &= (uint16_t)~(PCI_CMD_IO | PCI_CMD_MEM);
    if (!pci_master_stuck) pci_command &= (uint16_t)~PCI_CMD_MASTER;
}
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    (void)bus; (void)slot; (void)func;
    return off == PCI_CFG_COMMAND ? pci_command : UINT16_MAX;
}
uint64_t dev_bar_map(struct device *dev, int bar)
{
    (void)dev; (void)bar; maps++;
    return bar_available ? (uint64_t)(uintptr_t)test_regs : 0;
}

int dev_irq_request(struct device *dev, irq_handler_t fn, void *arg,
                    const char *name)
{ (void)dev; (void)fn; (void)arg; (void)name; return -1; }
int dev_irq_release(struct device *dev)
{
    (void)dev;
    irq_releases++;
    if (irq_reenter_on_release) {
        unsigned out_idx = (g_hda.out_base - SD_BASE) / 0x20;
        unsigned in_idx = (g_hda.in_base - SD_BASE) / 0x20;
        for (unsigned i = 0; i < 2; i++) {
            test_regs[g_hda.out_base + SD_STS] = 0x04;
            test_regs[g_hda.in_base + SD_STS] = 0x04;
            *(uint32_t *)(test_regs + INTSTS) |=
                (1u << out_idx) | (1u << in_idx);
            hda_isr(&g_hda);
            irq_reentries++;
        }
    }
    return irq_release_result;
}

void kprintf(const char *fmt, ...) { (void)fmt; }
uint64_t time_mono_ns(void) { now_ns += 1000000ull; return now_ns; }

void dma_device_init(struct dma_device *d, const char *name, uint64_t mask)
{
    memset(d, 0, sizeof *d);
    d->name = name;
    d->mask = mask;
}

struct dma_buffer *dma_alloc_coherent(struct dma_device *d, size_t size,
                                      size_t align, size_t boundary)
{
    (void)align; (void)boundary;
    if (allocation_calls++ == fail_at) return NULL;
    struct dma_buffer *b = calloc(1, sizeof *b);
    if (!b) abort();
    b->size = (size + 4095) & ~(size_t)4095;
    b->pages = b->size / 4096;
    b->cpu = aligned_alloc(4096, b->size);
    if (!b->cpu) abort();
    memset(b->cpu, 0, b->size);
    b->dma.value = 0x200000u + (uint64_t)allocation_calls * 0x10000u;
    b->phys = b->dma.value;
    b->state = DMA_READY;
    b->dev = d;
    b->next = d->buffers;
    d->buffers = b;
    live++;
    return b;
}

int dma_free_coherent(struct dma_buffer *b)
{
    if (!b || b->state == DMA_DEVICE_OWNED || b->state == DMA_QUARANTINED)
        return -1;
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
    b->token++;
    submitted++;
    return b->token;
}
int dma_buffer_complete(struct dma_buffer *b, uint64_t token)
{
    if (!b || b->state != DMA_DEVICE_OWNED || b->token != token) return -1;
    b->state = DMA_COMPLETED;
    return 0;
}
void dma_device_quarantine(struct dma_device *d)
{
    d->blocked = 1;
    for (struct dma_buffer *b = d->buffers; b; b = b->next)
        if (b->state == DMA_DEVICE_OWNED) b->state = DMA_QUARANTINED;
}
void dma_device_quiesced(struct dma_device *d)
{
    d->blocked = 1;
    for (struct dma_buffer *b = d->buffers; b; b = b->next) {
        b->state = DMA_QUIESCED;
        b->token = 0;
    }
}
void dma_wmb(void) { }
void dma_rmb(void) { }

int snd_register_device(struct snd_device *dev) { (void)dev; return 0; }
int snd_register_capture_device(struct snd_capdevice *dev)
{ (void)dev; return 0; }
void snd_unregister_device(struct snd_device *dev) { (void)dev; snd_unregisters++; }
void snd_unregister_capture_device(struct snd_capdevice *dev)
{ (void)dev; snd_unregisters++; }
void snd_period_elapsed(struct snd_device *dev) { (void)dev; playback_periods++; }
void snd_capture_period_elapsed(struct snd_capdevice *dev)
{ (void)dev; capture_periods++; }
void snd_init(void) { }

static struct device make_dev(uint16_t vendor, uint16_t device,
                              uint8_t bus, uint8_t slot, uint8_t func,
                              uint8_t class_code, uint8_t subclass)
{
    struct device d;
    memset(&d, 0, sizeof d);
    d.bus_type = DEV_BUS_PCI;
    d.vendor = vendor;
    d.device = device;
    d.bus = bus;
    d.slot = slot;
    d.func = func;
    d.class_code = class_code;
    d.subclass = subclass;
    snprintf(d.name, sizeof d.name, "0000:%02x:%02x.%u", bus, slot, func);
    d.res[0].start = 0xf0000000;
    d.res[0].size = sizeof test_regs;
    d.res[0].flags = DEV_RES_MEM;
    return d;
}

static void __attribute__((unused)) clear_active(void)
{
    memset(&g_hda, 0, sizeof g_hda);
    memset(test_regs, 0, sizeof test_regs);
    enables = disables = maps = 0;
    bar_available = 1;
    fail_at = -1;
    allocation_calls = submitted = 0;
    ignore_initial_reset = ignore_cleanup_reset = 0;
    irq_release_result = irq_releases = snd_unregisters = 0;
    irq_reenter_on_release = model_stream_w1c = 0;
    irq_reentries = playback_acks = capture_acks = 0;
    playback_periods = capture_periods = 0;
    pci_command = 0;
    pci_master_stuck = 0;
    ignore_run_clear = -1;
    ignore_ring_size_write = 0;
    event_clock = corb_stop_seq = rirb_stop_seq = 0;
    stream_stop_count = last_stream_stop_seq = 0;
    reset_low_seq = master_enable_seq = 0;
    now_ns = 0;
}

static void __attribute__((unused)) release_fixture_quarantine(void)
{
    dma_device_quiesced(&g_hda_quarantined_dma);
    while (g_hda_quarantined_dma.buffers)
        if (dma_free_coherent(g_hda_quarantined_dma.buffers) != 0) abort();
}

static void setup_x79_with_gtx(void)
{
    ndevices = 3;
    devices[0] = make_dev(0x10de, 0x1c81, 1, 0, 0, 0x03, 0x00);
    devices[1] = make_dev(0x10de, 0x0fb9, 1, 0, 1, 0x04, 0x03);
    devices[2] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
}

static void __attribute__((unused)) prepare_dma_failure(void)
{
    *(uint16_t *)(test_regs + GCAP) = 0x1100; /* one input + one output */
    *(uint16_t *)(test_regs + STATESTS) = 1;
    test_regs[CORBSIZE] = 0x40;               /* 256-entry ring supported */
    test_regs[RIRBSIZE] = 0x40;
    ignore_cleanup_reset = 1;
}

static void __attribute__((unused)) prepare_firmware_dma(void)
{
    *(uint16_t *)(test_regs + GCAP) = 0x1100; /* one input + one output */
    *(uint16_t *)(test_regs + STATESTS) = 1;
    test_regs[CORBSIZE] = 0x40;
    test_regs[RIRBSIZE] = 0x40;
    test_regs[CORBCTL] = HDA_RUN;
    test_regs[RIRBCTL] = HDA_RUN;
    test_regs[SD_BASE + SD_CTL] = HDA_RUN;
    test_regs[SD_BASE + 0x20 + SD_CTL] = HDA_RUN;
    pci_command = PCI_CMD_MEM | PCI_CMD_MASTER;
}

static void __attribute__((unused)) prepare_live_instance(void)
{
    dma_device_init(&g_hda.dma, "hda", DMA_MASK_64);
    g_hda.mmio = test_regs;
    g_hda.dev = &devices[0];
    g_hda.in_base = SD_BASE;
    g_hda.out_base = 0xa0;
    g_hda.ring_dma = dma_alloc_coherent(&g_hda.dma, 4096, 4096, 0);
    g_hda.ring = g_hda.ring_dma->cpu;
    g_hda.snd.priv = &g_hda;
    dma_buffer_submit(g_hda.ring_dma);
    *(uint32_t *)(test_regs + GCTL) = 1;
}

int main(void)
{
    setup_x79_with_gtx();

#ifdef HDA_X79_NEGCTL_MASTER_BEFORE_QUIESCE
    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    prepare_firmware_dma();
    check(hda_probe(&devices[0]) < 0 &&
          corb_stop_seq && rirb_stop_seq && stream_stop_count == 2 &&
          corb_stop_seq < reset_low_seq && rirb_stop_seq < reset_low_seq &&
          last_stream_stop_seq < reset_low_seq &&
          reset_low_seq < master_enable_seq,
          "firmware DMA is stopped and read back before CRST and BME");
#elif defined(HDA_X79_NEGCTL_IGNORE_STUCK_FIRMWARE_RUN)
    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    prepare_firmware_dma();
    ignore_run_clear = CORBCTL;
    check(hda_probe(&devices[0]) < 0 &&
          !reset_low_seq && !master_enable_seq,
          "stuck firmware RUN state blocks CRST and BME");
#elif defined(HDA_X79_NEGCTL_FORCE_256_RINGS)
    clear_active();
    g_hda.mmio = test_regs;
    test_regs[CORBSIZE] = 0x20; /* this controller implements only 16 */
    unsigned entries = 0;
    check(hda_set_ring_size(&g_hda, CORBSIZE, &entries) == 0 &&
          entries == 16 && (test_regs[CORBSIZE] & 3) == 1,
          "16-entry-only command ring is negotiated and read back");
#elif defined(HDA_X79_NEGCTL_ACCEPT_GPU_AUDIO)
    check(!hda_controller_candidate(&devices[1]),
          "GTX 1050 display-function HDA is declined before X79 onboard audio");
#elif defined(HDA_X79_NEGCTL_KEEP_POISONED_INSTANCE)
    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    check(hda_probe(&devices[0]) < 0, "fixture reaches no-output-stream failure");
    check(g_hda.mmio == NULL,
          "failed HDA probe clears the active instance for a later controller");
#elif defined(HDA_X79_NEGCTL_RELEASE_UNACKED_DMA)
    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    prepare_dma_failure();
    check(hda_probe(&devices[0]) < 0, "fixture reaches post-publication failure");
    check(live > 0,
          "unacknowledged controller reset retains hardware-owned DMA");
#elif defined(HDA_X79_NEGCTL_REMOVE_AFTER_IRQ_RELEASE_FAIL)
    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    prepare_live_instance();
    irq_release_result = -1;
    hda_remove(&devices[0]);
    check(g_hda.mmio && live == 1 && snd_unregisters == 0,
          "failed IRQ teardown retains HDA callbacks and DMA");
#elif defined(HDA_X79_NEGCTL_ISR_EARLY_RETURN_ON_REMOVE)
    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    prepare_live_instance();
    g_hda.has_capture = 1;
    model_stream_w1c = 1;
    irq_reenter_on_release = 1;
    irq_release_result = -1;
    hda_remove(&devices[0]);
    check(irq_reentries == 2 && playback_acks == 2 && capture_acks == 2 &&
          playback_periods == 0 && capture_periods == 0 &&
          test_regs[g_hda.out_base + SD_STS] == 0 &&
          test_regs[g_hda.in_base + SD_STS] == 0,
          "removing ISR acknowledges both stream sources on every failed-release re-entry");
#else
    clear_active();
    g_hda.mmio = test_regs;
    unsigned entries = 0;
    check(CORBSIZE == 0x4e && RIRBSIZE == 0x5e,
          "ring-size registers use their specification offsets");
    test_regs[CORBSIZE] = 0x10;
    check(hda_set_ring_size(&g_hda, CORBSIZE, &entries) == 0 &&
          entries == 2 && (test_regs[CORBSIZE] & 3) == 0,
          "2-entry-only command ring is negotiated");
    test_regs[CORBSIZE] = 0x20;
    check(hda_set_ring_size(&g_hda, CORBSIZE, &entries) == 0 &&
          entries == 16 && (test_regs[CORBSIZE] & 3) == 1,
          "16-entry-only command ring is negotiated");
    test_regs[RIRBSIZE] = 0x40;
    check(hda_set_ring_size(&g_hda, RIRBSIZE, &entries) == 0 &&
          entries == 256 && (test_regs[RIRBSIZE] & 3) == 2,
          "256-entry response ring is negotiated");
    test_regs[RIRBSIZE] = 0x70;
    check(hda_set_ring_size(&g_hda, RIRBSIZE, &entries) == 0 &&
          entries == 256,
          "largest advertised response ring is preferred");
    test_regs[CORBSIZE] = 0;
    check(hda_set_ring_size(&g_hda, CORBSIZE, &entries) < 0,
          "ring with no advertised size is rejected");
    test_regs[CORBSIZE] = 0x20;
    ignore_ring_size_write = 1;
    check(hda_set_ring_size(&g_hda, CORBSIZE, &entries) < 0,
          "ring-size selection must read back");
    ignore_ring_size_write = 0;

    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    prepare_firmware_dma();
    check(hda_probe(&devices[0]) < 0 &&
          corb_stop_seq && rirb_stop_seq && stream_stop_count == 2 &&
          corb_stop_seq < reset_low_seq && rirb_stop_seq < reset_low_seq &&
          last_stream_stop_seq < reset_low_seq &&
          reset_low_seq < master_enable_seq,
          "firmware DMA is stopped and read back before CRST and BME");

    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    prepare_firmware_dma();
    ignore_run_clear = CORBCTL;
    check(hda_probe(&devices[0]) < 0 &&
          !reset_low_seq && !master_enable_seq,
          "stuck firmware RUN state blocks CRST and BME");

    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    pci_command = PCI_CMD_MASTER;
    pci_master_stuck = 1;
    check(hda_probe(&devices[0]) < 0 && maps == 0 &&
          !g_hda.mmio && (pci_command & PCI_CMD_MASTER),
          "unconfirmed PCI MEM-only state is refused before BAR access");

    clear_active();
    setup_x79_with_gtx();
    check(!hda_controller_candidate(&devices[1]),
          "GTX 1050 display-function HDA is declined before X79 onboard audio");
    check(hda_controller_candidate(&devices[2]),
          "8086:1d20 at 00:1b.0 is the preferred X79 onboard HDA");

    clear_active();
    setup_x79_with_gtx();
    check(hda_probe(&devices[1]) < 0,
          "production probe declines the GTX 1050 HDMI function");
    check(enables == 0 && maps == 0 && disables == 0 && g_hda.mmio == NULL,
          "declined GPU audio has no PCI, BAR, MMIO, or singleton side effect");

    devices[0] = make_dev(0x1234, 0x1111, 2, 4, 0, 0x04, 0x03);
    devices[1] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    ndevices = 2;
    check(!hda_controller_candidate(&devices[0]),
          "generic HDA is deferred when exact X79 onboard audio exists");

    ndevices = 1;
    check(hda_controller_candidate(&devices[0]),
          "standalone class HDA remains available when no X79 controller exists");
    devices[0].subclass = 0x01;
    check(!hda_controller_candidate(&devices[0]),
          "non-HDA multimedia function is rejected by the production policy");

    clear_active();
    ndevices = 1;
    devices[0] = make_dev(0x8086, 0x1d20, 0, 0x1b, 0, 0x04, 0x03);
    check(hda_probe(&devices[0]) < 0, "zero-stream controller fails probe");
    check(!g_hda.mmio && !g_hda.dev && !g_hda.dma.buffers,
          "zero-stream failure restores an empty active instance");
    check(enables == 1 && maps == 1 && disables == 2,
          "failed mapped probe isolates PCI before BAR access and after cleanup");

    *(uint16_t *)(test_regs + GCAP) = 0x1000;
    check(hda_probe(&devices[0]) < 0, "no-codec controller fails probe");
    check(maps == 2 && !g_hda.mmio,
          "a second controller attempt proceeds after the first failure");

    clear_active();
    *(uint32_t *)(test_regs + GCTL) = 1;
    ignore_initial_reset = 1;
    check(hda_probe(&devices[0]) < 0, "unacknowledged reset-low fails probe");
    check(!g_hda.mmio && disables == 2,
          "reset-low failure remains retryable without DMA allocation");

    clear_active();
    bar_available = 0;
    check(hda_probe(&devices[0]) < 0, "missing BAR fails probe");
    check(!g_hda.mmio && enables == 1 && disables == 2,
          "missing BAR restores PCI and singleton state");

    clear_active();
    *(uint16_t *)(test_regs + GCAP) = 0x1100;
    *(uint16_t *)(test_regs + STATESTS) = 1;
    fail_at = 0;
    check(hda_probe(&devices[0]) < 0, "partial CORB allocation fails probe");
    check(live == 0 && !g_hda.mmio,
          "acknowledged reset releases partial unpublished DMA and clears state");

    clear_active();
    prepare_dma_failure();
    check(hda_probe(&devices[0]) < 0,
          "codec-path failure reaches controller-wide reset cleanup");
    check(live == 2 && !g_hda.mmio && !g_hda.dma.buffers,
          "unacknowledged reset moves owned CORB/RIRB out of active state");
    check(g_hda_quarantined_dma.buffers &&
          g_hda_quarantined_dma.buffers->state == DMA_QUARANTINED,
          "unacknowledged reset leaves retained DMA quarantined");

    ignore_cleanup_reset = 0;
    *(uint16_t *)(test_regs + GCAP) = 0;
    check(hda_probe(&devices[0]) < 0 && maps == 2,
          "a later onboard HDA attempt proceeds beside retained quarantine");
    release_fixture_quarantine();
    check(live == 0, "fixture releases quarantine only after simulated power removal");

    clear_active();
    prepare_live_instance();
    g_hda.has_capture = 1;
    model_stream_w1c = 1;
    irq_reenter_on_release = 1;
    irq_release_result = -1;
    hda_remove(&devices[0]);
    check(irq_releases == 1 && g_hda.mmio && !g_hda.removing &&
          live == 1 && snd_unregisters == 0,
          "failed IRQ teardown retains HDA callbacks, DMA, and retry state");

    /* A failed PCI source suppression leaves the handler live.  Even while
     * remove owns the instance it must ACK every level-triggered stream status,
     * while suppressing callbacks into upper audio objects being torn down. */
    check(irq_reentries == 2 && playback_acks == 2 && capture_acks == 2 &&
          playback_periods == 0 && capture_periods == 0 &&
          test_regs[g_hda.out_base + SD_STS] == 0 &&
          test_regs[g_hda.in_base + SD_STS] == 0 && g_hda.mmio,
          "removing ISR acknowledges both stream sources on every failed-release re-entry");

    irq_reenter_on_release = 0;
    irq_release_result = 0;
    hda_remove(&devices[0]);
    check(irq_releases == 2 && !g_hda.mmio && live == 0 && snd_unregisters == 2,
          "confirmed IRQ teardown permits complete HDA removal");
#endif

    printf("HDA_X79: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
