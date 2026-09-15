#include <stdint.h>
#include <stddef.h>
#include "virtio.h"
#include "pci.h"
#include "driver.h"
#include "vmm.h"
#include "pmm.h"
#include "kprintf.h"

void *memset(void *, int, size_t);

/* virtio_pci_common_cfg register offsets */
#define C_DEVFEAT_SEL  0x00
#define C_DEVFEAT      0x04
#define C_DRVFEAT_SEL  0x08
#define C_DRVFEAT      0x0C
#define C_NUM_QUEUES   0x12
#define C_STATUS       0x14
#define C_QSELECT      0x16
#define C_QSIZE        0x18
#define C_QENABLE      0x1C
#define C_QNOTIFY_OFF  0x1E
#define C_QDESC        0x20
#define C_QDRIVER      0x28
#define C_QDEVICE      0x30

/* The host harness replaces only hardware effects; queue construction, DMA
 * allocation and timeout ownership below are the production implementation. */
#ifdef VIRTIO_HOSTTEST
uint8_t virtio_test_r8(volatile uint8_t *, int);
void virtio_test_w8(volatile uint8_t *, int, uint8_t);
void virtio_test_poll(struct virtio_dev *, struct virtq *);
#define VIRTIO_POLL_SPINS 32
static inline uint8_t r8(volatile uint8_t *b, int o) { return virtio_test_r8(b, o); }
#else
#define VIRTIO_POLL_SPINS 200000000
static inline uint8_t r8(volatile uint8_t *b, int o) { return *(volatile uint8_t *)(b + o); }
#endif
static inline uint16_t r16(volatile uint8_t *b, int o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t r32(volatile uint8_t *b, int o) { return *(volatile uint32_t *)(b + o); }
static inline void w8(volatile uint8_t *b, int o, uint8_t v) {
#ifdef VIRTIO_HOSTTEST
    virtio_test_w8(b, o, v);
#else
    *(volatile uint8_t *)(b + o) = v;
#endif
}
static inline void w16(volatile uint8_t *b, int o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void w32(volatile uint8_t *b, int o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void w64(volatile uint8_t *b, int o, uint64_t v) { w32(b, o, (uint32_t)v); w32(b, o + 4, (uint32_t)(v >> 32)); }
static inline void barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* Don't let the timer preempt a virtio request mid-poll (same reasoning as the
 * ATA fix: the completion runs on QEMU's IO thread and an IF=0 busy-poll starves
 * it). interrupts.c checks this. */
volatile int g_virtio_busy = 0;
int virtio_busy(void) { return g_virtio_busy; }

int virtio_init(uint16_t devid, struct virtio_dev *vd, uint32_t want_lo)
{
    /* Off the device registry rather than a bus-0 slot scan: the model has
     * already sized every BAR, so the capability offsets below can be bounds-
     * checked against the BAR's REAL size instead of the 0x8000 this used to
     * assume, and a virtio device behind a bridge is found like any other. */
    if (!vd || vd->failed || vd->started) return -1;
    return virtio_init_device(dev_find_id(VIRTIO_VENDOR, devid, NULL), vd, want_lo);
}

int virtio_init_device(struct device *dev, struct virtio_dev *vd, uint32_t want_lo)
{
    /* Keyboard, mouse and tablet all have PCI ID 1af4:1052. A probe already
     * owns a particular function: searching again by ID would reset the first
     * keyboard when binding the mouse, then point both drivers at its rings.
     * Keep the legacy first-match entry point above for singleton callers. */
    if (!dev || !vd || vd->failed || vd->started) return -1;
    uint16_t devid = dev->device;
    if (dev_enable_checked(dev, 0) != 0) {
        kprintf("[virtio] %x: PCI Command decode rejected\n", devid);
        return -1;
    }

    vd->dev = dev;
    dma_device_init(&vd->dma, dev->name, DMA_MASK_64);
    vd->bus = dev->bus; vd->slot = dev->slot; vd->func = dev->func;
    vd->common = vd->notify_base = vd->isr = vd->device = NULL;
    vd->notify_mult = 0;

    /* Each modern virtio config structure is described by its own vendor (0x09)
     * capability: cap+3 = type, cap+4 = BAR index, cap+8 = offset in that BAR. */
    for (uint8_t cap = pci_cap_next(dev->bus, dev->slot, dev->func, PCI_CAP_VENDOR, 0);
         cap;
         cap = pci_cap_next(dev->bus, dev->slot, dev->func, PCI_CAP_VENDOR, cap)) {
        uint32_t d0   = pci_cfg_read(dev->bus, dev->slot, dev->func, cap);
        uint8_t  type = (uint8_t)((d0 >> 24) & 0xFF);
        int      barn = (int)(pci_cfg_read(dev->bus, dev->slot, dev->func, (uint16_t)(cap + 4)) & 0xFF);
        uint32_t off  = pci_cfg_read(dev->bus, dev->slot, dev->func, (uint16_t)(cap + 8));
        uint32_t len  = pci_cfg_read(dev->bus, dev->slot, dev->func, (uint16_t)(cap + 12));

        if (barn < 0 || barn >= DEV_NRES) continue;
        uint64_t base = dev_bar_map(dev, barn);
        if (!base) continue;
        if ((uint64_t)off + len > dev->res[barn].size) continue;   /* outside its BAR */

        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(base + off);
        switch (type) {
        case 1: vd->common = p; break;
        case 2: vd->notify_base = p;
                vd->notify_mult = pci_cfg_read(dev->bus, dev->slot, dev->func, (uint16_t)(cap + 16));
                break;
        case 3: vd->isr = p; break;
        case 4: vd->device = p; break;
        }
    }
    if (!vd->common || !vd->notify_base) { kprintf("[virtio] %x: missing caps\n", devid); return -1; }

    w8(vd->common, C_STATUS, 0);                       /* reset */
    for (long i = 0; i < VIRTIO_POLL_SPINS; i++) {
        if (r8(vd->common, C_STATUS) == 0) break;
    }
    if (r8(vd->common, C_STATUS) != 0) {
        kprintf("[virtio] %x: reset timeout\n", devid);
        return -1;
    }
    vd->quiesced = 1;
    w8(vd->common, C_STATUS, VIRTIO_S_ACK);
    w8(vd->common, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER);

    /* Negotiate: VIRTIO_F_VERSION_1 (bit 32) is mandatory for modern; accept the
     * caller's low-32 device features that the device offers. */
    w32(vd->common, C_DRVFEAT_SEL, 1); w32(vd->common, C_DRVFEAT, 1);   /* bit 32 */
    w32(vd->common, C_DEVFEAT_SEL, 0);
    uint32_t devlo = r32(vd->common, C_DEVFEAT);
    vd->features_lo = devlo & want_lo;          /* what we actually got, for the driver to test */
    w32(vd->common, C_DRVFEAT_SEL, 0); w32(vd->common, C_DRVFEAT, vd->features_lo);

    w8(vd->common, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(r8(vd->common, C_STATUS) & VIRTIO_S_FEATURES_OK)) {
        kprintf("[virtio] %x: FEATURES_OK rejected\n", devid);
        return -1;
    }
    /* Reset and FEATURES_OK acknowledged a quiet transport.  No queue or
     * payload has been published yet, so a failed BME readback is unwindable. */
    if (dev_enable_checked(dev, 1) != 0) {
        kprintf("[virtio] %x: PCI bus-master enable rejected\n", devid);
        w8(vd->common, C_STATUS, 0);
        vd->failed = 1;
        return -1;
    }
    kprintf("[virtio] %x up (%s, %d queues)\n", devid, dev->name, r16(vd->common, C_NUM_QUEUES));
    return 0;
}

int virtio_queue_setup(struct virtio_dev *vd, int qidx, struct virtq *vq)
{
    IO_GUARD(&vd->gate);
    if (vd->failed || vd->started || vd->nqueues == 4) return -1;
    w16(vd->common, C_QSELECT, (uint16_t)qidx);
    uint16_t size = r16(vd->common, C_QSIZE);
    if (size == 0) return -1;
    if (size > 256) size = 256;                        /* cap to a single frame each */
    w16(vd->common, C_QSIZE, size);

    vq->size = size;
    /* Previously pmm_alloc pointers doubled as physical queue addresses. Every
     * ring now owns a coherent handle; even low pages use a different CPU alias. */
    if (!(vq->desc_mem = dma_alloc_coherent(&vd->dma, 4096, 4096, 0)) ||
        !(vq->avail_mem = dma_alloc_coherent(&vd->dma, 4096, 4096, 0)) ||
        !(vq->used_mem = dma_alloc_coherent(&vd->dma, 4096, 4096, 0))) {
        dma_free_coherent(vq->desc_mem); vq->desc_mem = NULL;
        dma_free_coherent(vq->avail_mem); vq->avail_mem = NULL;
        dma_free_coherent(vq->used_mem); vq->used_mem = NULL;
        return -1;  /* these new pages were never published */
    }
    vq->desc = vq->desc_mem->cpu;
    vq->avail = vq->avail_mem->cpu;
    vq->used = vq->used_mem->cpu;
    memset(vq->desc, 0, 4096); memset(vq->avail, 0, 4096); memset(vq->used, 0, 4096);
    vq->last_used = 0;
    vq->free_head = 0;

    w64(vd->common, C_QDESC,   dma_addr_value(vq->desc_mem->dma));
    w64(vd->common, C_QDRIVER, dma_addr_value(vq->avail_mem->dma));
    w64(vd->common, C_QDEVICE, dma_addr_value(vq->used_mem->dma));
    uint16_t noff = r16(vd->common, C_QNOTIFY_OFF);
    vq->notify = (volatile uint16_t *)(vd->notify_base + (uint32_t)noff * vd->notify_mult);
    barrier();
    w16(vd->common, C_QENABLE, 1);
    vd->queues[vd->nqueues++] = vq;
    kprintf("[virtio-dma] %s queue=%d cpu=%p dma=%p avail=%p used=%p\n",
            vd->dev->name, qidx, vq->desc, dma_addr_value(vq->desc_mem->dma),
            dma_addr_value(vq->avail_mem->dma), dma_addr_value(vq->used_mem->dma));
    return 0;
}

static int virtio_stop_locked(struct virtio_dev *vd)
{
    vd->failed = 1;  /* reject new submissions before asking the device to stop */
    if (vd->quiesced) { dma_device_quiesced(&vd->dma); return 0; }
    if (!vd->common) return -1;
    w8(vd->common, C_STATUS, 0);
    for (long spins = 0; spins < VIRTIO_POLL_SPINS; spins++) {
        if (r8(vd->common, C_STATUS) == 0) {
            barrier();
            dma_device_quiesced(&vd->dma);
            vd->quiesced = 1;
            vd->started = 0;
            return 0;
        }
    }
    /* Keep queue handles and client backing. A timeout is not a release fence;
     * even a late used entry does not make other queues in this reset domain safe. */
    dma_device_quarantine(&vd->dma);
    kprintf("[virtio] reset failed: DMA quarantined, device disabled\n");
    return -1;
}

void virtio_queue_release(struct virtio_dev *vd, struct virtq *vq)
{
    IO_GUARD(&vd->gate);
    if (!vd->quiesced) return;
    dma_free_coherent(vq->desc_mem); vq->desc_mem = NULL;
    dma_free_coherent(vq->avail_mem); vq->avail_mem = NULL;
    dma_free_coherent(vq->used_mem); vq->used_mem = NULL;
    vq->desc = NULL; vq->avail = NULL; vq->used = NULL; vq->size = 0;
}

void virtio_driver_ok(struct virtio_dev *vd)
{
    IO_GUARD(&vd->gate);
    if (vd->failed) return;
    for (unsigned i = 0; i < vd->nqueues; i++) {
        struct virtq *q = vd->queues[i];
        if (!dma_buffer_submit(q->desc_mem) || !dma_buffer_submit(q->avail_mem) ||
            !dma_buffer_submit(q->used_mem)) { virtio_stop_locked(vd); return; }
    }
    vd->quiesced = 0; vd->started = 1;
    barrier();
    w8(vd->common, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);
}

int virtio_request(struct virtio_dev *vd, struct virtq *vq, int qidx,
                   struct virtio_buf *bufs, int n)
{
    IO_GUARD(&vd->gate);
    if (vd->failed || !vd->started || n <= 0 || n > vq->size) return -1;
    /* Descriptors rotate through the ring: after a timeout the device may still
     * own the old chain, and its late completion must not alias our head.
     * Correction: rotation alone cannot protect reused payloads. A timeout now
     * permanently stops this device before callers may release any mapping. */
    uint64_t tokens[256];
    for (int i = 0; i < n; i++) {
        tokens[i] = bufs[i].owner ? dma_buffer_submit(bufs[i].owner) : 0;
        if (bufs[i].owner && !tokens[i]) {
            for (int j = 0; j < i; j++)
                if (bufs[j].owner) dma_buffer_complete(bufs[j].owner, tokens[j]);
            return -1;
        }
    }
    uint16_t head = vq->free_head;
    for (int i = 0; i < n; i++) {
        uint16_t d = (uint16_t)((head + i) % vq->size);
        vq->desc[d].addr  = dma_addr_value(bufs[i].dma_addr);
        vq->desc[d].len   = bufs[i].len;
        vq->desc[d].flags = (uint16_t)((bufs[i].device_writes ? VIRTQ_DESC_F_WRITE : 0) |
                                       (i < n - 1 ? VIRTQ_DESC_F_NEXT : 0));
        vq->desc[d].next  = (uint16_t)((d + 1) % vq->size);
    }
    vq->free_head = (uint16_t)((vq->free_head + n) % vq->size);
    vq->avail->ring[vq->avail->idx % vq->size] = head;
    barrier();
    vq->avail->idx++;
    barrier();

    /* Poll for completion with interrupts enabled (so QEMU's IO thread runs the
     * request) but non-preemptible (so we aren't switched away mid-poll). */
#ifndef VIRTIO_HOSTTEST
    uint64_t fl; __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
#endif
    __atomic_fetch_add(&g_virtio_busy, 1, __ATOMIC_RELAXED);
#ifndef VIRTIO_HOSTTEST
    __asm__ volatile ("sti");
#endif
    *vq->notify = (uint16_t)qidx;
    int rc = -1;
    for (long spins = 0; spins < VIRTIO_POLL_SPINS; spins++) {
#ifdef VIRTIO_HOSTTEST
        virtio_test_poll(vd, vq);
#endif
        barrier();
        if (vq->used->idx != vq->last_used) {
            struct virtq_used_elem *e = &vq->used->ring[vq->last_used % vq->size];
            vq->last_used++;
            if (e->id != head) continue;        /* stale completion from a timed-out request */
            rc = (int)e->len;
            break;
        }
    }
    if (rc < 0) virtio_stop_locked(vd);
    else for (int i = 0; i < n; i++)
        if (bufs[i].owner && dma_buffer_complete(bufs[i].owner, tokens[i])) rc = -1;
#ifndef VIRTIO_HOSTTEST
    if (!(fl & 0x200)) __asm__ volatile ("cli");
#endif
    __atomic_fetch_sub(&g_virtio_busy, 1, __ATOMIC_RELAXED);
    return rc;
}

int virtio_stop(struct virtio_dev *vd)
{
    IO_GUARD(&vd->gate);
    return virtio_stop_locked(vd);
}
