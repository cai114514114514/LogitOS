/* Fail-closed GP107 acceleration bring-up.
 *
 * This is intentionally the boundary before the first GPU command.  The
 * NVIDIA open kernel modules cannot be transplanted here: NVIDIA's own support
 * statement says their GSP architecture begins at Turing.  Pascal is instead
 * described by Nouveau.  Linux commit
 * 587858367581b9c55c3690f4e63382ad622719d4 names GP107's gp100 MMU/FIFO/CE,
 * gp102 ACR/SEC2 and gp107 GR stack.  Its channel path additionally allocates
 * GPU page tables, instance memory, RAMFC/USERD, a runlist, push ring and a
 * semaphore before exposing PASCAL_DMA_COPY_A/B.  None can be skipped merely
 * because firmware left a visible GOP framebuffer.
 *
 * The manifest below pins the complete redistributable prerequisite bundle at
 * linux-firmware commit 1522c78ab870b3c051d8a3a1d24ecbbc12b23be5:
 * ACR load/unload (4), both supported SEC2 ABI alternatives (3 + 3), and the
 * twelve GP107 GR files.  A runtime will later select one SEC2 ABI; checking
 * both here proves that the installed bundle matches the pinned release rather
 * than silently mixing versions.  Files shared through upstream symlinks use
 * their canonical gp102 paths because LogitOS does not need symlink semantics
 * to measure them.
 *
 * Ordering is the safety property: exact PCI function and BAR0 are checked,
 * then every byte bundle is size/SHA-256 verified, then and only then BAR0 is
 * mapped and three identity/status registers are READ.  There is no MMIO write,
 * PCI config write, BME, IRQ, firmware upload or engine reset in this stage.
 * BOOT0 must report chipset 0x137 (GP107).  We then stop at CHANNEL_STACK and
 * retain the CPU framebuffer.  Calling this acceleration before an off-screen
 * CE canary, fence/readback and fault quarantine would turn an untested register
 * poke into a product claim, so fill/copy remain explicit refusals.
 */

#include <stddef.h>
#include <stdint.h>
#include "nvidia_pascal_accel.h"
#include "nvidia_pascal.h"
#include "driver.h"
#include "vfs.h"
#include "kheap.h"
#include "crypto.h"
#include "kprintf.h"

#define NV_PMC_BOOT_0   0x000000u
#define NV_PMC_BOOT_1   0x000004u
#define NV_PMC_ENABLE   0x000200u
#define NV_PRI_MIN_SIZE 0x00100000ull

