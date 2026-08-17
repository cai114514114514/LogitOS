/* virtio-rng (virtio device ID 4, "entropy source") -- the simplest virtio
 * device there is: one virtqueue, no device-specific config space, no
 * feature bits (virtio-v1.1 spec sec 5.4 "Entropy Device": 5.4.2 names the
 * single queue "requestq" = index 0; 5.4.3 "Feature bits: None"; 5.4.5
 * "Device Operation": the driver places device-writable buffers on the
 * queue and the device fills them, in order, with random data -- possibly
 * only PARTIALLY filling a buffer per completion, which is why
 * virtio_rng_get() below loops on a short return).
 *
 * PCI Device ID 0x1005 is the transitional ID QEMU's `virtio-rng-pci`
 * answers to by default (legacy BAR0 + the modern vendor capabilities this
 * driver actually uses side by side) -- the same numbering family already in
 * this tree for the other two virtio devices: VIRTIO_DEV_BLK=0x1001 and
 * VIRTIO_DEV_GPU=0x1050 (virtio.h). BLK and RNG are both pre-1.0 "transitional"
 * IDs (spec sec 4.1.2, Table 4.1); GPU is modern-only and follows the newer
 * 0x1040+device-id rule (0x1040+16). Defined locally rather than added to the
 * shared virtio.h: it is used nowhere but this file's own virtio_init() call.
 *
 * THE HONEST REASON THIS DRIVER EXISTS. rng.c's kernel_random_bytes() seeds
 * its Hash_DRBG from RDSEED/RDRAND when the CPU offers them and falls back to
 * rdtsc+PIT ticks -- calling that fallback "weak" out loud, in a kprintf
 * WARNING, and rng_strong() (which gates whether TLS will even attempt a
 * handshake) is defined as "RDSEED or RDRAND available", nothing else. That
 * fallback is not a hypothetical: this project's own Makefile documents
 * hitting it --
 *
 *     "The default TCG cpu (qemu64) has no RDRAND/RDSEED, and the kernel TLS
 *      client refuses to handshake on the weak rdtsc-only RNG fallback --
 *      expose the hardware-RNG flags so HTTPS works. Overridable: `make run
 *      QEMU_CPU="-cpu qemu64"`."      (Makefile:1182-1185)
 *
 * -- i.e. the project's own default (`-cpu max`) is a WORKAROUND for a gap
 * this driver closes properly: any machine whose CPU model doesn't carry
 * RDRAND/RDSEED (an explicit `-cpu qemu64` override, an older `-cpu` model,
 * or -- the case QEMU_CPU exists to work around in the first place -- a real
 * hypervisor that masks the instruction from guests) currently has NO
 * hardware entropy at all. virtio-rng is exactly the standard answer: entropy
 * from the host, independent of what the virtual CPU claims to support.
 *
 * WHAT THIS FILE DOES NOT DO: wire itself into rng.c. rng.c is not owned by
 * this change, so rng_gather()/rng_strong() are untouched here -- see the
 * REQUEST in the accompanying report for the exact hook. This file proves the
 * device end works (probe, bring-up, and a two-independent-reads self-test on
 * the boot serial log, since there is no syscall path into this driver yet)
 * so that hook is a small, low-risk addition once made. */
#include <stdint.h>
#include <stddef.h>
#include "driver.h"
#include "virtio.h"
#include "virtio_rng.h"
#include "kprintf.h"

/* Transitional PCI Device ID for virtio-rng -- see the file header. */
#define VIRTIO_DEV_RNG 0x1005

static struct virtio_dev rngdev;
static struct virtq      rngvq;
static int                rng_ready;

int virtio_rng_present(void) { return rng_ready; }

/* One request buffer at a time, capped so it fits the identity-mapped static
 * staging buffer below (same shape as virtio_blk.c's static request header:
 * "static: identity-mapped, single outstanding"). virtio_request() is
 * synchronous, so there is never a second request in flight to alias it. */
#define RNG_STAGE 64

