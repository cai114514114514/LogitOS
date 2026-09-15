#include "amd/polaris/memory/layout.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static struct polaris_memory_request good(void)
{
    struct polaris_memory_request r = {0};
    r.vram = (struct polaris_memory_range){UINT64_C(0x800000000), UINT64_C(0x200000000)};
    r.aperture = (struct polaris_memory_range){UINT64_C(0x800000000), 0x10000000};
    r.aperture_cpu_base = 0xd0000000;
    r.arena = (struct polaris_memory_range){UINT64_C(0x801000000), 0x2000000};
    r.scanout = (struct polaris_memory_range){UINT64_C(0x800000000), 0x400000};
    r.firmware_bytes = 0x2001;
    r.staging_bytes = 0x200001;
    return r;
}
static void reject(struct polaris_memory_request r)
{
    struct polaris_memory_layout out, before;
    memset(&out, 0xa6, sizeof(out)); before = out;
    CHECK(polaris_memory_plan(&r, &out) == -1);
    CHECK(memcmp(&out, &before, sizeof(out)) == 0);
}
int main(void)
{
    struct polaris_memory_request r = good();
    struct polaris_memory_layout out;
    const uint64_t offsets[] = {0, 0x1000, 0xc9000, 0xcc000, 0xcd000, 0xce000};
    const uint64_t sizes[] = {0x1000, 0xc8000, 0x3000, 0x1000, 0x1000, 0x201000};
    CHECK(polaris_memory_plan(&r, &out) == 0);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(out.object[i].gpu_address == UINT64_C(0x801000000) + offsets[i]);
        CHECK(out.object[i].cpu_physical == UINT64_C(0xd1000000) + offsets[i]);
        CHECK(out.object[i].aperture_offset == UINT64_C(0x1000000) + offsets[i]);
        CHECK(out.object[i].bytes == sizes[i]);
    }
    CHECK(out.bytes_used == 0x2cf000);
    r.arena.bytes = 0x2cf000; CHECK(polaris_memory_plan(&r, &out) == 0);
    --r.arena.bytes; reject(r);
    r = good(); r.scanout = r.arena;
    /* This assertion is the mutation oracle: allowing scanout to be used for
     * firmware/ring storage must visibly fail even if all addresses align. */
    CHECK(polaris_memory_plan(&r, &out) == -1);
    r = good(); r.arena.base += 1; reject(r);
    r = good(); r.arena.base = r.aperture.base + r.aperture.bytes; reject(r);
    r = good(); r.arena.bytes = UINT64_MAX; reject(r);
    r = good(); r.aperture.bytes = UINT64_MAX; reject(r);
    r = good(); ++r.aperture.base; reject(r);
    r = good(); r.vram.base = UINT64_C(1) << 40; reject(r);
    r = good(); r.vram.bytes = (UINT64_C(1) << 40); reject(r);
    r = good(); r.scanout.bytes = 0; reject(r);
    r = good(); r.aperture_cpu_base = UINT64_MAX - 4095; reject(r);
    r = good(); ++r.aperture_cpu_base; reject(r);
    r = good(); r.firmware_bytes = UINT64_MAX; reject(r);
    r = good(); r.firmware_bytes = 0; reject(r);
    r = good(); r.staging_bytes = UINT64_MAX; reject(r);
    r = good(); r.staging_bytes = 0; reject(r);
    r = good(); r.reserved_count = 1; reject(r);
    r = good(); r.reserved_count = 65; reject(r);
    struct polaris_memory_range reserved = {UINT64_C(0x80f000000), 0x100000};
    r = good(); r.reserved = &reserved; r.reserved_count = 1;
    CHECK(polaris_memory_plan(&r, &out) == 0);
    reserved = r.arena; reject(r);
    reserved.base = r.arena.base - 1; reserved.bytes = 2; reject(r);
    reserved.base = r.arena.base + r.arena.bytes - 1; reject(r);
    reserved.base = r.arena.base + r.arena.bytes; reserved.bytes = 1;
    CHECK(polaris_memory_plan(&r, &out) == 0);
    reserved.base = 0; reject(r);
    reserved.base = r.vram.base; reserved.bytes = 0; reject(r);
    CHECK(polaris_memory_plan(0, &out) == -1);
    CHECK(polaris_memory_plan(&r, 0) == -1);
    printf("POLARIS_MEMORY_LAYOUT: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