static const struct nv1050_fw_manifest_entry firmware_manifest[] = {
    { "nvidia/gp102/acr/bl.bin", 1280, "01326cc997716bebb0eab9fe1f463fffa0ee586079e4ca0e5bb096ab6fcfedab" },
    { "nvidia/gp102/acr/ucode_load.bin", 17152, "5d2ce92a781c6c42a781a50badfbfc7db9267945996ee46aaa15429facdf2920" },
    { "nvidia/gp102/acr/unload_bl.bin", 1280, "dcee20bfa564da6a8fadc21dd9e0087d45f630b8b18f8e296954a3b8f0e28173" },
    { "nvidia/gp102/acr/ucode_unload.bin", 3328, "1f139e5ada193fc191ed9a235a53901076d0008f3ea15ab7ca66d33748ea6a03" },
    { "nvidia/gp102/sec2/desc.bin", 656, "33116677910e9cc9ea047e420a40bd308dd6597503126feecd9827e056668a11" },
    { "nvidia/gp102/sec2/image.bin", 99072, "9ca3293e0d11dcf08570ebe226f4d9889128238f8c5d5b5e3ea7999bb4880e3d" },
    { "nvidia/gp102/sec2/sig.bin", 192, "6611a8ff65b34aadfab9db2c072205df30174c18d62cb2acb1a6ad7ac5eb1dd9" },
    { "nvidia/gp102/sec2/desc-1.bin", 656, "0d0ce7988281fc9115d3c6320e7b904d530719ddeacdd85ea29f744ba7b039ee" },
    { "nvidia/gp102/sec2/image-1.bin", 109568, "90404a9c365eb9b2d3f692c255bc3b50af9313ccae73306df369ec712c4b511e" },
    { "nvidia/gp102/sec2/sig-1.bin", 192, "14c3524427b08a6dca1fc846f3830387ee835c3257792860783e5e4148833ebb" },
    { "nvidia/gp107/gr/fecs_bl.bin", 576, "e683be6f7b2012f4e5e6f4995086a5f0ef5bc4fd687f24dcedc5df7070f3a43d" },
    { "nvidia/gp107/gr/fecs_inst.bin", 22879, "e68760571d3da8734485e819d79202f7ead21797ee8b4b67eb579aab1489f02a" },
    { "nvidia/gp107/gr/fecs_data.bin", 2756, "e61962ec88d1cc0c38f2397c6a5e8f3521d9299472e586700011fe621ee85999" },
    { "nvidia/gp107/gr/fecs_sig.bin", 192, "6682d9eb17e81108484dc84c73f1e765ea7d8607ca3a96834ec1682f8e014856" },
    { "nvidia/gp107/gr/gpccs_bl.bin", 576, "302d719d56b68b5641d659b05a84dde870072c31ecf931491f5303ebce1d62c5" },
    { "nvidia/gp107/gr/gpccs_inst.bin", 12587, "1ef4cc6d4798577850102dcddc300fe905e091dc7ddc9d6289360f6ef1291f76" },
    { "nvidia/gp107/gr/gpccs_data.bin", 2100, "bc16310ec9c2128e7c8c51e23152b29f0f1ee7088494f11cf852e98afe19992c" },
    { "nvidia/gp107/gr/gpccs_sig.bin", 192, "7eab77572dce601d91055d3efe5e8d4bfc1350367af324204bfd76282eebd9ef" },
    { "nvidia/gp107/gr/sw_ctx.bin", 6000, "c60f4e9ea298af2ebfb466accacae95e4a1baa7138971b280a97c4131ff29121" },
    { "nvidia/gp107/gr/sw_nonctx.bin", 2496, "4890912b427bde77e4998a601ef8b282eae1ae5ce4dc20e5de2d7b50917f4ac0" },
    { "nvidia/gp102/gr/sw_bundle_init.bin", 7680, "5f543872e72731f164e9e9f1fc19fc30e73a508629d2c6d0724fe6247c976c50" },
    { "nvidia/gp102/gr/sw_method_init.bin", 12288, "1123176ec93df9046b3fb5071141235424c101e405294f505494e00ba925a719" },
};

_Static_assert(sizeof firmware_manifest / sizeof firmware_manifest[0] ==
               NV1050_FW_MANIFEST_COUNT, "GP107 firmware manifest count drifted");

static struct nv1050_accel_info active_info = {
    .stage = NV1050_ACCEL_OFF,
    .software_fallback = 1,
};

size_t nv1050_fw_manifest_count(void)
{
    return sizeof firmware_manifest / sizeof firmware_manifest[0];
}

const struct nv1050_fw_manifest_entry *nv1050_fw_manifest_at(size_t index)
{
    return index < nv1050_fw_manifest_count() ? &firmware_manifest[index] : NULL;
}

static uint8_t hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xff;
}

static int digest_matches(const uint8_t digest[32], const char *hex)
{
    if (!digest || !hex) return 0;
    for (unsigned i = 0; i < 32; i++) {
        uint8_t hi = hex_nibble(hex[i * 2]);
        uint8_t lo = hex_nibble(hex[i * 2 + 1]);
        if (hi == 0xff || lo == 0xff || digest[i] != (uint8_t)((hi << 4) | lo))
            return 0;
    }
    return hex[64] == '\0';
}

static void info_init(struct nv1050_accel_info *info, const struct device *dev)
{
    uint8_t *p = (uint8_t *)info;
    for (size_t i = 0; i < sizeof *info; i++) p[i] = 0;
    info->firmware_files = (uint32_t)nv1050_fw_manifest_count();
    info->pci_device = dev ? dev->device : 0;
    info->software_fallback = 1;
}

static int fail(struct nv1050_accel_info *info, enum nv1050_accel_blocker why)
{
    info->stage = NV1050_ACCEL_BLOCKED;
    info->blocker = why;
    info->software_fallback = 1;
    return -1;
}

static int device_and_bar0_sane(const struct device *dev)
{
    if (!dev || dev->bus_type != DEV_BUS_PCI || dev->vendor != 0x10de ||
        !nvidia_pascal_model_name(dev->device) ||
        dev->class_code != 0x03 ||
        (dev->subclass != 0x00 && dev->subclass != 0x02) ||
        dev->header_type != 0)
        return NV1050_BLOCK_WRONG_DEVICE;

    const struct dev_resource *bar0 = &dev->res[0];
    if (!(bar0->flags & DEV_RES_MEM) || !bar0->start ||
        bar0->size < NV_PRI_MIN_SIZE || bar0->start + bar0->size < bar0->start)
        return NV1050_BLOCK_BAD_BAR0;
    return NV1050_BLOCK_NONE;
}

