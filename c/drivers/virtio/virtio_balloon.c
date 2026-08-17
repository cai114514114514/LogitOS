/* virtio-balloon (virtio device ID 5, "Memory Balloon Device"; virtio-v1.1
 * spec sec 5.5) -- the host asking the guest to hand physical frames back.
 *
 * PCI Device ID 0x1002 is the transitional ID QEMU's `virtio-balloon-pci`
 * answers to by default (legacy BAR0 + the modern vendor capabilities this
 * driver actually uses, side by side -- same shape as the other two
 * transitional devices already in this tree). It is not derived from the
 * virtio device ID (5) by the "0x1000 + id" rule that would suggest 0x1005 --
 * that number is already taken here by virtio-rng (virtio_rng.c). The
 * transitional PCI IDs were assigned individually early in virtio's history
 * and only look formulaic in places: 0x1000 network, 0x1001 block
 * (VIRTIO_DEV_BLK in virtio.h), 0x1002 balloon, 0x1003 console, 0x1004 SCSI
 * host, 0x1005 entropy source (VIRTIO_DEV_RNG in virtio_rng.c). Defined
 * locally rather than added to virtio.h: used nowhere but this file's own
 * virtio_init() call, same reasoning virtio_rng.c gives for VIRTIO_DEV_RNG.
 *
 * THE HONEST REASON THIS DRIVER EXISTS. This tree has a full reclaim + swap
 * subsystem (c/kernel/mm/{rmap,reclaim,swap}.c) whose own header says
 * plainly that pressure has to be MANUFACTURED to test any of it -- the full
 * desktop peaks at 229 MiB of 511, so `test-swap` boots the SAME kernel with
 * a smaller `-m` and runs a workload that maps more than exists. That is
 * pressure from INSIDE the guest, fixed at boot. A balloon is pressure from
 * OUTSIDE, and it can be applied to an already-running machine (`balloon N`
 * over QMP) rather than only by choosing a smaller machine before it boots --
 * the thing reclaim/swap's own test harness names as the gap.
 *
 * WHAT THIS FILE DOES NOT DO: get itself polled once the desktop is running,
 * or wire into anything that would let a QMP `balloon` call issued AFTER boot
 * change this driver's behaviour. See virtio_balloon.h's header comment for
 * why (wm.c ownership) and what the one-line fix is. What IS proven here,
 * self-contained, on the boot serial log: (1) a self-directed inflate/deflate
 * round-trip that does not depend on anything the host does
 * (VIRTIO_BALLOON_SELFTEST), and (2) if the host has already set a nonzero
 * target in config space by the time this driver's probe() runs -- which a
 * `balloon N` sent over QMP immediately after the QEMU process starts
 * achieves, because QMP's monitor is live and the device model is fully
 * realised long before the guest's own PCI enumeration reaches this driver --
 * that target is honoured for real (VIRTIO_BALLOON_TARGET), and the frames
 * stay taken out of circulation, which is the actual point.
 *
 * WHERE THE FRAMES COME FROM, AND WHY THE RECLAIM INVARIANT IS UNTOUCHED.
 * pmm.h says a fresh pmm_alloc() frame has refcount 1 and is not mapped into
 * any address space -- which per rmap.h means it has NO rmap chain entry
 * ("most of what must never be evicted needs no pin at all: it is not mapped
 * into any user address space, so the reverse map has no entry for it").
 * This driver ONLY ever inflates with pmm_alloc()'d frames -- genuinely free
 * memory pulled straight from the allocator, never a frame taken away from an
 * existing mapping. It therefore never calls into reclaim.c and never has to
 * satisfy reclaim's own eviction rule (rmap_count(f) == pmm_refcount(f),
 * rmap.h) itself: that rule governs reclaim taking a frame FROM a mapping,
 * and there is no mapping here to take one from. The one invariant this
 * driver DOES have to keep is pmm's own: a frame handed to the host is never
 * pmm_free()'d while held (see balloon_inflate()'s comment) -- freeing it
 * would let a later pmm_alloc() hand the SAME physical page, which the host
 * may already be repurposing, to a second owner inside this kernel. */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "virtio.h"
#include "virtio_balloon.h"
#include "kprintf.h"
#include "pmm.h"

/* Transitional PCI Device ID for virtio-balloon -- see the file header. */
#define VIRTIO_DEV_BALLOON 0x1002

/* virtio-v1.1 sec 5.5.2: queue 0 = inflateq, queue 1 = deflateq. Queues 2
 * (statsq) and 3 (free_page_vq) only exist if their feature bits are
 * negotiated; this driver negotiates none (want_lo=0 below, same as
 * virtio_rng.c's "sec 5.4.3: no feature bits to request" -- balloon's own
 * optional bits are all extras this driver does not need). */
#define BAL_Q_INFLATE 0
#define BAL_Q_DEFLATE 1

