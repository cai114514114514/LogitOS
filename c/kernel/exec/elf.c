#include <stdint.h>
#include <stddef.h>
#include "elf.h"
#include "vmm.h"
#include "pmm.h"

void *memcpy(void *, const void *, size_t);   /* lib/string.c */
void *memset(void *, int, size_t);

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} __attribute__((packed));

#define PT_LOAD 1

/* The private user region (PML4[0]/PDPT[1]) is exactly 1 GiB. A segment or entry
 * point outside it would map/write into the SHARED kernel page tables (next_table
 * propagates USER into them), so the whole load is rejected instead. */
#define USER_VA_BASE 0x40000000ull
#define USER_VA_END  0x80000000ull

uint64_t elf_load(void *image, uint64_t image_size)
{
    struct elf64_ehdr *eh = image;

    if (image_size < sizeof *eh) return 0;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 2)            /* ELFCLASS64 */
        return 0;

    /* The program-header table is disk-controlled: bound it to the image before
     * dereferencing (subtraction form avoids overflow in the add). */
    if (eh->e_phentsize != sizeof(struct elf64_phdr)) return 0;
    if (eh->e_phnum > 64) return 0;                      /* sane cap */
    if (eh->e_phoff > image_size ||
        (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > image_size - eh->e_phoff)
        return 0;

    /* The entry point must land in the user region too, with 64 MiB of headroom
     * above it: setup_cli_stack() maps the CLI stack at (entry & ~0xFFFFF) +
     * 0x4000000. (Subtraction form: no overflow for a huge e_entry.) */
    if (eh->e_entry < USER_VA_BASE || eh->e_entry > USER_VA_END - 0x4000000)
        return 0;

    struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        /* Validate the segment: file bytes must fit the image, file<=mem, and the
         * target VA range must lie wholly inside the private user region. */
        if (ph[i].p_filesz > ph[i].p_memsz) return 0;
        if (ph[i].p_offset > image_size || ph[i].p_filesz > image_size - ph[i].p_offset)
            return 0;
        uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        if (end < start) return 0;                  /* p_vaddr + p_memsz overflow */
        /* Some lld versions emit a read-only headers PT_LOAD at a low fixed VA
         * (0x200000). It is unused at runtime, and mapping it into the SHARED
         * low region with USER would hand ring-3 a window over kernel memory --
         * skip it instead of mapping. Anything straddling or above the user
         * region boundary is still rejected outright. */
        if (end <= USER_VA_BASE) continue;
        if (start < USER_VA_BASE || end > USER_VA_END) return 0;  /* outside the user region */

        for (uint64_t a = start; a < end; a += 0x1000) {
            uint64_t frame = pmm_alloc();
            if (!frame) return 0;                      /* OOM: caller aborts + frees the space */
            memset((void *)frame, 0, 0x1000);          /* zero (covers .bss) */
            vmm_map_page(a, frame, VMM_WRITABLE | VMM_USER);
        }

        /* Copy file-backed bytes to the now-mapped user virtual address. */
        memcpy((void *)ph[i].p_vaddr, (uint8_t *)image + ph[i].p_offset,
               ph[i].p_filesz);
    }

    return eh->e_entry;
}
