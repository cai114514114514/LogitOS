/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include "virtio_scsi.h"
#include "virtio.h"
#include "blkdev.h"
#include "driver.h"
#include "kprintf.h"
#include "panic.h"

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

/* Virtio 1.2 section 5.6. A virtio-scsi controller transports SCSI commands;
 * unlike virtio-blk, capacity, medium type and cache barriers come from the
 * addressed logical unit. No vendor command or QEMU-only opcode is used.
 * https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html
 *
 * Deliberate first boundary: one controller, target 0..7, LUN 0, direct-access
 * disks with 512-byte logical blocks. The block layer has a 512-byte contract;
 * registering a 4Kn disk without translation would silently read the wrong
 * offsets. CD-ROM, hotplug, task-management and multiple request queues remain
 * absent. No related feature is negotiated and the event queue stays empty.
 * Requests are synchronous and controller-serialized, including different
 * targets: the existing block layer can give each medium its own gate, but the
 * three queues and command staging here share one reset domain. */
#define VSCSI_TRANSITIONAL 0x1004
#define VSCSI_MODERN       0x1048
#define VSCSI_TARGETS      8
#define VSCSI_REQ_QUEUE    2
#define VSCSI_RESPONSE_OFF 128
#define VSCSI_CDB_SIZE     32
#define VSCSI_SENSE_SIZE   96

struct vscsi_cmd {
    uint8_t lun[8];
    uint64_t tag;
    uint8_t task_attr, priority, crn;
    uint8_t cdb[VSCSI_CDB_SIZE];
} __attribute__((packed));
struct vscsi_resp {
    uint32_t sense_len, residual;
    uint16_t status_qualifier;
    uint8_t status, response;
    uint8_t sense[VSCSI_SENSE_SIZE];
} __attribute__((packed));
_Static_assert(sizeof(struct vscsi_cmd) == 51, "virtio-scsi command wire size");
_Static_assert(sizeof(struct vscsi_resp) == 108, "virtio-scsi response wire size");

struct vscsi_disk {
    unsigned target;
    uint64_t sectors;
    struct blkdev *blk;
};
static io_lock_t scsi_gate = IO_LOCK_INIT;
static struct virtio_dev scsi;
static struct virtq queues[3];
static struct dma_buffer *control;
static struct dma_mapping *payload;
static struct vscsi_disk disks[VSCSI_TARGETS];
static uint64_t next_tag;
static uint32_t max_sectors;
static int ready, initialized, disk_count;