/* virtio-v1.1 sec 5.5.4 "Device configuration layout": le32 num_pages at
 * offset 0, le32 actual at offset 4. (Two more le32 fields follow only under
 * feature bits this driver does not negotiate, so they are never read here.)
 * PCI transport fields are little-endian (virtio-v1.1 sec 4.1.3), and so is
 * this CPU, so no byteswap is needed -- same assumption virtio_gpu.c and
 * virtio_blk.c already make of their own device-config reads. */
#define CFG_NUM_PAGES 0
#define CFG_ACTUAL    4

/* virtio-v1.1 sec 5.5.6: the PFN array element is a Guest Physical Frame
 * Number, i.e. the physical address shifted right by VIRTIO_BALLOON_PFN_SHIFT
 * = 12 (4096-byte pages, matching FRAME_SIZE in pmm.h). */
#define PFN_SHIFT 12

static struct virtio_dev bdev;
static struct virtq      inflate_vq, deflate_vq;
static int                bal_ready;

/* Physical addresses of every frame currently donated to the host, so
 * deflate can hand back exactly what was taken (virtio-v1.1 sec 5.5.6.3,
 * "the Driver MUST NOT ask to remove memory it did not previously add" --
 * honoured here unconditionally, not only under VIRTIO_BALLOON_F_MUST_TELL_
 * HOST, which this driver does not negotiate either way). Bounded static
 * array (no malloc in this kernel): 65536 entries * 8 bytes = 512 KiB of
 * .bss, capping the balloon at 256 MiB -- ample against a 512 MiB machine
 * (the desktop's own measured peak is 229 MiB, per reclaim.h) and small
 * enough not to be a meaningful image-size cost next to the CJK font's own
 * 2.2 MB. */
#define MAX_BALLOON_FRAMES 65536
static uint64_t held[MAX_BALLOON_FRAMES];
static uint32_t held_n;

/* One page of PFNs per virtio_request -- 1024 * 4 bytes = FRAME_SIZE, so it
 * is exactly one pmm_alloc()'d-shaped buffer, identity-mapped like every
 * other virtio driver's static staging buffer in this tree (virtio_rng.c's
 * "stage" comment: "identity-mapped, single outstanding"; virtio_request()
 * is synchronous so there is never a second request in flight to alias it).
 * One request this size moves up to 4 MiB. */
#define STAGE_PFNS 1024
static uint32_t stage[STAGE_PFNS];

static inline uint32_t cfg_r32(int off) { return *(volatile uint32_t *)(bdev.device + off); }
static inline void     cfg_w32(int off, uint32_t v) { *(volatile uint32_t *)(bdev.device + off) = v; }

/* Take `want` frames out of circulation (or fewer -- see the return value).
 * Batches through the STAGE_PFNS staging buffer; each batch is a single
 * virtio_request() on the inflateq. A batch that pmm cannot fully fill (pmm
 * genuinely out of free frames) or that the device does not complete (a
 * virtio_request() timeout) stops the whole call rather than retrying
 * forever -- this tree's "nothing stubbed to success" rule: a caller asking
 * for 1000 and getting 400 back must be told 400, not made to believe 1000
 * happened. */
static uint32_t balloon_inflate(uint32_t want)
{
    uint32_t done = 0;
    while (done < want && held_n < MAX_BALLOON_FRAMES) {
        uint32_t batch = want - done;
        uint32_t cap = MAX_BALLOON_FRAMES - held_n;
        if (batch > cap) batch = cap;
        if (batch > STAGE_PFNS) batch = STAGE_PFNS;

        uint32_t got = 0;
        for (; got < batch; got++) {
            uint64_t f = pmm_alloc();
            if (!f) break;                      /* pmm is out of free frames */
            held[held_n + got] = f;
            stage[got] = (uint32_t)(f >> PFN_SHIFT);
        }
        if (got == 0) break;

        struct virtio_buf b = { (uint64_t)(uintptr_t)stage, got * 4u, 0 };  /* driver-writable, device-readable */
        int rc = virtio_request(&bdev, &inflate_vq, BAL_Q_INFLATE, &b, 1);
        if (rc < 0) {
            kprintf("[virtio-balloon] inflate request timed out, giving %u frames back\n", got);
            for (uint32_t i = 0; i < got; i++) pmm_free(held[held_n + i]);
            break;
        }
        held_n += got;
        done += got;
        if (got < batch) break;   /* pmm ran dry; do not spin asking pmm_alloc for more */
    }
    return done;
}

/* Give back up to `want` frames the device previously took. Tells the device
 * FIRST and only pmm_free()s the frames the device has acknowledged
 * (virtio-v1.1 sec 5.5.6.3's MUST_TELL_HOST discipline, honoured
 * unconditionally -- see the file header). Returns frames actually
 * returned. */
static uint32_t balloon_deflate(uint32_t want)
{
    uint32_t done = 0;
    while (done < want && held_n > 0) {
        uint32_t batch = want - done;
        if (batch > held_n) batch = held_n;
        if (batch > STAGE_PFNS) batch = STAGE_PFNS;
        uint32_t base = held_n - batch;
        for (uint32_t i = 0; i < batch; i++) stage[i] = (uint32_t)(held[base + i] >> PFN_SHIFT);

        struct virtio_buf b = { (uint64_t)(uintptr_t)stage, batch * 4u, 0 };
        int rc = virtio_request(&bdev, &deflate_vq, BAL_Q_DEFLATE, &b, 1);
        if (rc < 0) {
            kprintf("[virtio-balloon] deflate request timed out after %u of %u frames\n", done, want);
            break;
        }
        for (uint32_t i = 0; i < batch; i++) pmm_free(held[base + i]);
        held_n -= batch;
        done += batch;
    }
    return done;
}