int virtio_rng_get(void *buf, int len)
{
    if (!rng_ready || len <= 0) return -1;
    static uint8_t stage[RNG_STAGE];
    uint8_t *out = (uint8_t *)buf;
    int done = 0;
    while (done < len) {
        int want = len - done;
        if (want > RNG_STAGE) want = RNG_STAGE;
        struct virtio_buf b = { (uint64_t)(uintptr_t)stage, (uint32_t)want, 1 };
        int rc = virtio_request(&rngdev, &rngvq, 0, &b, 1);
        if (rc < 0) return done > 0 ? done : -1;    /* timeout: keep partial progress */
        if (rc > want) rc = want;                    /* never trust the device past our own buffer */
        for (int i = 0; i < rc; i++) out[done + i] = stage[i];
        done += rc;
        if (rc == 0) break;   /* a completion that filled nothing: stop rather than spin */
    }
    return done;
}

/* Boot self-test: pull two INDEPENDENT reads and print both, so a boot
 * harness can assert real entropy is flowing without a syscall path into
 * this driver (none exists yet -- rng.c is not wired to this file, see the
 * header comment). A stub that "succeeds" without doing the work -- the
 * thing this tree refuses everywhere else -- would show up here as two
 * equal or two all-zero reads, which is exactly what the assertions below
 * check for.
 *
 * THIS PRINTS 32 BYTES OF THE ENTROPY STREAM TO THE SERIAL CONSOLE, AND THE
 * HOOK THIS FILE ASKS FOR IS WHAT MAKES THAT A LEAK. Today nothing consumes
 * virtio_rng_get(), so those bytes feed nothing and naming them costs
 * nothing. The moment rng.c calls in -- which the header requests -- the
 * first 32 bytes the DRBG would have been seeded from are sitting in a boot
 * log that every harness in tests/boot captures to a file. Whoever writes
 * that hook removes this function or puts it behind a build flag; it is not
 * a thing to leave for later, because a seed printed once is printed
 * forever. Two reads and a difference is all the assertion needs, so a fold
 * (say, a per-read XOR checksum) would carry the same proof with none of
 * this -- deliberately not done here, because it would silently change what
 * the accompanying harness greps for. */
static void rng_selftest(const char *devname)
{
    uint8_t a[16], b[16];
    int na = virtio_rng_get(a, sizeof a);
    int nb = virtio_rng_get(b, sizeof b);
    if (na != (int)sizeof a || nb != (int)sizeof b) {
        kprintf("VIRTIO_RNG_SELFTEST FAIL %s short-read na=%d nb=%d\n", devname, na, nb);
        return;
    }
    int zero_a = 1, zero_b = 1, same = 1;
    for (int i = 0; i < 16; i++) {
        if (a[i]) zero_a = 0;
        if (b[i]) zero_b = 0;
        if (a[i] != b[i]) same = 0;
    }
    static const char hexd[] = "0123456789abcdef";
    char hexa[33], hexb[33];
    for (int i = 0; i < 16; i++) {
        hexa[2*i] = hexd[a[i] >> 4]; hexa[2*i + 1] = hexd[a[i] & 0xF];
        hexb[2*i] = hexd[b[i] >> 4]; hexb[2*i + 1] = hexd[b[i] & 0xF];
    }
    hexa[32] = 0; hexb[32] = 0;
    kprintf("VIRTIO_RNG_SELFTEST %s a=%s b=%s zero_a=%d zero_b=%d same=%d\n",
            devname, hexa, hexb, zero_a, zero_b, same);
}

static int rng_probe(struct device *dev)
{
    if (virtio_init(VIRTIO_DEV_RNG, &rngdev, 0) != 0) return -1;   /* sec 5.4.3: no feature bits to request */
    if (virtio_queue_setup(&rngdev, 0, &rngvq) != 0) return -1;    /* queue 0 = "requestq" (sec 5.4.2) */
    virtio_driver_ok(&rngdev);
    rng_ready = 1;
    kprintf("[virtio-rng] %s: ready\n", dev->name);
    rng_selftest(dev->name);
    return 0;
}

static const struct dev_match rng_ids[] = {
    DEV_MATCH_VD(VIRTIO_VENDOR, VIRTIO_DEV_RNG),
    DEV_MATCH_END
};

static struct driver rng_driver = {
    .name     = "virtio-rng",
    .bus_type = DEV_BUS_PCI,
    .match    = rng_ids,
    .probe    = rng_probe,
    .remove   = NULL,
    .next     = NULL,
};
DRIVER_DECLARE(rng_driver);
