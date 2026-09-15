#include "amd/polaris/device.h"
#include "driver.h"
#include "pci.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* Literal byte-offset oracle is independent of the probe's private constants.
 * These are Linux v6.12 generated GMC 8.1 / OSS 3.0 DWORD indices multiplied
 * by four. Returning by expected access position makes a wrong register fail
 * even if the implementation happens to assign it a plausible value. */
static const uint32_t offsets[8] = {
    0x5428, 0x2024, 0x0e4c, 0x1410, 0xd048, 0xd848, 0xd200, 0xda00
};
struct fixture {
    struct device dev;
    struct polaris_probe_ops ops;
    struct polaris_info out;
    uint64_t mapped, lfb, bytes;
    uint32_t width, height, values[8], seen[8];
    uint16_t command, power;
    unsigned maps, reads, bad_access;
    int bar;
};

static uint64_t map(void *arg, int bar)
{
    struct fixture *f = arg;
    ++f->maps; f->bar = bar;
    if (bar != 5) ++f->bad_access;
    return f->mapped;
}

static uint32_t read_reg(void *arg, uint64_t base, uint32_t off)
{
    struct fixture *f = arg;
    unsigned n = f->reads++;
    if (base != f->mapped || n >= 8 || off != offsets[n] ||
        (uint64_t)off + 4 > f->dev.res[5].size) ++f->bad_access;
    if (n >= 8) return UINT32_MAX;
    f->seen[n] = off;
    return f->values[n];
}

static void init(struct fixture *f)
{
    *f = (struct fixture){0};
    f->dev.bus_type = DEV_BUS_PCI; f->dev.vendor = 0x1002;
    f->dev.device = 0x67df; f->dev.class_code = PCI_CLASS_DISPLAY;
    f->dev.res[0] = (struct dev_resource){0xe0000000, 0x10000000,
        DEV_RES_MEM | DEV_RES_64 | DEV_RES_PREFETCH};
    f->dev.res[5] = (struct dev_resource){0xf1000000, 0x40000, DEV_RES_MEM};
    f->ops = (struct polaris_probe_ops){f, map, read_reg};
    f->command = PCI_CMD_MEM; f->mapped = 0x10000000;
    f->lfb = 0xe0100000; f->width = 1280; f->height = 720;
    f->bytes = (uint64_t)f->width * f->height * 4;
    f->values[0] = 8192; f->values[1] = 0x41ff4000;
    for (unsigned i = 2; i < 8; ++i) f->values[i] = 0x100 + i;
    memset(&f->out, 0xa5, sizeof(f->out));
}

static int run(struct fixture *f)
{
    return polaris_probe_readonly(&f->dev, f->command, f->power, f->lfb,
        f->bytes, f->width, f->height, &f->ops, &f->out);
}

static void refuse(struct fixture *f, enum polaris_probe_result reason)
{
    CHECK(run(f) == -1 && f->out.result == reason && !f->maps && !f->reads &&
          !f->out.vram_bytes && !f->out.layout_covers_vram);
}

