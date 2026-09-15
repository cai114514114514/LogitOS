#ifndef LOGIT_BOOT_H
#define LOGIT_BOOT_H

#include <stdint.h>

/* Logit native boot protocol, version 1.
 *
 * Entry is x86-64 long mode with IF=0, four-level paging, CR4.PAE=1 and
 * EFER.LME=1.  RAX carries LOGIT_BOOT_MAGIC and RDI carries the physical
 * address of struct logit_boot_header.  CS is the loader's flat 64-bit code
 * selector 0x08, DS/ES/SS its flat writable selector 0x10, and RSP is a
 * temporary aligned low-memory stack; the kernel replaces it before calling
 * C.  Matching the kernel GDT indices matters after gdt_init(): an interrupt
 * returning to a loader-only selector would otherwise #GP at IRET.
 *
 * The loader identity-maps exactly [0, LOGIT_BOOT_IDENTITY_MAP_BYTES) with
 * writable/executable 2 MiB leaves and maps nothing above it.  This is only a
 * runway through early kernel entry.  It is deliberately not the physical
 * memory model: pmm.c extends the active tables from the firmware memory map,
 * keeping that policy in one kernel implementation instead of duplicating it
 * across BIOS and UEFI loaders.
 *
 * There is deliberately no discoverable/scanned kernel header for this
 * protocol.  A native-loader build selects logit_native_start as ELF e_entry;
 * loaders continue to validate and load ELF PT_LOAD program headers.
 *
 * The numeric constants below are authoritative.  tests/bootself.mk derives
 * the NASM include used by the BIOS loader from these #defines, so a second
 * spelling cannot silently agree on the wrong value.
 */
#define LOGIT_BOOT_MAGIC                 0x4c425436
#define LOGIT_BOOT_VERSION               0x0001
#define LOGIT_BOOT_HEADER_SIZE           0x0018
#define LOGIT_BOOT_IDENTITY_MAP_BYTES    0x40000000
#define LOGIT_BOOT_IDENTITY_PAGE_BYTES   0x00200000
#define LOGIT_BOOT_BASE_PAGE_BYTES       0x00001000

/* These are intentionally outside the retired Multiboot2 namespace. Unknown
 * tags are skipped using size rounded up to eight bytes; END must be last. */
#define LOGIT_BOOT_TAG_MEMORY_MAP        0x4c420101
#define LOGIT_BOOT_TAG_FRAMEBUFFER       0x4c420102
#define LOGIT_BOOT_TAG_ACPI_OLD          0x4c420103
#define LOGIT_BOOT_TAG_ACPI_NEW          0x4c420104
#define LOGIT_BOOT_TAG_END               0x4c42ffff
#define LOGIT_BOOT_TAG_NAMESPACE_MASK    0xffff0000
#define LOGIT_BOOT_TAG_NAMESPACE         0x4c420000

/* Memory kinds preserve the firmware/E820 values already consumed by pmm.c.
 * Renumbering them would buy no native-protocol property and would make the
 * entry adapter translate descriptor contents as well as tag provenance. */
#define LOGIT_BOOT_MEMORY_AVAILABLE      1
#define LOGIT_BOOT_MEMORY_RESERVED       2
#define LOGIT_BOOT_MEMORY_ACPI_RECLAIM   3
#define LOGIT_BOOT_MEMORY_NVS            4
#define LOGIT_BOOT_MEMORY_BADRAM         5

struct logit_boot_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t identity_map_bytes;
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed));

struct logit_boot_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

/* Payloads deliberately retain the useful shapes of the retired protocol.
 * Only provenance and type numbers change, so entry-time normalization can
 * leave one reader per fact rather than teaching every consumer a second tag
 * walker. The compatibility is internal layout reuse, not a second boot ABI. */
struct logit_boot_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed));

struct logit_boot_mmap_tag {
    struct logit_boot_tag tag;
    uint32_t entry_size;
    uint32_t entry_version;
    struct logit_boot_mmap_entry entries[];
} __attribute__((packed));

struct logit_boot_framebuffer_tag {
    struct logit_boot_tag tag;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t red_position, red_mask_size;
    uint8_t green_position, green_mask_size;
    uint8_t blue_position, blue_mask_size;
} __attribute__((packed));

_Static_assert(sizeof(struct logit_boot_header) == LOGIT_BOOT_HEADER_SIZE,
               "native boot header layout drifted");
_Static_assert(sizeof(struct logit_boot_mmap_entry) == 24,
               "native memory-map entry layout drifted");
_Static_assert(sizeof(struct logit_boot_framebuffer_tag) == 38,
               "native framebuffer tag layout drifted");

uint64_t logit_boot_normalize(uint64_t info_addr, uint64_t entry_magic);

#endif