int nv1050_accel_prepare_with_ops(struct device *dev,
                                  const struct nv1050_fw_reader *firmware,
                                  const struct nv1050_hw_reader *hardware,
                                  struct nv1050_accel_info *out)
{
    struct nv1050_accel_info local;
    info_init(&local, dev);
    if (!out) out = &local;
    else *out = local;

    int bad = device_and_bar0_sane(dev);
    if (bad) return fail(out, (enum nv1050_accel_blocker)bad);
    if (!firmware || !firmware->measure || !hardware ||
        !hardware->map_bar0 || !hardware->read32)
        return fail(out, NV1050_BLOCK_FW_IO);

    for (size_t i = 0; i < nv1050_fw_manifest_count(); i++) {
        const struct nv1050_fw_manifest_entry *entry = &firmware_manifest[i];
        uint32_t measured_size = 0;
        uint8_t digest[32];
        for (unsigned j = 0; j < sizeof digest; j++) digest[j] = 0;
        int rc = firmware->measure(firmware->ctx, entry, &measured_size, digest);
        if (rc != 0) {
#ifdef NV1050_NEGCTL_MAP_ON_MISSING
            /* Mutation gate: recreate the unsafe order in which an absent
             * firmware bundle still causes a BAR mapping. */
            if (rc == NV1050_BLOCK_FW_MISSING)
                (void)hardware->map_bar0(hardware->ctx, dev);
#endif
            enum nv1050_accel_blocker reason =
                (rc == NV1050_BLOCK_FW_MISSING || rc == NV1050_BLOCK_FW_SIZE ||
                 rc == NV1050_BLOCK_FW_HASH || rc == NV1050_BLOCK_FW_IO)
                ? (enum nv1050_accel_blocker)rc : NV1050_BLOCK_FW_IO;
            return fail(out, reason);
        }
        if (measured_size != entry->size) return fail(out, NV1050_BLOCK_FW_SIZE);
        if (!digest_matches(digest, entry->sha256_hex))
#ifndef NV1050_NEGCTL_ACCEPT_BAD_HASH
            return fail(out, NV1050_BLOCK_FW_HASH);
#else
            ; /* Mutation gate: mixed/corrupt bundle reaches hardware. */
#endif
        out->firmware_verified++;
    }
    out->stage = NV1050_ACCEL_FW_VERIFIED;

    /* Mapping happens after all 22 hashes.  The generic mapper creates a
     * writable CPU PTE, so the enforced guarantee is zero STORE instructions
     * through this pointer; hardware->read32 is the only downstream seam. */
    uint64_t pri = hardware->map_bar0(hardware->ctx, dev);
    if (!pri) return fail(out, NV1050_BLOCK_MAP_FAILED);
    out->stage = NV1050_ACCEL_PRI_MAPPED;

    out->boot0 = hardware->read32(hardware->ctx, pri, NV_PMC_BOOT_0);
    out->mmio_reads++;
    out->boot1 = hardware->read32(hardware->ctx, pri, NV_PMC_BOOT_1);
    out->mmio_reads++;
    out->pmc_enable = hardware->read32(hardware->ctx, pri, NV_PMC_ENABLE);
    out->mmio_reads++;
    out->chipset = (uint16_t)((out->boot0 & 0x1ff00000u) >> 20);
    out->chiprev = (uint8_t)out->boot0;
    if (out->boot0 == 0 || out->boot0 == UINT32_MAX || out->boot1 == UINT32_MAX ||
        out->chipset != 0x137)
        return fail(out, NV1050_BLOCK_WRONG_CHIPSET);

    out->stage = NV1050_ACCEL_IDENTIFIED;

    /* GP107's PRI identity is now known.  The next safe write requires the
     * complete gp100 MMU + instance-memory + PFIFO/PBDMA + RAMFC/USERD/runlist
     * + channel stack and an off-screen PASCAL_DMA_COPY_A/B canary.  The GOP
     * PCI address is not a GPU virtual address, and Nouveau rebuilds BAR1 from
     * GPU page tables; submitting it directly could corrupt unrelated VRAM. */
    return fail(out, NV1050_BLOCK_CHANNEL_STACK);
}

