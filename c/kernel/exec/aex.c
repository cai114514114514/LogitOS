#include <stdint.h>
#include "aex.h"
#include "elf.h"

static int magic_ok(const struct aex_header *h)
{
    return h->magic[0] == 'A' && h->magic[1] == 'E' &&
           h->magic[2] == 'X' && h->magic[3] == '1' &&
           h->version == AEX_VERSION;
}

static void copy_field(char *dst, const char *src, int n)
{
    int i = 0;
    for (; i < n - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

int aex_info(const void *file, char *out_name, char *out_ext)
{
    const struct aex_header *h = file;
    if (!magic_ok(h))
        return -1;
    if (out_name) copy_field(out_name, h->name, 32);
    if (out_ext)  copy_field(out_ext, h->ext, 8);
    return 0;
}

int aex_elf_range(const void *file, uint64_t file_size, const void **elf, uint64_t *elf_size)
{
    const struct aex_header *h = file;
    if (file_size <= AEX_HDR_SIZE) return -1;
    if (!magic_ok(h)) return -1;
    if (elf)      *elf = (const uint8_t *)file + AEX_HDR_SIZE;
    if (elf_size) *elf_size = file_size - AEX_HDR_SIZE;
    return 0;
}

int aex_load_image(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                   struct elf_image *out)
{
    const void *elf; uint64_t elfsz;
    if (aex_elf_range(file, file_size, &elf, &elfsz) != 0)
        return -1;
    const struct aex_header *h = file;
    if (out_name) copy_field(out_name, h->name, 32);
    if (out_ext)  copy_field(out_ext, h->ext, 8);
    return elf_load_image((void *)elf, elfsz, out) == ELF_OK ? 0 : -1;
}

uint64_t aex_load(const void *file, uint64_t file_size, char *out_name, char *out_ext,
                  uint64_t *out_top)
{
    struct elf_image img;
    if (out_top) *out_top = 0;
    if (aex_load_image(file, file_size, out_name, out_ext, &img) != 0)
        return 0;
    if (out_top) *out_top = img.top;
    return img.entry;
}