static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p)
{ return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put_be32(uint8_t *p, uint32_t n)
{ p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n; }
static void put_be64(uint8_t *p, uint64_t n)
{ put_be32(p, (uint32_t)(n >> 32)); put_be32(p + 4, (uint32_t)n); }

static void scsi_release(void)
{
    ready = 0;
    if (virtio_stop(&scsi)) return; /* retain quarantined storage on reset failure */
    if (payload) { dma_unmap(payload); payload = NULL; }
    dma_free_coherent(control); control = NULL;
    for (int i = 0; i < 3; ++i) virtio_queue_release(&scsi, &queues[i]);
}

/* Returns transferred bytes, -2 for a target that is absent, -3 for Unit
 * Attention, and -1 for any other error. Callers may retry Unit Attention only
 * while discovering a disk: silently replaying a WRITE after an ambiguous
 * completion would hide an error from the filesystem. */
static int command(unsigned target, const uint8_t cdb[VSCSI_CDB_SIZE],
                   void *buf, uint32_t bytes, int write)
{
    if (!ready || scsi.failed || (bytes && !buf) || bytes > DMA_MAX_MAPPING) return -1;
    struct vscsi_cmd *req = control->cpu;
    struct vscsi_resp *resp = (void *)((uint8_t *)control->cpu + VSCSI_RESPONSE_OFF);
    memset(req, 0, sizeof *req);
    memset(resp, 0, sizeof *resp);
    resp->response = 0xff;
    req->lun[0] = 1; req->lun[1] = (uint8_t)target;
    req->lun[2] = 0x40; /* flat-space LUN 0, SAM single-level address */
    req->tag = ++next_tag;
    memcpy(req->cdb, cdb, VSCSI_CDB_SIZE);
    uint64_t token = 0;
    if (bytes) {
        payload = dma_map_kernel(&scsi.dma, buf, bytes,
                                write ? DMA_TO_DEVICE : DMA_FROM_DEVICE,
                                DMA_MAP_CONTIGUOUS);
        if (!payload) return -1;
        token = dma_mapping_submit(payload);
        if (!token) { dma_unmap(payload); payload = NULL; return -1; }
    }
    /* Virtio requires all readable descriptors before writable descriptors.
     * SCSI's response is before datain but AFTER dataout: using virtio-blk's
     * header/payload/status ordering for reads corrupts the returned payload. */
    struct virtio_buf b[3];
    int n = 0;
    b[n++] = (struct virtio_buf){control->dma, sizeof *req, 0, control};
    if (bytes && write)
        b[n++] = (struct virtio_buf){dma_mapping_addr(payload, 0), bytes, 0, NULL};
    b[n++] = (struct virtio_buf){dma_addr_add(control->dma, VSCSI_RESPONSE_OFF), sizeof *resp, 1, NULL};
    if (bytes && !write)
        b[n++] = (struct virtio_buf){dma_mapping_addr(payload, 0), bytes, 1, NULL};
    int used = virtio_request(&scsi, &queues[VSCSI_REQ_QUEUE], VSCSI_REQ_QUEUE, b, n);
    if (used < 0 && !scsi.quiesced) {
        if (!scsi.failed) virtio_stop(&scsi);
        if (!scsi.quiesced) panic("virtio-scsi: DMA reset failed; caller payload still owned");
    }
    int result = -1;
    uint32_t transferred = 0;
    if (used >= (int)sizeof *resp && resp->sense_len <= VSCSI_SENSE_SIZE && resp->residual <= bytes) {
        transferred = bytes - resp->residual;
        if (resp->response == 3) result = -2;
        else if (!resp->response && !resp->status &&
                 (write || (uint64_t)used >= sizeof *resp + transferred)) result = (int)transferred;
        else if (!resp->response && resp->status == 2 && resp->sense_len >= 3) {
            unsigned format = resp->sense[0] & 0x7f;
            unsigned key = (format == 0x72 || format == 0x73) ? resp->sense[1] & 15 : resp->sense[2] & 15;
            if (key == 6) result = -3;
        }
    }
    if (result == -1)
        kprintf("[virtio-scsi] target=%u cmd=%x failed: used=%d response=%u status=%u residual=%u sense=%x/%x/%x\n",
                target, (unsigned)cdb[0], used, (unsigned)resp->response, (unsigned)resp->status,
                resp->residual, (unsigned)resp->sense[2], (unsigned)resp->sense[12], (unsigned)resp->sense[13]);
    if (payload) {
        if (used >= 0 && dma_mapping_complete(payload, token, result >= 0 ? transferred : 0)) result = -1;
        if (dma_unmap(payload)) panic("virtio-scsi: attempted release of owned DMA payload");
        payload = NULL;
    }
    return result;
}

static int discover_command(unsigned target, uint8_t cdb[VSCSI_CDB_SIZE], void *buf, uint32_t bytes)
{
    int rc = -1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        rc = command(target, cdb, buf, bytes, 0);
        if (rc != -3) break;
    }
    return rc;
}

static int scsi_rw(void *ctx, uint64_t lba, uint32_t count, void *buf, int write)
{
    IO_GUARD(&scsi_gate);
    struct vscsi_disk *disk = ctx;
    if (!ready || scsi.failed || !buf || !count || lba >= disk->sectors || count > disk->sectors - lba) return -1;
    while (count) {
        uint32_t chunk = count > max_sectors ? max_sectors : count;
        uint8_t cdb[VSCSI_CDB_SIZE] = {0};
        cdb[0] = write ? 0x8a : 0x88; /* WRITE(16) / READ(16), 64-bit LBA */
        put_be64(cdb + 2, lba); put_be32(cdb + 10, chunk);
        uint32_t bytes = chunk * BLK_SECTOR;
        if (command(disk->target, cdb, buf, bytes, write) != (int)bytes) return -1;
        lba += chunk; count -= chunk; buf = (uint8_t *)buf + bytes;
    }
    return 0;
}
static int scsi_read(void *c, uint64_t l, uint32_t n, void *b)
{ return scsi_rw(c, l, n, b, 0); }
static int scsi_write(void *c, uint64_t l, uint32_t n, const void *b)
{ return scsi_rw(c, l, n, (void *)b, 1); }
static int scsi_flush(void *ctx)
{
    IO_GUARD(&scsi_gate);
    struct vscsi_disk *disk = ctx;
    uint8_t cdb[VSCSI_CDB_SIZE] = {0x35}; /* SYNCHRONIZE CACHE(10), IMMED=0 */
    /* A GOOD completion is the barrier; neither transport success alone nor
     * an unsupported opcode counts. Zero LBA/count flushes the whole medium,
     * including LBAs above 32 bits. CACHE(10) is broadly implemented; QEMU
     * scsi-hd rejected CACHE(16) with ILLEGAL REQUEST / invalid opcode (5/20/0)
     * during the first persistence test, and that test caught failed fsync. */
    return command(disk->target, cdb, NULL, 0, 0) == 0 ? 0 : -1;
}
static const struct blk_ops scsi_ops = {
    .read = scsi_read, .write = scsi_write, .flush = scsi_flush,
};

