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

/* The BIOS loader cannot include C declarations.  These wire offsets are
 * therefore named here, emitted into its generated NASM include, and checked
 * against the packed structs below.  Keeping the numbers beside the C shape
 * means a field move cannot leave an old, plausible displacement hidden in
 * loader.asm; tests/bootself.mk also verifies that each BIOS write uses the
 * symbol for the field it claims to populate. */
#define LOGIT_BOOT_HEADER_MAGIC_OFFSET                 0x00
#define LOGIT_BOOT_HEADER_VERSION_OFFSET               0x04
#define LOGIT_BOOT_HEADER_HEADER_SIZE_OFFSET           0x06
#define LOGIT_BOOT_HEADER_IDENTITY_MAP_BYTES_OFFSET    0x08
#define LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET            0x10
#define LOGIT_BOOT_HEADER_RESERVED_OFFSET              0x14
#define LOGIT_BOOT_TAG_TYPE_OFFSET                     0x00
#define LOGIT_BOOT_TAG_SIZE_OFFSET                     0x04
#define LOGIT_BOOT_TAG_SIZE                            0x08
#define LOGIT_BOOT_MMAP_TAG_ENTRY_SIZE_OFFSET          0x08
#define LOGIT_BOOT_MMAP_TAG_ENTRY_VERSION_OFFSET       0x0c
#define LOGIT_BOOT_MMAP_TAG_ENTRIES_OFFSET             0x10
#define LOGIT_BOOT_MMAP_TAG_HEADER_SIZE                0x10
#define LOGIT_BOOT_MMAP_ENTRY_ADDR_OFFSET              0x00
#define LOGIT_BOOT_MMAP_ENTRY_LEN_OFFSET               0x08
#define LOGIT_BOOT_MMAP_ENTRY_TYPE_OFFSET              0x10
#define LOGIT_BOOT_MMAP_ENTRY_RESERVED_OFFSET          0x14
#define LOGIT_BOOT_MMAP_ENTRY_SIZE                     0x18
#define LOGIT_BOOT_FRAMEBUFFER_TAG_ADDR_OFFSET         0x08
#define LOGIT_BOOT_FRAMEBUFFER_TAG_PITCH_OFFSET        0x10
#define LOGIT_BOOT_FRAMEBUFFER_TAG_WIDTH_OFFSET        0x14
#define LOGIT_BOOT_FRAMEBUFFER_TAG_HEIGHT_OFFSET       0x18
#define LOGIT_BOOT_FRAMEBUFFER_TAG_BPP_OFFSET          0x1c
#define LOGIT_BOOT_FRAMEBUFFER_TAG_TYPE_OFFSET         0x1d
#define LOGIT_BOOT_FRAMEBUFFER_TAG_RESERVED_OFFSET     0x1e
#define LOGIT_BOOT_FRAMEBUFFER_TAG_RED_POSITION_OFFSET 0x20
#define LOGIT_BOOT_FRAMEBUFFER_TAG_RED_SIZE_OFFSET     0x21
#define LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_POSITION_OFFSET 0x22
#define LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_SIZE_OFFSET   0x23
#define LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_POSITION_OFFSET 0x24
#define LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_SIZE_OFFSET    0x25
#define LOGIT_BOOT_FRAMEBUFFER_TAG_SIZE                0x26

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
_Static_assert(sizeof(struct logit_boot_tag) == LOGIT_BOOT_TAG_SIZE,
               "native tag header layout drifted");
_Static_assert(sizeof(struct logit_boot_mmap_entry) == LOGIT_BOOT_MMAP_ENTRY_SIZE,
               "native memory-map entry layout drifted");
_Static_assert(sizeof(struct logit_boot_mmap_tag) == LOGIT_BOOT_MMAP_TAG_HEADER_SIZE,
               "native memory-map tag header layout drifted");
_Static_assert(sizeof(struct logit_boot_framebuffer_tag) == LOGIT_BOOT_FRAMEBUFFER_TAG_SIZE,
               "native framebuffer tag layout drifted");

#define LOGIT_BOOT_ASSERT_OFFSET(type, field, expected) \
    _Static_assert(__builtin_offsetof(type, field) == (expected), \
                   "native boot " #type "." #field " offset drifted")
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_header, magic,
                         LOGIT_BOOT_HEADER_MAGIC_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_header, version,
                         LOGIT_BOOT_HEADER_VERSION_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_header, header_size,
                         LOGIT_BOOT_HEADER_HEADER_SIZE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_header, identity_map_bytes,
                         LOGIT_BOOT_HEADER_IDENTITY_MAP_BYTES_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_header, total_size,
                         LOGIT_BOOT_HEADER_TOTAL_SIZE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_header, reserved,
                         LOGIT_BOOT_HEADER_RESERVED_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_tag, type,
                         LOGIT_BOOT_TAG_TYPE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_tag, size,
                         LOGIT_BOOT_TAG_SIZE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_mmap_tag, entry_size,
                         LOGIT_BOOT_MMAP_TAG_ENTRY_SIZE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_mmap_tag, entry_version,
                         LOGIT_BOOT_MMAP_TAG_ENTRY_VERSION_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_mmap_tag, entries,
                         LOGIT_BOOT_MMAP_TAG_ENTRIES_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_mmap_entry, addr,
                         LOGIT_BOOT_MMAP_ENTRY_ADDR_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_mmap_entry, len,
                         LOGIT_BOOT_MMAP_ENTRY_LEN_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_mmap_entry, type,
                         LOGIT_BOOT_MMAP_ENTRY_TYPE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_mmap_entry, reserved,
                         LOGIT_BOOT_MMAP_ENTRY_RESERVED_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, addr,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_ADDR_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, pitch,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_PITCH_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, width,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_WIDTH_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, height,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_HEIGHT_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, bpp,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_BPP_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, framebuffer_type,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_TYPE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, reserved,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_RESERVED_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, red_position,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_RED_POSITION_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, red_mask_size,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_RED_SIZE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, green_position,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_POSITION_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, green_mask_size,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_GREEN_SIZE_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, blue_position,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_POSITION_OFFSET);
LOGIT_BOOT_ASSERT_OFFSET(struct logit_boot_framebuffer_tag, blue_mask_size,
                         LOGIT_BOOT_FRAMEBUFFER_TAG_BLUE_SIZE_OFFSET);
#undef LOGIT_BOOT_ASSERT_OFFSET

uint64_t logit_boot_normalize(uint64_t info_addr, uint64_t entry_magic);

#endif
