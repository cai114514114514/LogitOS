#include <stdint.h>
#include <stddef.h>
#include "virtio.h"
#include "pci.h"
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

static inline uint8_t  r8 (volatile uint8_t *b, int o) { return *(volatile uint8_t  *)(b + o); }
static inline uint16_t r16(volatile uint8_t *b, int o) { return *(volatile uint16_t *)(b + o); }
static inline uint32_t r32(volatile uint8_t *b, int o) { return *(volatile uint32_t *)(b + o); }
static inline void w8 (volatile uint8_t *b, int o, uint8_t  v) { *(volatile uint8_t  *)(b + o) = v; }
static inline void w16(volatile uint8_t *b, int o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }
static inline void w32(volatile uint8_t *b, int o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void w64(volatile uint8_t *b, int o, uint64_t v) { w32(b, o, (uint32_t)v); w32(b, o + 4, (uint32_t)(v >> 32)); }
static inline void barrier(void) { __asm__ volatile ("mfence" ::: "memory"); }

/* Don't let the timer preempt a virtio request mid-poll (same reasoning as the
 * ATA fix: the completion runs on QEMU's IO thread and an IF=0 busy-poll starves
 * it). interrupts.c checks this. */
volatile int g_virtio_busy = 0;
int virtio_busy(void) { return g_virtio_busy; }

/* Map the 64-bit MMIO BAR `barn` of a PCI device, return its base (identity). */
static uint64_t map_bar(uint8_t slot, int barn)
{
    uint32_t lo = pci_cfg_read(0, slot, 0, 0x10 + barn * 4);
    uint64_t base = lo & ~(uint64_t)0xF;
    if ((lo & 0x6) == 0x4)        /* 64-bit BAR: high dword in the next slot */
        base |= (uint64_t)pci_cfg_read(0, slot, 0, 0x10 + (barn + 1) * 4) << 32;
    vmm_map_range(base, base, 0x8000, VMM_WRITABLE | VMM_NOCACHE);
    return base;
}

int virtio_init(uint16_t devid, struct virtio_dev *vd, uint32_t want_lo)
{
    struct pci_dev dev;
    if (pci_find(VIRTIO_VENDOR, devid, &dev) != 0) return -1;
    vd->bus = dev.bus; vd->slot = dev.slot; vd->func = dev.func;
    vd->common = vd->notify_base = vd->isr = vd->device = NULL;
    vd->notify_mult = 0;

    /* Walk the vendor (0x09) capabilities to locate the modern cfg structures. */
    uint8_t cap = (uint8_t)(pci_cfg_read(0, dev.slot, 0, 0x34) & 0xFF);
    for (int g = 0; cap && g < 48; g++) {
        uint32_t d0 = pci_cfg_read(0, dev.slot, 0, cap);
        uint8_t vndr = d0 & 0xFF, next = (d0 >> 8) & 0xFF, type = (d0 >> 24) & 0xFF;
        if (vndr == 0x09) {
            uint8_t barn = pci_cfg_read(0, dev.slot, 0, cap + 4) & 0xFF;
            uint32_t off = pci_cfg_read(0, dev.slot, 0, cap + 8);
            volatile uint8_t *p = (volatile uint8_t *)(map_bar(dev.slot, barn) + off);
            switch (type) {
            case 1: vd->common = p; break;
            case 2: vd->notify_base = p; vd->notify_mult = pci_cfg_read(0, dev.slot, 0, cap + 16); break;
            case 3: vd->isr = p; break;
            case 4: vd->device = p; break;
            }
        }
        cap = next;
    }
    if (!vd->common || !vd->notify_base) { kprintf("[virtio] %x: missing caps\n", devid); return -1; }

    w8(vd->common, C_STATUS, 0);                       /* reset */
    while (r8(vd->common, C_STATUS) != 0) ;
    w8(vd->common, C_STATUS, VIRTIO_S_ACK);
    w8(vd->common, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER);

    /* Negotiate: VIRTIO_F_VERSION_1 (bit 32) is mandatory for modern; accept the
     * caller's low-32 device features that the device offers. */
    w32(vd->common, C_DRVFEAT_SEL, 1); w32(vd->common, C_DRVFEAT, 1);   /* bit 32 */
    w32(vd->common, C_DEVFEAT_SEL, 0);
    uint32_t devlo = r32(vd->common, C_DEVFEAT);
    w32(vd->common, C_DRVFEAT_SEL, 0); w32(vd->common, C_DRVFEAT, devlo & want_lo);

    w8(vd->common, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK);
    if (!(r8(vd->common, C_STATUS) & VIRTIO_S_FEATURES_OK)) {
        kprintf("[virtio] %x: FEATURES_OK rejected\n", devid);
        return -1;
    }
    kprintf("[virtio] %x up (slot %d, %d queues)\n", devid, dev.slot, r16(vd->common, C_NUM_QUEUES));
    return 0;
}

int virtio_queue_setup(struct virtio_dev *vd, int qidx, struct virtq *vq)
{
    w16(vd->common, C_QSELECT, (uint16_t)qidx);
    uint16_t size = r16(vd->common, C_QSIZE);
    if (size == 0) return -1;
    if (size > 256) size = 256;                        /* cap to a single frame each */
    w16(vd->common, C_QSIZE, size);

    vq->size = size;
    vq->desc  = (struct virtq_desc  *)pmm_alloc();     /* size*16 <= 4096 */
    vq->avail = (struct virtq_avail *)pmm_alloc();
    vq->used  = (struct virtq_used  *)pmm_alloc();
    if (!vq->desc || !vq->avail || !vq->used) return -1;
    memset(vq->desc, 0, 4096); memset(vq->avail, 0, 4096); memset(vq->used, 0, 4096);
    vq->last_used = 0;

    w64(vd->common, C_QDESC,   (uint64_t)vq->desc);
    w64(vd->common, C_QDRIVER, (uint64_t)vq->avail);
    w64(vd->common, C_QDEVICE, (uint64_t)vq->used);
    uint16_t noff = r16(vd->common, C_QNOTIFY_OFF);
    vq->notify = (volatile uint16_t *)(vd->notify_base + (uint32_t)noff * vd->notify_mult);
    w16(vd->common, C_QENABLE, 1);
    return 0;
}

void virtio_driver_ok(struct virtio_dev *vd)
{
    w8(vd->common, C_STATUS, VIRTIO_S_ACK | VIRTIO_S_DRIVER | VIRTIO_S_FEATURES_OK | VIRTIO_S_DRIVER_OK);
}

int virtio_request(struct virtio_dev *vd, struct virtq *vq, int qidx,
                   struct virtio_buf *bufs, int n)
{
    (void)vd;
    if (n <= 0 || n > vq->size) return -1;
    for (int i = 0; i < n; i++) {
        vq->desc[i].addr  = bufs[i].addr;
        vq->desc[i].len   = bufs[i].len;
        vq->desc[i].flags = (uint16_t)((bufs[i].device_writes ? VIRTQ_DESC_F_WRITE : 0) |
                                       (i < n - 1 ? VIRTQ_DESC_F_NEXT : 0));
        vq->desc[i].next  = (uint16_t)(i + 1);
    }
    uint16_t head = 0;
    vq->avail->ring[vq->avail->idx % vq->size] = head;
    barrier();
    vq->avail->idx++;
    barrier();

    /* Poll for completion with interrupts enabled (so QEMU's IO thread runs the
     * request) but non-preemptible (so we aren't switched away mid-poll). */
    uint64_t fl; __asm__ volatile ("pushfq; pop %0" : "=r"(fl) :: "memory");
    g_virtio_busy++;
    __asm__ volatile ("sti");
    *vq->notify = (uint16_t)qidx;
    int rc = -1;
    for (long spins = 0; spins < 200000000; spins++) {
        barrier();
        if (vq->used->idx != vq->last_used) {
            struct virtq_used_elem *e = &vq->used->ring[vq->last_used % vq->size];
            rc = (int)e->len;
            vq->last_used++;
            break;
        }
    }
    if (!(fl & 0x200)) __asm__ volatile ("cli");
    g_virtio_busy--;
    return rc;
}
