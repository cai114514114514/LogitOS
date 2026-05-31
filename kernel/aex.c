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

uint64_t aex_load(const void *file, char *out_name, char *out_ext)
{
    const struct aex_header *h = file;
    if (!magic_ok(h))
        return 0;
    if (out_name) copy_field(out_name, h->name, 32);
    if (out_ext)  copy_field(out_ext, h->ext, 8);
    return elf_load((void *)((const uint8_t *)file + AEX_HDR_SIZE));
}
