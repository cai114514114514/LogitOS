/* boot/efi/mb2.h -- the Multiboot2 information block this loader FORGES.
 *
 * The kernel's boot contract is Multiboot2 and it does not change in this
 * milestone: c/boot/boot.asm:38 checks eax == 0x36d76289 and every consumer
 * walks the tag list ebx points at. So the honest description of this loader
 * is that it IMPERSONATES GRUB -- it gathers from UEFI what GRUB gathers from
 * BIOS and hands over a byte-identical-in-shape structure.
 *
 * There are exactly three consumers in the kernel today, and each one is the
 * authority for the layout of its tag. They are cited by file and line beside
 * each struct below, because these structs are not "the spec" -- they are what
 * the code on the other side of the jump reads, and where the two disagree the
 * code wins (see the reserved-field note on the framebuffer tag, which is a
 * real disagreement, not a typo).
 *
 *   type 6  memory map -> c/kernel/mm/pmm.c:59-77, pmm_init()
 *   type 8  framebuffer -> c/kernel/gui/fb.c:11-22, fb_init()
 *   type 15 ACPI 2.0 RSDP -> nobody yet; see acpi-mb2-tag.patch
 *
 * Tags are 8-byte aligned and terminated by a type-0 tag (Multiboot2 spec
 * sec 3.6). pmm's walk breaks on `size == 0` and fb's on `size < 8`, so a tag
 * whose size field is wrong does not corrupt the kernel -- it truncates the
 * list, and the kernel then reports no memory or no framebuffer. That is a
 * mercy for debugging and not a licence to get the sizes wrong.
 */
#ifndef LOGIT_MB2_H
#define LOGIT_MB2_H

#include "efi.h"

/* c/boot/boot.asm:38 -- the value the kernel compares eax against. */
#define MB2_BOOTLOADER_MAGIC 0x36d76289u

#define MB2_TAG_END        0
#define MB2_TAG_CMDLINE    1
#define MB2_TAG_LOADER     2
#define MB2_TAG_MMAP       6
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_TAG_ACPI_OLD   14   /* ACPI 1.0 RSDP, 20 bytes */
#define MB2_TAG_ACPI_NEW   15   /* ACPI 2.0+ RSDP, full length */

/* mb2 memory types (spec sec 3.6.8). pmm.c treats ONLY type 1 as usable
 * (MMAP_AVAILABLE, pmm.c:18); the rest exist so the map stays informative
 * rather than collapsing every non-RAM range into one bucket. */
#define MB2_MEM_AVAILABLE  1
#define MB2_MEM_RESERVED   2
#define MB2_MEM_ACPI_RECLAIM 3
#define MB2_MEM_NVS        4
#define MB2_MEM_BADRAM     5

struct mb2_tag {
    UINT32 type;
    UINT32 size;
};

/* Mirrors struct mb2_mmap_entry, c/kernel/mm/pmm.c:64-69. */
struct mb2_mmap_entry {
    UINT64 addr;
    UINT64 len;
    UINT32 type;
    UINT32 reserved;
};
_Static_assert(sizeof(struct mb2_mmap_entry) == 24, "pmm.c:64 reads 24-byte entries");

struct mb2_tag_mmap {
    UINT32 type;            /* = 6 */
    UINT32 size;            /* = 16 + n * entry_size */
    UINT32 entry_size;      /* = 24; pmm.c:236 refuses anything smaller */
    UINT32 entry_version;   /* = 0 */
    struct mb2_mmap_entry entries[];
};
_Static_assert(OFF(struct mb2_tag_mmap, entries) == 16, "pmm.c:71-75");

/* Mirrors struct mb2_fb_tag, c/kernel/gui/fb.c:13-22.
 *
 * THE DISAGREEMENT WORTH KNOWING: the Multiboot2 spec's prose (sec 3.6.12)
 * shows a single `u8 reserved` after framebuffer_type, which would put the
 * colour fields at offset 31. The reference implementation's header
 * (GRUB's multiboot2.h, struct multiboot_tag_framebuffer_common) declares
 * `multiboot_uint16_t reserved`, putting them at 32 -- and GRUB is what has
 * been booting this kernel, so fb.c was written against 32 and is right.
 * Emitting the spec's 31 here would shift red/green/blue by one byte and the
 * desktop would come up in a colour scheme nobody chose. Follow the header. */
struct mb2_tag_framebuffer {
    UINT32 type;            /* = 8 */
    UINT32 size;            /* = 38 for the RGB variant */
    UINT64 addr;
    UINT32 pitch;           /* BYTES per scanline, not pixels */
    UINT32 width;
    UINT32 height;
    UINT8  bpp;             /* fb.c:157 requires 32 */
    UINT8  fb_type;         /* fb.c:157 requires 1 (direct RGB) */
    UINT16 reserved;
    UINT8  red_pos,   red_size;
    UINT8  green_pos, green_size;
    UINT8  blue_pos,  blue_size;
};
_Static_assert(OFF(struct mb2_tag_framebuffer, addr)      ==  8, "fb.c:15");
_Static_assert(OFF(struct mb2_tag_framebuffer, pitch)     == 16, "fb.c:16");
_Static_assert(OFF(struct mb2_tag_framebuffer, bpp)       == 28, "fb.c:17");
_Static_assert(OFF(struct mb2_tag_framebuffer, red_pos)   == 32, "fb.c:19 -- see the note above");
_Static_assert(sizeof(struct mb2_tag_framebuffer) == 40, "38 bytes of content + 2 pad");

#define MB2_FB_TYPE_RGB 1

/* ACPI tags (spec sec 3.6.14/3.6.15): the tag header followed by a verbatim
 * copy of the RSDP. Not a pointer -- a copy, because the firmware's own copy
 * lives in EfiACPIReclaimMemory which a kernel is entitled to reuse. */
struct mb2_tag_acpi {
    UINT32 type;            /* 14 (v1 RSDP) or 15 (v2+ RSDP) */
    UINT32 size;            /* 8 + rsdp length */
    UINT8  rsdp[];
};

#endif /* LOGIT_MB2_H */
