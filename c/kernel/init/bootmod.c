/* Boot-module registry: see bootmod.h for the contract.
 *
 * The tags are read from the same normalized view smbios.c and hwhud.c walk:
 * u32 total_size, u32 reserved, then 8-aligned tags that end with type 0, with
 * extension tags keeping their LOGIT_BOOT_TAG_* type (bootinfo.c). The loader
 * is trusted no further than any other firmware-derived input: every field is
 * bounds-checked before use, because a bad tag here would hold the wrong pages
 * or hand out a pointer into someone else's memory. */
#include <stddef.h>
#include <stdint.h>
#include "bootmod.h"
#include "abi/logit_boot.h"
#include "kprintf.h"
#include "mmhost.h"
#include "pmm.h"

/* Same bound smbios.c applies to the whole block. */
#define BOOT_INFO_MAX 0x100000u
/* Sanity ceiling for one module; the biggest Polaris file is under 400 KiB and
 * files.c refuses anything over 2 MiB, so this only stops absurd tags. */
#define BOOTMOD_BYTES_MAX (16ull * 1024 * 1024)

struct bootmod {
    char name[LOGIT_BOOT_MODULE_NAME_MAX];
    uint64_t start;
    uint64_t bytes;
};

/* The loader stops at the same count, so a full registry is a loader bug. */
static struct bootmod modules[LOGIT_BOOT_MODULES_MAX];
static unsigned module_count;
/* The loader's LOGIT_BOOT_TAG_GPU_OPTOUT was present and well formed. */
static int gpu_optout_assumed;

/* Why a local compare: this file must build in the kernel (no string.h
 * guarantee for the freestanding includes) and in the host test unchanged. */
static int name_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Length of the NUL-terminated name inside a tag of SIZE bytes, or -1 when the
 * name is empty, too long, unterminated before the tag ends, or holds a byte
 * outside printable ASCII. A '/' is refused too: the name is a file name, and
 * a path separator would let a tag impersonate a directory lookup. */
static int tag_name_length(const uint8_t *tag, uint32_t size)
{
    const char *name = (const char *)tag + sizeof(struct logit_boot_module_tag);
    uint32_t room = size - (uint32_t)sizeof(struct logit_boot_module_tag);
    for (uint32_t i = 0; i < room && i < LOGIT_BOOT_MODULE_NAME_MAX; i++) {
        if (name[i] == '\0')
            return i ? (int)i : -1;
        if (name[i] < 0x20 || name[i] > 0x7e || name[i] == '/')
            return -1;
    }
    return -1;
}

/* True when [start, start + bytes) touches a module already registered. Two
 * tags naming overlapping pages would let one file's bytes alias another's. */
static int overlaps_registered(uint64_t start, uint64_t bytes)
{
    for (unsigned i = 0; i < module_count; i++) {
        if (start < modules[i].start + modules[i].bytes && modules[i].start < start + bytes)
            return 1;
    }
    return 0;
}

/* Validate one module tag and, if it is sound, register and hold it. Returns
 * nothing: a bad tag is reported and dropped so one corrupt entry cannot take
 * the other firmware files down with it. */
static void register_module(const uint8_t *raw, uint32_t size)
{
    const struct logit_boot_module_tag *tag = (const struct logit_boot_module_tag *)raw;
    if (size < sizeof(*tag) + 2) {
        kprintf("[bootmod] rejected tag: too small for a name\n");
        return;
    }
    int length = tag_name_length(raw, size);
    if (length < 0) {
        kprintf("[bootmod] rejected tag: bad name\n");
        return;
    }
    const char *name = (const char *)raw + sizeof(*tag);
    uint64_t start = tag->mod_start;
    uint64_t end = tag->mod_end;
    if (end <= start || end - start > BOOTMOD_BYTES_MAX || end > PHYSMAP_SIZE) {
        kprintf("[bootmod] rejected %s: bad range\n", name);
        return;
    }
    uint64_t bytes = end - start;
    if (overlaps_registered(start, bytes)) {
        kprintf("[bootmod] rejected %s: overlaps another module\n", name);
        return;
    }
    for (unsigned i = 0; i < module_count; i++) {
        if (name_equal(modules[i].name, name)) {
            kprintf("[bootmod] rejected %s: duplicate name\n", name);
            return;
        }
    }
    if (module_count == LOGIT_BOOT_MODULES_MAX) {
        kprintf("[bootmod] rejected %s: registry full\n", name);
        return;
    }
    /* Hold BEFORE registering: a module the allocator may still hand out must
     * never be findable. */
    if (pmm_hold_range(start, bytes)) {
        kprintf("[bootmod] rejected %s: cannot hold its pages\n", name);
        return;
    }
    struct bootmod *slot = &modules[module_count++];
    for (int i = 0; i <= length; i++)
        slot->name[i] = name[i];
    slot->start = start;
    slot->bytes = bytes;
    kprintf("[bootmod] %s %llu bytes\n", slot->name, (unsigned long long)bytes);
}

/* Why the tag is checked for its exact size: it carries no payload, and a tag
 * of any other size is not the loader's message, so it must not switch the
 * self-test off by accident (nor be believed as an opt-in by anything). */
static void register_gpu_optout_tag(uint32_t size)
{
    if (size != sizeof(struct logit_boot_tag)) {
        kprintf("[bootmod] rejected GPU opt-out tag: bad size\n");
        return;
    }
    gpu_optout_assumed = 1;
    kprintf("[bootmod] GPU opt-out assumed by the loader\n");
}

void bootmod_init(uint64_t info)
{
    module_count = 0;
    gpu_optout_assumed = 0;
    if (!info)
        return;
    const uint8_t *base = (const uint8_t *)mm_p2v(info);
    uint32_t total = *(const uint32_t *)base;
    if (total < 16 || total > BOOT_INFO_MAX)
        return;
    for (uint32_t off = 8; off + 8 <= total; ) {
        uint32_t type = *(const uint32_t *)(base + off);
        uint32_t size = *(const uint32_t *)(base + off + 4);
        if (type == 0 || size < 8 || size > total - off)
            return;
        if (type == LOGIT_BOOT_TAG_MODULE)
            register_module(base + off, size);
        else if (type == LOGIT_BOOT_TAG_GPU_OPTOUT)
            register_gpu_optout_tag(size);
        off += (size + 7) & ~7u;
    }
}

/* A module is addressable only through the direct map. Below 1 GiB that is the
 * low alias pmm_init completes first; above it, the high map, which pmm_init
 * may have refused to build. Reading an unmapped alias would fault in the
 * middle of driver probing, so report "not there" instead. */
static int module_reachable(const struct bootmod *module)
{
    int mapped = module->start + module->bytes <= PMM_LOW_LIMIT
        ? pmm_physmap_low_ready() : pmm_physmap_ready();
    return mapped && pmm_is_ram(module->start, module->bytes);
}

const void *bootmod_find(const char *name, uint64_t *bytes)
{
    if (!name)
        return NULL;
    for (unsigned i = 0; i < module_count; i++) {
        if (!name_equal(modules[i].name, name))
            continue;
        if (!module_reachable(&modules[i]))
            return NULL;
        if (bytes)
            *bytes = modules[i].bytes;
        return mm_p2v(modules[i].start);
    }
    return NULL;
}

unsigned bootmod_count(void)
{
    return module_count;
}

int bootmod_gpu_optout_assumed(void)
{
    return gpu_optout_assumed;
}
