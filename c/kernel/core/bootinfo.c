#include <stdint.h>
#include "abi/logit_boot.h"
#include "serial.h"

/* The three boot consumers intentionally keep one parser each.  Native tags
 * have independent type numbers on the wire, then this entry-only adapter
 * validates their framing and rewrites only those numbers into the existing
 * canonical MB2-shaped view.  The payload bytes never move: pmm.c, fb.c and
 * acpi.c therefore cannot diverge between two walkers for the same fact.
 *
 * Header fields are ordered so the canonical eight-byte header begins at
 * native offset 16.  Normalization costs five type stores plus one total-size
 * adjustment on the measured SeaBIOS block; copying a second map would need
 * another low-memory capacity rule and could silently reorder descriptors. */

#define MB2_TAG_END         0u
#define MB2_TAG_MMAP        6u
#define MB2_TAG_FRAMEBUFFER 8u
#define MB2_TAG_ACPI_OLD    14u
#define MB2_TAG_ACPI_NEW    15u

static void append_hex(char **out, uint64_t value, unsigned digits)
{
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned shift = digits * 4; shift; ) {
        shift -= 4;
        *(*out)++ = hex[(value >> shift) & 15u];
    }
}

static void version_refused(uint16_t got)
{
    char line[] = "LOGIT BOOT VERSION REFUSED got=0000 wanted=0000\n";
    char *p = line + 31;
    append_hex(&p, got, 4);
    p = line + 43;
    append_hex(&p, LOGIT_BOOT_VERSION, 4);
    serial_puts(line);
}

static __attribute__((noreturn)) void refuse(const char *message)
{
    serial_puts(message);
    for (;;) __asm__ volatile ("cli; hlt");
}

uint64_t logit_boot_normalize(uint64_t info_addr, uint64_t entry_magic)
{
    serial_init();
    if (entry_magic != LOGIT_BOOT_MAGIC)
        refuse("LOGIT BOOT ENTRY MAGIC REFUSED\n");
    if (info_addr >= LOGIT_BOOT_IDENTITY_MAP_BYTES - LOGIT_BOOT_HEADER_SIZE)
        refuse("LOGIT BOOT HEADER OUTSIDE IDENTITY MAP\n");

    struct logit_boot_header *header =
        (struct logit_boot_header *)(uintptr_t)info_addr;
    if (header->magic != LOGIT_BOOT_MAGIC)
        refuse("LOGIT BOOT BLOCK MAGIC REFUSED\n");
    if (header->version != LOGIT_BOOT_VERSION) {
        version_refused(header->version);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    if (header->header_size != LOGIT_BOOT_HEADER_SIZE || header->reserved ||
        header->total_size < LOGIT_BOOT_HEADER_SIZE + sizeof(struct logit_boot_tag) ||
        header->total_size > LOGIT_BOOT_IDENTITY_MAP_BYTES - info_addr)
        refuse("LOGIT BOOT HEADER EXTENT REFUSED\n");
    if (header->identity_map_bytes != LOGIT_BOOT_IDENTITY_MAP_BYTES)
        refuse("LOGIT BOOT IDENTITY EXTENT REFUSED\n");

    /* Read the final byte promised by the runway before trusting any tag.
     * The short-map negative control splits the final 2 MiB leaf and leaves
     * precisely its last 4 KiB PTE absent.  It must fault after the observable
     * line below, not wander into C with a promise the tables did not keep. */
    serial_puts("LOGIT BOOT IDENTITY VERIFY promised=0000000040000000\n");
    (void)*(volatile const uint8_t *)(uintptr_t)(header->identity_map_bytes - 1);

    uint8_t *base = (uint8_t *)(uintptr_t)info_addr;
    uint8_t *cursor = base + header->header_size;
    uint8_t *end = base + header->total_size;
    int saw_end = 0;
    while (cursor + sizeof(struct logit_boot_tag) <= end) {
        struct logit_boot_tag *tag = (struct logit_boot_tag *)cursor;
        if (tag->size < sizeof(*tag) || tag->size > (uint64_t)(end - cursor))
            refuse("LOGIT BOOT TAG LIST TRUNCATED\n");
        uint64_t step = ((uint64_t)tag->size + 7u) & ~7ull;
        if (!step || step > (uint64_t)(end - cursor))
            refuse("LOGIT BOOT TAG LIST TRUNCATED\n");

        switch (tag->type) {
        case LOGIT_BOOT_TAG_MEMORY_MAP:  tag->type = MB2_TAG_MMAP; break;
        case LOGIT_BOOT_TAG_FRAMEBUFFER: tag->type = MB2_TAG_FRAMEBUFFER; break;
        case LOGIT_BOOT_TAG_ACPI_OLD:    tag->type = MB2_TAG_ACPI_OLD; break;
        case LOGIT_BOOT_TAG_ACPI_NEW:    tag->type = MB2_TAG_ACPI_NEW; break;
        case LOGIT_BOOT_TAG_END:
            if (tag->size != sizeof(*tag) || cursor + step != end)
                refuse("LOGIT BOOT TAG LIST TRUNCATED\n");
            tag->type = MB2_TAG_END;
            saw_end = 1;
            break;
        default:
            /* Extensions remain skippable, but only inside our namespace.
             * Otherwise an accidentally MB2-numbered tag would survive this
             * adapter and erase the provenance distinction the protocol was
             * introduced to make explicit. */
            if ((tag->type & LOGIT_BOOT_TAG_NAMESPACE_MASK) !=
                LOGIT_BOOT_TAG_NAMESPACE)
                refuse("LOGIT BOOT FOREIGN TAG TYPE REFUSED\n");
            break;
        }
        cursor += step;
        if (saw_end) break;
    }
    if (!saw_end || cursor != end)
        refuse("LOGIT BOOT TAG LIST TRUNCATED\n");

    /* Native offsets 16/20 are now the canonical total_size/reserved header;
     * the native prefix remains in the same reserved low-memory page. */
    uint32_t *canonical = (uint32_t *)(base + 16);
    canonical[0] = header->total_size - 16;
    canonical[1] = 0;
    serial_puts("LOGIT_BOOT_NATIVE_OK version=0001\n");
    return info_addr + 16;
}