int virtio_scsi_init(void)
{
    IO_GUARD(&scsi_gate);
    if (initialized) return ready && !scsi.failed ? disk_count : -1;
    initialized = 1;
    /* The same modern capabilities can be exposed by a transitional PCI ID.
     * Choose by presence before init: a failed reset must not be overwritten by
     * a second initialization that loses its quarantined DMA ownership. */
    uint16_t id = dev_find_id(VIRTIO_VENDOR, VSCSI_MODERN, NULL) ? VSCSI_MODERN : VSCSI_TRANSITIONAL;
    if (virtio_init(id, &scsi, 0)) return -1;
    if (!scsi.device) { scsi_release(); return -1; }
    volatile uint32_t *cfg = (volatile uint32_t *)(void *)scsi.device;
    if (!cfg[0] || !cfg[1] || cfg[5] != VSCSI_SENSE_SIZE || cfg[6] != VSCSI_CDB_SIZE) {
        kprintf("[virtio-scsi] unsupported configuration\n"); scsi_release(); return -1;
    }
    max_sectors = DMA_MAX_MAPPING / BLK_SECTOR;
    if (cfg[2] && cfg[2] < max_sectors) max_sectors = cfg[2];
    for (int i = 0; i < 3; ++i)
        if (virtio_queue_setup(&scsi, i, &queues[i])) { scsi_release(); return -1; }
    control = dma_alloc_coherent(&scsi.dma, 4096, 4096, 0);
    if (!control) { scsi_release(); return -1; }
    virtio_driver_ok(&scsi); ready = 1;
    unsigned max_target = *(volatile uint16_t *)(scsi.device + 30);
    if (max_target >= VSCSI_TARGETS) max_target = VSCSI_TARGETS - 1;
    for (unsigned target = 0; target <= max_target; ++target) {
        uint8_t cdb[VSCSI_CDB_SIZE] = {0x12, 0, 0, 0, 36};
        uint8_t inquiry[36] = {0};
        int rc = discover_command(target, cdb, inquiry, sizeof inquiry);
        if (rc < 0) continue;
        if (rc < 36 || (inquiry[0] & 0xe0) || (inquiry[0] & 31)) {
            kprintf("[virtio-scsi] target=%u lun=0 rejected: not a direct-access disk\n", target); continue;
        }
        uint8_t capacity[32] = {0};
        memset(cdb, 0, sizeof cdb); cdb[0] = 0x9e; cdb[1] = 0x10;
        put_be32(cdb + 10, sizeof capacity); /* READ CAPACITY(16) */
        rc = discover_command(target, cdb, capacity, sizeof capacity);
        if (rc < 12) { kprintf("[virtio-scsi] target=%u capacity failed\n", target); continue; }
        uint64_t last = be64(capacity);
        uint32_t sector_size = be32(capacity + 8);
        if (sector_size != BLK_SECTOR || last == UINT64_MAX) {
            kprintf("[virtio-scsi] target=%u rejected: logical-sector=%u\n", target, sector_size); continue;
        }
        struct vscsi_disk *disk = &disks[target];
        disk->target = target; disk->sectors = last + 1;
        char name[] = "vscsi0"; name[5] = (char)('0' + target);
        disk->blk = blk_register(name, &scsi_ops, disk, disk->sectors);
        if (!disk->blk) break;
        ++disk_count;
        char model[17]; memcpy(model, inquiry + 16, 16); model[16] = 0;
        kprintf("[virtio-scsi] %s target=%u lun=0 sectors=%llu logical-sector=512 model='%s'\n",
                name, target, (unsigned long long)disk->sectors, model);
    }
    kprintf("[virtio-scsi] ready disks=%d request-queue=2 max-sectors=%u\n", disk_count, max_sectors);
    dma_report("virtio-scsi");
    return disk_count;
}

static int scsi_probe_existing(struct device *dev)
{ return dev == scsi.dev && ready && !scsi.failed ? 0 : -1; }
static void scsi_remove(struct device *dev)
{
    if (dev != scsi.dev) return;
    /* Keep lock order medium -> controller, as in regular I/O. Holding the
     * controller gate before draining media deadlocks a submitter owning a
     * medium gate that is waiting for this controller. */
    for (int i = 0; i < VSCSI_TARGETS; ++i)
        if (disks[i].blk) blk_dev_offline(disks[i].blk);
    IO_GUARD(&scsi_gate);
    scsi_release();
}
static const struct dev_match scsi_ids[] = {
    DEV_MATCH_VD(VIRTIO_VENDOR, VSCSI_TRANSITIONAL),
    DEV_MATCH_VD(VIRTIO_VENDOR, VSCSI_MODERN), DEV_MATCH_END
};
static struct driver scsi_driver = {
    .name = "virtio-scsi", .bus_type = DEV_BUS_PCI, .match = scsi_ids,
    .probe = scsi_probe_existing, .remove = scsi_remove,
};
DRIVER_DECLARE(scsi_driver);