int      virtio_balloon_present(void)      { return bal_ready; }
uint32_t virtio_balloon_held_pages(void)   { return held_n; }
uint32_t virtio_balloon_target_pages(void) { return bal_ready ? cfg_r32(CFG_NUM_PAGES) : 0; }

int virtio_balloon_poll(void)
{
    if (!bal_ready) return 0;
    uint32_t target = cfg_r32(CFG_NUM_PAGES);
    if (target > held_n) {
        int moved = (int)balloon_inflate(target - held_n);
        cfg_w32(CFG_ACTUAL, held_n);
        return moved;
    }
    if (target < held_n) {
        int moved = (int)balloon_deflate(held_n - target);
        cfg_w32(CFG_ACTUAL, held_n);
        return -moved;
    }
    return 0;
}

/* Boot self-test, independent of anything the host does: inflate a handful of
 * frames, deflate the same ones straight back, and print pmm's own free-frame
 * count at all three points. A stub that "succeeds" without doing the real
 * work would show up here as free_after_inflate == free_before (nothing
 * actually left the pool) -- exactly the shape the RNG driver's own
 * self-test comment names as the thing to check for. Small (4 frames) and
 * fast: this runs unconditionally at every boot with the device present,
 * from dev_probe_all() (c/kernel/core/kmain.c), which runs after smp_init()
 * -- the AP cores are up by then -- but before wm_run() starts the scheduler
 * proper and spawns the first app thread. Nothing else is runnable yet to
 * contend for pmm_lock, so the before/after free-frame arithmetic cannot be
 * made flaky by concurrent allocation elsewhere. */
#define SELFTEST_N 4

static void balloon_selftest(const char *devname)
{
    uint64_t f0 = pmm_free_frames();
    uint32_t got = balloon_inflate(SELFTEST_N);
    uint64_t f1 = pmm_free_frames();
    uint32_t back = balloon_deflate(got);
    uint64_t f2 = pmm_free_frames();
    kprintf("VIRTIO_BALLOON_SELFTEST %s inflated=%u deflated=%u "
            "free_before=%u free_after_inflate=%u free_after_deflate=%u\n",
            devname, got, back, (unsigned)f0, (unsigned)f1, (unsigned)f2);
}

static int balloon_probe(struct device *dev)
{
    if (virtio_init(VIRTIO_DEV_BALLOON, &bdev, 0) != 0) return -1;   /* sec 5.5.3: no feature bits requested */
    if (!bdev.device) {
        kprintf("[virtio-balloon] %s: no device-config capability, refusing\n", dev->name);
        return -1;
    }
    if (virtio_queue_setup(&bdev, BAL_Q_INFLATE, &inflate_vq) != 0) return -1;
    if (virtio_queue_setup(&bdev, BAL_Q_DEFLATE, &deflate_vq) != 0) return -1;
    virtio_driver_ok(&bdev);
    bal_ready = 1;
    kprintf("[virtio-balloon] %s: ready\n", dev->name);

    balloon_selftest(dev->name);

    /* Honour whatever target the host has already latched into config space.
     * A `balloon N` sent over QMP right after the QEMU process starts lands
     * here, because the device model is fully realised (and answers QMP)
     * long before the guest's own boot reaches PCI enumeration -- see the
     * file header. */
    uint32_t target = cfg_r32(CFG_NUM_PAGES);
    if (target > 0) {
        uint64_t before = pmm_free_frames();
        uint32_t got = balloon_inflate(target);
        cfg_w32(CFG_ACTUAL, held_n);
        uint64_t after = pmm_free_frames();
        kprintf("VIRTIO_BALLOON_TARGET %s target=%u held=%u free_before=%u free_after=%u\n",
                dev->name, target, held_n, (unsigned)before, (unsigned)after);
        if (got < target) {
            kprintf("[virtio-balloon] %s: WARNING only %u of %u requested pages honoured "
                    "(pmm out of free frames, or MAX_BALLOON_FRAMES=%u cap)\n",
                    dev->name, got, target, (unsigned)MAX_BALLOON_FRAMES);
        }
    }
    return 0;
}

static const struct dev_match balloon_ids[] = {
    DEV_MATCH_VD(VIRTIO_VENDOR, VIRTIO_DEV_BALLOON),
    DEV_MATCH_END
};

static struct driver balloon_driver = {
    .name     = "virtio-balloon",
    .bus_type = DEV_BUS_PCI,
    .match    = balloon_ids,
    .probe    = balloon_probe,
    .remove   = NULL,
    .next     = NULL,
};
DRIVER_DECLARE(balloon_driver);