int main(void)
{
    struct fixture f;
    init(&f);
    CHECK(run(&f) == 0 && f.out.result == POLARIS_PROBE_OBSERVED);
    CHECK(f.maps == 1 && f.bar == 5 && f.reads == 8 && !f.bad_access &&
          f.out.mmio_reads == 8 && !memcmp(f.seen, offsets, sizeof(offsets)));
    CHECK(f.out.vram_bytes == (8ull << 30) && f.out.cpu_aperture_bytes == (256ull << 20));
    CHECK(f.out.mc_base == (256ull << 30) && f.out.mc_end == (264ull << 30) &&
          f.out.layout_covers_vram == 1);
    CHECK(f.out.srbm_status2 == 0x102 && f.out.vm_context0 == 0x103 &&
          f.out.sdma_f32[0] == 0x104 && f.out.sdma_f32[1] == 0x105 &&
          f.out.sdma_ring[0] == 0x106 && f.out.sdma_ring[1] == 0x107);
    init(&f); f.values[0] = 4096; f.values[1] = 0x40ff4000;
    CHECK(run(&f) == 0 && f.out.vram_bytes == (4ull << 30) &&
          f.out.cpu_aperture_bytes == (256ull << 20) && f.out.layout_covers_vram);
    init(&f); f.values[1] = 0x400f4000;
    CHECK(run(&f) == 0 && f.out.result == POLARIS_PROBE_OBSERVED &&
          !f.out.layout_covers_vram);
    init(&f); f.values[1] = 0x3fff4000;
    CHECK(run(&f) == 0 && !f.out.layout_covers_vram);
    init(&f); f.values[1] = 0x3ffe4000;
    CHECK(run(&f) == 0 && !f.out.layout_covers_vram);
    init(&f); f.dev.subclass = 2;
    CHECK(run(&f) == 0 && f.reads == 8 && !f.bad_access);
    init(&f); f.power = 0x8000; f.command |= PCI_CMD_MASTER;
    CHECK(run(&f) == 0 && f.reads == 8);
    init(&f);
    CHECK(polaris_probe_readonly(NULL, 2, 0, f.lfb, f.bytes, f.width,
          f.height, &f.ops, &f.out) == -1 && f.out.result == POLARIS_PROBE_ID && !f.maps);
    CHECK(polaris_probe_readonly(&f.dev, 2, 0, f.lfb, f.bytes, f.width,
          f.height, &f.ops, NULL) == -1 && !f.maps);

    init(&f); f.dev.vendor = 0x10de; refuse(&f, POLARIS_PROBE_ID);
    init(&f); f.dev.device = 0x5159; refuse(&f, POLARIS_PROBE_ID);
    init(&f); f.dev.device = 0x67ef; refuse(&f, POLARIS_PROBE_ID);
    init(&f); f.dev.bus_type = DEV_BUS_PLATFORM; refuse(&f, POLARIS_PROBE_ID);
    init(&f); f.dev.class_code = PCI_CLASS_STORAGE; refuse(&f, POLARIS_PROBE_ID);
    init(&f); f.dev.subclass = 1; refuse(&f, POLARIS_PROBE_ID);
    init(&f); f.dev.header_type = 1; refuse(&f, POLARIS_PROBE_ID);
    init(&f); f.command = 0; refuse(&f, POLARIS_PROBE_DECODE);
    init(&f); f.command = PCI_CMD_MASTER; refuse(&f, POLARIS_PROBE_DECODE);
    init(&f); f.command = UINT16_MAX; refuse(&f, POLARIS_PROBE_DECODE);
    for (unsigned p = 1; p <= 3; ++p) {
        init(&f); f.power = (uint16_t)p; refuse(&f, POLARIS_PROBE_POWER);
    }
    init(&f); f.power = UINT16_MAX; refuse(&f, POLARIS_PROBE_POWER);

    const unsigned bars[] = {0, 5};
    for (unsigned i = 0; i < 2; ++i) {
        unsigned b = bars[i];
        init(&f); f.dev.res[b].flags = DEV_RES_IO; refuse(&f, POLARIS_PROBE_BAR);
        init(&f); f.dev.res[b].flags |= DEV_RES_IO; refuse(&f, POLARIS_PROBE_BAR);
        init(&f); f.dev.res[b].flags = 0; refuse(&f, POLARIS_PROBE_BAR);
        init(&f); f.dev.res[b].start = 0; refuse(&f, POLARIS_PROBE_BAR);
        init(&f); f.dev.res[b].size = 0; refuse(&f, POLARIS_PROBE_BAR);
        init(&f); f.dev.res[b].start++; refuse(&f, POLARIS_PROBE_BAR);
        init(&f); f.dev.res[b].size--; refuse(&f, POLARIS_PROBE_BAR);
        init(&f); f.dev.res[b].start = UINT64_MAX - 4095; refuse(&f, POLARIS_PROBE_BAR);
    }
    init(&f); f.dev.res[5].size = 0xd000; refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[5].size = 0x101000; refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[5].flags |= DEV_RES_64; refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[4].flags = DEV_RES_MEM | DEV_RES_64;
    refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[5].start = 0xffff0000; refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[0].flags &= ~DEV_RES_64;
    f.dev.res[0].start = 0xffff0000; refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[5].start = f.dev.res[0].start; refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[5].start = f.dev.res[0].start - 4096; refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[5].start = f.dev.res[0].start + f.dev.res[0].size - 4096;
    refuse(&f, POLARIS_PROBE_BAR);
    init(&f); f.dev.res[5].size = 0xe000;
    CHECK(run(&f) == 0 && f.reads == 8 && !f.bad_access);
    init(&f); f.dev.res[5].size = 0x100000;
    CHECK(run(&f) == 0 && f.reads == 8 && !f.bad_access);
    init(&f); f.dev.res[5].start = f.dev.res[0].start + f.dev.res[0].size;
    CHECK(run(&f) == 0 && f.reads == 8 && !f.bad_access);
    init(&f); f.dev.res[0].start = 1ull << 32; f.lfb = f.dev.res[0].start;
    CHECK(run(&f) == 0 && f.reads == 8 && !f.bad_access);

    init(&f); f.lfb = f.dev.res[0].start - 4; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.lfb = f.dev.res[0].start + f.dev.res[0].size - f.bytes + 4;
    refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.lfb = UINT64_MAX - 3; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.bytes--; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.bytes = (uint64_t)(f.width * 4 - 4) * f.height;
    refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.bytes = (uint64_t)(f.width * 4 + 1) * f.height;
    refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.width = 0; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.height = 0; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.width = 16385; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.height = UINT32_MAX; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.width = 16384; f.height = 16384;
    f.bytes = (uint64_t)f.width * f.height * 4; refuse(&f, POLARIS_PROBE_SURFACE);
    init(&f); f.lfb = f.dev.res[0].start + f.dev.res[0].size - f.bytes;
    CHECK(run(&f) == 0 && f.reads == 8);
    init(&f); f.bytes = (uint64_t)(f.width * 4 + 256) * f.height;
    CHECK(run(&f) == 0 && f.out.result == POLARIS_PROBE_OBSERVED && f.reads == 8);
    init(&f); f.bytes = (uint64_t)(f.width * 4 + 256) * f.height;
    f.lfb = f.dev.res[0].start + f.dev.res[0].size - f.bytes + 4;
    refuse(&f, POLARIS_PROBE_SURFACE);

    init(&f); f.ops.map_bar = NULL; refuse(&f, POLARIS_PROBE_MAP);
    init(&f); f.ops.read32 = NULL; refuse(&f, POLARIS_PROBE_MAP);
    init(&f);
    CHECK(polaris_probe_readonly(&f.dev, 2, 0, f.lfb, f.bytes, f.width,
          f.height, NULL, &f.out) == -1 && f.out.result == POLARIS_PROBE_MAP && !f.maps);
    const uint64_t bad_maps[] = {0, 1, UINTPTR_MAX - 3};
    for (unsigned i = 0; i < 3; ++i) {
        init(&f); f.mapped = bad_maps[i];
        CHECK(run(&f) == -1 && f.out.result == POLARIS_PROBE_MAP &&
              f.maps == 1 && !f.reads && !f.out.layout_covers_vram);
    }
    for (unsigned i = 0; i < 8; ++i) {
        init(&f); f.values[i] = UINT32_MAX;
        CHECK(run(&f) == -1 && f.out.result == POLARIS_PROBE_READ &&
              f.maps == 1 && f.reads == i + 1 && f.out.mmio_reads == i + 1 &&
              !f.bad_access && !f.out.layout_covers_vram);
    }
    const uint32_t bad_memory[] = {0, 127, 16385};
    for (unsigned i = 0; i < 3; ++i) {
        init(&f); f.values[0] = bad_memory[i];
        CHECK(run(&f) == -1 && f.out.result == POLARIS_PROBE_READ && f.reads == 8 &&
              !f.out.vram_bytes && !f.out.layout_covers_vram);
    }
    CHECK(!strcmp(polaris_probe_result_name(POLARIS_PROBE_OBSERVED), "registers-observed"));
    CHECK(!strcmp(polaris_probe_result_name((enum polaris_probe_result)99), "unknown"));
    printf("POLARIS_PROBE: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
