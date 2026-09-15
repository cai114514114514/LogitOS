#include "gart.h"
#include "layout.h"

static int bytes_ok(const void *p, size_t n)
{ return p && n && (uintptr_t)p <= UINTPTR_MAX - (n - 1); }
static int alias(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}
int polaris_gart_encode(uint64_t addr, uint64_t flags, uint64_t *out)
{
    const uint64_t allowed = POLARIS_PTE_VALID | POLARIS_PTE_SYSTEM |
        POLARIS_PTE_SNOOPED | POLARIS_PTE_READABLE | POLARIS_PTE_WRITEABLE;
    if (!out || (addr & 4095) || addr >= POLARIS_MEMORY_LIMIT ||
        (flags & ~allowed) ||
        (flags & (POLARIS_PTE_VALID | POLARIS_PTE_SYSTEM)) !=
        (POLARIS_PTE_VALID | POLARIS_PTE_SYSTEM) ||
        !(flags & (POLARIS_PTE_READABLE | POLARIS_PTE_WRITEABLE))) return -1;
#ifdef POLARIS_GART_NEGCTL_SYSTEM
    flags &= ~POLARIS_PTE_SYSTEM;
#endif
    *out = addr | flags;
    return 0;
}
int polaris_gart_build(void *table, size_t capacity, uint64_t table_gpu,
                       uint64_t gpu_base, const uint64_t *pages,
                       size_t count, uint64_t flags,
                       struct polaris_gart_descriptor *out)
{
    struct polaris_gart_descriptor d = {0};
    uint64_t pte;
    size_t bytes;
    if (!count || count > SIZE_MAX / 8 || count > POLARIS_MEMORY_LIMIT / 4096)
        return -1;
    bytes = count * 8;
    if (capacity < bytes || !bytes_ok(table, bytes) || !bytes_ok(pages, bytes) ||
        ((uintptr_t)pages & (_Alignof(uint64_t) - 1)) ||
        !bytes_ok(out, sizeof(*out)) ||
        alias(table, bytes, pages, bytes) || alias(table, bytes, out, sizeof(*out)) ||
        alias(pages, bytes, out, sizeof(*out)) ||
        (table_gpu & 4095) || table_gpu >= POLARIS_MEMORY_LIMIT ||
        bytes > POLARIS_MEMORY_LIMIT - table_gpu ||
        (gpu_base & 4095) || gpu_base >= POLARIS_MEMORY_LIMIT ||
        count > (POLARIS_MEMORY_LIMIT - gpu_base) / 4096) return -1;
    /* Validate ALL addresses before writing; otherwise a bad final page would
     * leave a half-replaced live table and a misleading failed return value. */
    for (size_t i = 0; i < count; ++i)
        if (polaris_gart_encode(pages[i], flags, &pte)) return -1;
    d.start_page = (uint32_t)(gpu_base >> 12);
    d.end_page = (uint32_t)((gpu_base >> 12) + count - 1);
    d.table_base_page = (uint32_t)(table_gpu >> 12);
    d.table_bytes = bytes;
    for (size_t i = 0; i < count; ++i) {
        (void)polaris_gart_encode(pages[i], flags, &pte);
        for (unsigned j = 0; j < 8; ++j) ((uint8_t *)table)[i * 8 + j] = (uint8_t)(pte >> (j * 8));
    }
    *out = d;
    return 0;
}
