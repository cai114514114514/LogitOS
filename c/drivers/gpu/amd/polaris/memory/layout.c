#include "layout.h"

static int range_ok(struct polaris_memory_range r)
{
    return r.bytes && r.base < POLARIS_MEMORY_LIMIT &&
           r.bytes <= POLARIS_MEMORY_LIMIT - r.base;
}
static int contains(struct polaris_memory_range a, struct polaris_memory_range b)
{
    return b.base >= a.base && b.bytes <= a.bytes && b.base - a.base <= a.bytes - b.bytes;
}
static int overlap(struct polaris_memory_range a, struct polaris_memory_range b)
{
    return a.base < b.base + b.bytes && b.base < a.base + a.bytes;
}
static int round_page(uint64_t n, uint64_t *out)
{
    if (!n || n > POLARIS_MEMORY_LIMIT - (POLARIS_MEMORY_PAGE - 1)) return -1;
    *out = (n + POLARIS_MEMORY_PAGE - 1) & ~(uint64_t)(POLARIS_MEMORY_PAGE - 1);
    return 0;
}
int polaris_memory_plan(const struct polaris_memory_request *r,
                        struct polaris_memory_layout *out)
{
    struct polaris_memory_layout p = {0};
    uint64_t sizes[POLARIS_MEMORY_OBJECTS], cursor;
    if (!r || !out || !range_ok(r->vram) || !range_ok(r->aperture) ||
        !range_ok(r->arena) || !range_ok(r->scanout) ||
        !contains(r->vram, r->aperture) || !contains(r->aperture, r->arena) ||
        !contains(r->vram, r->scanout) ||
        (r->arena.base & (POLARIS_MEMORY_PAGE - 1)) ||
        (r->aperture.base & (POLARIS_MEMORY_PAGE - 1)) ||
        (r->aperture_cpu_base & (POLARIS_MEMORY_PAGE - 1)) ||
        r->aperture_cpu_base > UINT64_MAX - (r->aperture.bytes - 1) ||
        r->reserved_count > POLARIS_MEMORY_MAX_RESERVED ||
        (r->reserved_count && !r->reserved)) return -1;
#ifndef POLARIS_MEMORY_NEGCTL_SCANOUT
    if (overlap(r->arena, r->scanout)) return -1;
#endif
    for (size_t i = 0; i < r->reserved_count; ++i)
        if (!range_ok(r->reserved[i]) || !contains(r->vram, r->reserved[i]) ||
            overlap(r->arena, r->reserved[i])) return -1;
    sizes[POLARIS_MEMORY_TOC] = POLARIS_MEMORY_PAGE;
    sizes[POLARIS_MEMORY_SMU_SCRATCH] = POLARIS_SMU_SCRATCH_BYTES;
    sizes[POLARIS_MEMORY_RING] = POLARIS_MEMORY_PAGE;
    sizes[POLARIS_MEMORY_FENCE] = POLARIS_MEMORY_PAGE;
    if (round_page(r->firmware_bytes, &sizes[POLARIS_MEMORY_FIRMWARE]) ||
        round_page(r->staging_bytes, &sizes[POLARIS_MEMORY_STAGING])) return -1;
    cursor = r->arena.base;
    for (unsigned i = 0; i < POLARIS_MEMORY_OBJECTS; ++i) {
        uint64_t offset = cursor - r->arena.base;
        if (offset > r->arena.bytes || sizes[i] > r->arena.bytes - offset) return -1;
        p.object[i].gpu_address = cursor;
        p.object[i].aperture_offset = cursor - r->aperture.base;
        p.object[i].cpu_physical = r->aperture_cpu_base + p.object[i].aperture_offset;
        p.object[i].bytes = sizes[i];
        cursor += sizes[i];
    }
    p.bytes_used = cursor - r->arena.base;
    *out = p;
    return 0;
}
