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
#define PF_X 1
#define PF_W 2
#define USER_BASE  0x40000000ull
#define USER_LIMIT 0x80000000ull

uint64_t elf_load(void *image, int image_len)
{
    if (!image || image_len < (int)sizeof(struct elf64_ehdr))
        return 0;
    struct elf64_ehdr *eh = image;

    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_ident[4] != 2)            /* ELFCLASS64 */
        return 0;
    if (eh->e_phentsize != sizeof(struct elf64_phdr))
        return 0;
    if (eh->e_phnum == 0)
        return 0;
    if (eh->e_phoff > (uint64_t)image_len)
        return 0;
    uint64_t phbytes = (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr);
    if (phbytes > (uint64_t)image_len - eh->e_phoff)
        return 0;
    if (eh->e_entry < USER_BASE || eh->e_entry >= USER_LIMIT)
        return 0;

    struct elf64_phdr *ph = (struct elf64_phdr *)((uint8_t *)image + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD)
            continue;

        if (ph[i].p_filesz > ph[i].p_memsz)
            return 0;
        if (ph[i].p_offset > (uint64_t)image_len ||
            ph[i].p_filesz > (uint64_t)image_len - ph[i].p_offset)
            return 0;
        if (ph[i].p_memsz == 0)
            continue;
        if (ph[i].p_vaddr < USER_BASE) {
            if (ph[i].p_memsz <= USER_BASE - ph[i].p_vaddr)
                continue;                       /* lld's ELF/PHDR header LOAD */
            return 0;
        }
        if (ph[i].p_vaddr >= USER_LIMIT)
            return 0;
        if (ph[i].p_memsz > USER_LIMIT - ph[i].p_vaddr)
            return 0;

        uint64_t start = ph[i].p_vaddr & ~(uint64_t)0xFFF;
        uint64_t end   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uint64_t)0xFFF;
        if (end > USER_LIMIT || end < start)
            return 0;
        uint64_t flags = VMM_USER | ((ph[i].p_flags & PF_W) ? VMM_WRITABLE : 0);
        for (uint64_t a = start; a < end; a += 0x1000) {
            uint64_t frame = pmm_alloc();
            if (!frame) return 0;
            memset((void *)frame, 0, 0x1000);          /* zero (covers .bss) */
            vmm_map_page(a, frame, flags);
        }

        /* Copy file-backed bytes to the now-mapped user virtual address. */
        memcpy((void *)ph[i].p_vaddr, (uint8_t *)image + ph[i].p_offset,
               ph[i].p_filesz);
    }

    return eh->e_entry;
}