static int default_measure(void *ctx,
                           const struct nv1050_fw_manifest_entry *entry,
                           uint32_t *measured_size, uint8_t digest[32])
{
    (void)ctx;
    char path[160];
    const char *prefix = "/lib/firmware/";
    size_t n = 0;
    while (prefix[n] && n + 1 < sizeof path) { path[n] = prefix[n]; n++; }
    if (prefix[n]) return NV1050_BLOCK_FW_IO;
    size_t i = 0;
    for (; entry->path[i] && n + 1 < sizeof path; i++)
        path[n++] = entry->path[i];
    /* A truncated filename could accidentally measure a different file and is
     * therefore an I/O refusal, never an ordinary not-found result. */
    if (entry->path[i]) return NV1050_BLOCK_FW_IO;
    path[n] = '\0';

    int size = vfs_size(path);
    if (size < 0) return NV1050_BLOCK_FW_MISSING;
    *measured_size = (uint32_t)size;
    if ((uint32_t)size != entry->size) return NV1050_BLOCK_FW_SIZE;
    uint8_t *bytes = kmalloc(entry->size);
    if (!bytes) return NV1050_BLOCK_FW_IO;
    int got = vfs_read(path, bytes, size);
    if (got != size) { kfree(bytes); return NV1050_BLOCK_FW_IO; }
    sha256(bytes, entry->size, digest);
    kfree(bytes);
    return 0;
}

static uint64_t default_map(void *ctx, struct device *dev)
{
    (void)ctx;
    return dev_bar_map(dev, 0);
}

static uint32_t default_read32(void *ctx, uint64_t base, uint32_t offset)
{
    (void)ctx;
    return *(volatile uint32_t *)(uintptr_t)(base + offset);
}

int nvidia_pascal_accel_prepare(struct device *dev)
{
    const struct nv1050_fw_reader fw = { NULL, default_measure };
    const struct nv1050_hw_reader hw = { NULL, default_map, default_read32 };
    int rc = nv1050_accel_prepare_with_ops(dev, &fw, &hw, &active_info);
    kprintf("[nv-accel] stage=%u blocker=%s firmware=%u/%u "
            "chipset=%x boot0=%x reads=%u writes=%u fallback=cpu\n",
            (unsigned)active_info.stage,
            nvidia_pascal_accel_blocker_name(active_info.blocker),
            active_info.firmware_verified, active_info.firmware_files,
            active_info.chipset, active_info.boot0,
            active_info.mmio_reads, active_info.mmio_writes);
    return rc;
}

int nvidia_pascal_accel_query(struct nv1050_accel_info *out)
{
    if (!out) return -1;
    *out = active_info;
    return active_info.stage == NV1050_ACCEL_ACTIVE ? 0 : -1;
}

void nvidia_pascal_accel_reset(void)
{
    info_init(&active_info, NULL);
}

int nvidia_pascal_accel_fill(uint64_t dst_gpu_va, uint32_t dst_pitch,
                             uint32_t width, uint32_t height, uint32_t color)
{
    (void)dst_gpu_va; (void)dst_pitch; (void)width; (void)height; (void)color;
    /* No fallback is hidden here: the framebuffer layer owns its existing CPU
     * path and invokes it after this explicit refusal. */
    return -1;
}

int nvidia_pascal_accel_copy(uint64_t dst_gpu_va, uint32_t dst_pitch,
                             uint64_t src_gpu_va, uint32_t src_pitch,
                             uint32_t width, uint32_t height)
{
    (void)dst_gpu_va; (void)dst_pitch; (void)src_gpu_va; (void)src_pitch;
    (void)width; (void)height;
    return -1;
}

const char *nvidia_pascal_accel_blocker_name(enum nv1050_accel_blocker blocker)
{
    switch (blocker) {
    case NV1050_BLOCK_NONE: return "none";
    case NV1050_BLOCK_WRONG_DEVICE: return "wrong-device";
    case NV1050_BLOCK_BAD_BAR0: return "bad-bar0";
    case NV1050_BLOCK_FW_MISSING: return "firmware-missing";
    case NV1050_BLOCK_FW_SIZE: return "firmware-size";
    case NV1050_BLOCK_FW_HASH: return "firmware-sha256";
    case NV1050_BLOCK_FW_IO: return "firmware-io";
    case NV1050_BLOCK_MAP_FAILED: return "bar0-map";
    case NV1050_BLOCK_WRONG_CHIPSET: return "wrong-chipset";
    case NV1050_BLOCK_CHANNEL_STACK: return "mmu-fifo-channel-ce";
    default: return "unknown";
    }
}
