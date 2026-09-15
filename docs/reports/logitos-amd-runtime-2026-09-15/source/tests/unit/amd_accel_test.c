#include "rv100_accel.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VRAM_BYTES (16u * 1024u * 1024u)
#define MMIO_BYTES 0x4000u
#define CNFG_MEMSIZE 0x00f8u
#define RBBM_STATUS 0x0e40u
#define SRC_PITCH_OFFSET 0x1428u
#define DST_PITCH_OFFSET 0x142cu
#define SRC_Y_X 0x1434u
#define DST_Y_X 0x1438u
#define DST_HEIGHT_WIDTH 0x143cu
#define DP_GUI_MASTER_CNTL 0x146cu
#define DP_BRUSH_FRGD_CLR 0x147cu
#define SC_LEFT 0x1640u
#define SC_RIGHT 0x1644u
#define SC_TOP 0x1648u
#define SC_BOTTOM 0x164cu
#define DP_CNTL 0x16c0u
#define DP_DATATYPE 0x16c4u
#define DP_MIX 0x16c8u
#define DP_WRITE_MASK 0x16ccu
#define DSTCACHE_CTLSTAT 0x1714u

#define RBBM_ACTIVE 0x80000000u
#define DP_LEFT_TO_RIGHT 1u
#define DP_TOP_TO_BOTTOM 2u

struct write_event { uint32_t off, value; };

struct mock {
    uint8_t *vram;
    uint32_t mmio[MMIO_BYTES / 4u];
    struct write_event writes[4096];
    unsigned nwrites;
    unsigned maps;
    unsigned bar0_maps;
    unsigned bar2_maps;
    unsigned triggers;
    unsigned canary_triggers;
    unsigned cache_flushes;
    unsigned barriers;
    uint64_t now;
    int ignore_command;
    int stick_engine_after_trigger;
    unsigned stick_cache_on_flush;
    int engine_stuck;
    int cache_stuck;
    int fifo_empty;
    int empty_fifo_on_upload;
};

static int checks, failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; \
} checks++; } while (0)

static uint32_t reg_get(const struct mock *m, uint32_t off)
{
    return off < MMIO_BYTES ? m->mmio[off / 4u] : UINT32_MAX;
}

static void reg_set(struct mock *m, uint32_t off, uint32_t value)
{
    if (off < MMIO_BYTES) m->mmio[off / 4u] = value;
}

static uint64_t map_bar(void *opaque, const struct rv100_device *dev, int bar)
{
    struct mock *m = opaque;
    (void)dev;
    m->maps++;
    if (bar == 0) { m->bar0_maps++; return 0x10000000ull; }
    if (bar == 2) { m->bar2_maps++; return 0x20000000ull; }
    return 0;
}

static uint32_t mmio_read32(void *opaque, uint64_t base, uint32_t off)
{
    struct mock *m = opaque;
    (void)base;
    if (off == RBBM_STATUS) {
        if (m->engine_stuck) return RBBM_ACTIVE;
        return m->fifo_empty ? 0u : 64u;
    }
    if (off == DSTCACHE_CTLSTAT && m->cache_stuck) return 0x80000000u;
    return reg_get(m, off);
}

static void execute_blt(struct mock *m)
{
    if (m->ignore_command || m->engine_stuck) return;
    uint32_t master = reg_get(m, DP_GUI_MASTER_CNTL);
    uint32_t direction = reg_get(m, DP_CNTL);
    uint32_t shape = reg_get(m, DST_HEIGHT_WIDTH);
    uint32_t width = shape & 0x3fffu;
    uint32_t height = (shape >> 16) & 0x3fffu;
    uint32_t dpo = reg_get(m, DST_PITCH_OFFSET);
    uint32_t spo = reg_get(m, SRC_PITCH_OFFSET);
    uint32_t dst_base = (dpo & 0x3fffffu) << 10;
    uint32_t src_base = (spo & 0x3fffffu) << 10;
    uint32_t dst_pitch = (dpo & 0x3fc00000u) >> 16;
    uint32_t src_pitch = (spo & 0x3fc00000u) >> 16;
    uint32_t dxy = reg_get(m, DST_Y_X), sxy = reg_get(m, SRC_Y_X);
    uint32_t dx0 = dxy & 0x3fffu, dy0 = (dxy >> 16) & 0x3fffu;
    uint32_t sx0 = sxy & 0x3fffu, sy0 = (sxy >> 16) & 0x3fffu;
    int xstep = (direction & DP_LEFT_TO_RIGHT) ? 1 : -1;
    int ystep = (direction & DP_TOP_TO_BOTTOM) ? 1 : -1;

    for (uint32_t yi = 0; yi < height; yi++) {
        uint32_t dy = (uint32_t)((int)dy0 + (int)yi * ystep);
        uint32_t sy = (uint32_t)((int)sy0 + (int)yi * ystep);
        for (uint32_t xi = 0; xi < width; xi++) {
            uint32_t dx = (uint32_t)((int)dx0 + (int)xi * xstep);
            uint32_t sx = (uint32_t)((int)sx0 + (int)xi * xstep);
            uint32_t doff = dst_base + dy * dst_pitch + dx * 4u;
            uint32_t value;
            if ((master & 0x00ff0000u) == 0x00f00000u)
                value = reg_get(m, DP_BRUSH_FRGD_CLR);
            else
                memcpy(&value, m->vram + src_base + sy * src_pitch + sx * 4u, 4);
            memcpy(m->vram + doff, &value, 4);
        }
    }
}

static void mmio_write32(void *opaque, uint64_t base, uint32_t off, uint32_t value)
{
    struct mock *m = opaque;
    (void)base;
    if (m->nwrites < sizeof m->writes / sizeof m->writes[0])
        m->writes[m->nwrites++] = (struct write_event){off, value};
    if (off == DP_GUI_MASTER_CNTL) {
        /* Match QEMU's destructive aliases so the fixture detects restoring
         * MASTER after the direct pitch/scissor state. */
        if (!(value & 1u)) reg_set(m, SRC_PITCH_OFFSET, 0u);
        if (!(value & 2u)) reg_set(m, DST_PITCH_OFFSET, 0u);
        if (!(value & 8u)) {
            reg_set(m, SC_LEFT, 0u); reg_set(m, SC_TOP, 0u);
            reg_set(m, SC_RIGHT, 0u); reg_set(m, SC_BOTTOM, 0u);
        }
    }
    reg_set(m, off, value);
    if (off == DSTCACHE_CTLSTAT) {
        m->cache_flushes++;
        if (m->stick_cache_on_flush == m->cache_flushes) m->cache_stuck = 1;
    }
    if (off == DST_HEIGHT_WIDTH) {
        m->triggers++;
        if (value == 0x0007000du) m->canary_triggers++;
        if (m->stick_engine_after_trigger) m->engine_stuck = 1;
        execute_blt(m);
    }
}

static uint32_t vram_read32(void *opaque, uint64_t base, uint32_t off)
{
    struct mock *m = opaque;
    uint32_t value = 0;
    (void)base;
    if (off <= VRAM_BYTES - 4u) memcpy(&value, m->vram + off, 4);
    return value;
}

static void vram_write32(void *opaque, uint64_t base, uint32_t off,
                         uint32_t value)
{
    struct mock *m = opaque;
    (void)base;
    if (off <= VRAM_BYTES - 4u) memcpy(m->vram + off, &value, 4);
    if (m->empty_fifo_on_upload) m->fifo_empty = 1;
}

static uint64_t now_ns(void *opaque)
{
    struct mock *m = opaque;
    m->now += 1000u;
    return m->now;
}

static void relax_cpu(void *opaque) { ((struct mock *)opaque)->now += 1000u; }
static void memory_barrier(void *opaque) { ((struct mock *)opaque)->barriers++; }

static void mock_init(struct mock *m)
{
    memset(m, 0, sizeof *m);
    m->vram = calloc(1, VRAM_BYTES);
    CHECK(m->vram != NULL);
    reg_set(m, CNFG_MEMSIZE, VRAM_BYTES);
    reg_set(m, SRC_PITCH_OFFSET, 0x00401234u);
    reg_set(m, DST_PITCH_OFFSET, 0x00805678u);
    reg_set(m, SRC_Y_X, 0x00110022u);
    reg_set(m, DST_Y_X, 0x00330044u);
    reg_set(m, DP_GUI_MASTER_CNTL, 0x50cc3600u);
    reg_set(m, DP_BRUSH_FRGD_CLR, UINT32_MAX); /* legal, must not reject MMIO */
    reg_set(m, SC_LEFT, 2u);
    reg_set(m, SC_RIGHT, 1300u);
    reg_set(m, SC_TOP, 3u);
    reg_set(m, SC_BOTTOM, 900u);
    reg_set(m, DP_CNTL, 2u);
    reg_set(m, DP_DATATYPE, 6u);
    reg_set(m, DP_MIX, 0x00cc0200u);
    reg_set(m, DP_WRITE_MASK, UINT32_MAX); /* all ones is the normal full mask */
}

static void mock_fini(struct mock *m) { free(m->vram); }

static struct rv100_device good_device(void)
{
    struct rv100_device d;
    memset(&d, 0, sizeof d);
    d.vendor = RV100_VENDOR_ID;
    d.device = RV100_DEVICE_ID;
    d.class_code = RV100_CLASS_DISPLAY;
    d.subclass = 0;
    d.header_type = 0;
    d.command = RV100_PCI_CMD_MEM;
    d.pm_cap = 0x40u;
    d.pmcsr = 0;
    d.bar[0] = (struct rv100_resource){0xf0000000ull, 128ull << 20, RV100_RES_MEM};
    d.bar[2] = (struct rv100_resource){0xfebf0000ull, 0x4000u, RV100_RES_MEM};
    return d;
}

static struct rv100_ops good_ops(struct mock *m)
{
    return (struct rv100_ops){
        .ctx = m, .map_bar = map_bar, .mmio_read32 = mmio_read32,
        .mmio_write32 = mmio_write32, .vram_read32 = vram_read32,
        .vram_write32 = vram_write32, .now_ns = now_ns,
        .relax = relax_cpu, .barrier = memory_barrier,
    };
}

static uint32_t word_at(struct mock *m, uint32_t off)
{
    uint32_t value;
    memcpy(&value, m->vram + off, 4);
    return value;
}

static void set_word(struct mock *m, uint32_t off, uint32_t value)
{
    memcpy(m->vram + off, &value, 4);
}

static void test_prepare_and_runtime(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);

    /* 1280x800x4: 5120-byte pitch and 4,096,000-byte scanout. */
    uint32_t scanout = 1280u * 800u * 4u;
    uint32_t src = (scanout + 1023u) & ~1023u;
    uint32_t dst = src + 1024u;
    for (uint32_t i = 0; i < 112u; i++) {
        set_word(&m, src + i * 4u, 0x11000000u + i);
        set_word(&m, dst + i * 4u, 0x22000000u + i);
    }

    CHECK(rv100_prepare(&c, &d, d.bar[0].start, scanout,
                        1280u, 800u, &ops) == 0);
    CHECK(c.info.stage == RV100_ACTIVE && c.info.canary_ready);
    CHECK(c.info.software_fallback == 1); /* compositor is not wired here */
    CHECK(c.info.fill_ok && c.info.copy_ok && c.info.restore_ok);
    CHECK(c.info.canary_src == src && c.info.canary_dst == dst);
    CHECK(m.maps == 2 && m.bar0_maps == 1 && m.bar2_maps == 1);
    CHECK(m.triggers == 2 && m.canary_triggers == 2);
    for (uint32_t i = 0; i < 112u; i++) {
        CHECK(word_at(&m, src + i * 4u) == 0x11000000u + i);
        CHECK(word_at(&m, dst + i * 4u) == 0x22000000u + i);
    }
    CHECK(reg_get(&m, DP_WRITE_MASK) == UINT32_MAX);
    CHECK(reg_get(&m, DP_BRUSH_FRGD_CLR) == UINT32_MAX);
    CHECK(reg_get(&m, SRC_PITCH_OFFSET) == 0x00401234u);
    CHECK(reg_get(&m, DST_PITCH_OFFSET) == 0x00805678u);

    CHECK(rv100_fill(&c, 4u, 3u, 5u, 2u, 0x12345678u) == 0);
    for (uint32_t y = 3; y < 5; y++)
        for (uint32_t x = 4; x < 9; x++)
            CHECK(word_at(&m, y * 5120u + x * 4u) == 0x12345678u);

    for (uint32_t x = 0; x < 16; x++) set_word(&m, x * 4u, x + 1u);
    CHECK(rv100_copy(&c, 2u, 0u, 0u, 0u, 8u, 1u) == 0);
    for (uint32_t x = 0; x < 8; x++) CHECK(word_at(&m, (x + 2u) * 4u) == x + 1u);

#ifndef RV100_NEGCTL_UNBOUNDED_SURFACE
    unsigned before = m.triggers;
#endif
    CHECK(rv100_fill(&c, 1279u, 0, 2u, 1u, 0) == -1);
#ifndef RV100_NEGCTL_UNBOUNDED_SURFACE
    CHECK(c.info.blocker == RV100_BAD_SURFACE && c.info.stage == RV100_ACTIVE);
    CHECK(m.triggers == before);
#endif
    mock_fini(&m);
}

static void test_modern_zero_map(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    d.device = 0x744cu; /* modern Navi; must never receive legacy RV100 MMIO */
    CHECK(rv100_prepare(&c, &d, d.bar[0].start, 4096000u,
                        1280u, 800u, &ops) == -1);
#ifndef RV100_NEGCTL_BROAD_ID
    CHECK(c.info.blocker == RV100_WRONG_DEVICE);
    CHECK(m.maps == 0 && m.nwrites == 0);
#endif
    mock_fini(&m);
}

static void test_validation_before_map(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    CHECK(rv100_prepare(&c, &d, d.bar[0].start + d.bar[0].size - 1024u,
                        4096000u, 1280u, 800u, &ops) == -1);
    CHECK(c.info.blocker == RV100_BAD_SCANOUT && m.maps == 0);
    d = good_device();
    CHECK(rv100_prepare(&c, &d, d.bar[0].start, 4000000u,
                        1280u, 800u, &ops) == -1); /* pitch 5000, not /64 */
    CHECK(c.info.blocker == RV100_BAD_SCANOUT && m.maps == 0);
    d = good_device();
    CHECK(rv100_prepare(&c, &d, d.bar[0].start,
                        5120ull * 8193ull, 1280u, 8193u, &ops) == -1);
    CHECK(c.info.blocker == RV100_BAD_SCANOUT && m.maps == 0);
    mock_fini(&m);
}

static void test_actual_vram_not_bar_aperture(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    reg_set(&m, CNFG_MEMSIZE, 2u << 20);
    CHECK(rv100_prepare(&c, &d, d.bar[0].start, 4096000u,
                        1280u, 800u, &ops) == -1);
    CHECK(c.info.blocker == RV100_BAD_VRAM_SIZE);
    CHECK(m.bar2_maps == 1 && m.bar0_maps == 0);
    mock_fini(&m);
}

static void test_safe_mismatch_restore(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    m.ignore_command = 1;
    CHECK(rv100_prepare(&c, &d, d.bar[0].start, 4096000u,
                        1280u, 800u, &ops) == -1);
    CHECK(c.info.blocker == RV100_FILL_MISMATCH);
    CHECK(c.info.restore_ok && !c.info.quarantined);
    for (uint32_t i = 0; i < 112u; i++) {
        CHECK(word_at(&m, c.info.canary_src + i * 4u) == 0);
        CHECK(word_at(&m, c.info.canary_dst + i * 4u) == 0);
    }
    mock_fini(&m);
}

static void test_timeout_quarantines_without_cpu_restore(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    m.stick_engine_after_trigger = 1;
    CHECK(rv100_prepare(&c, &d, d.bar[0].start, 4096000u,
                        1280u, 800u, &ops) == -1);
    CHECK(c.info.blocker == RV100_ENGINE_TIMEOUT);
    CHECK(c.info.quarantined && !c.info.restore_ok);
    /* A seed remains: the timeout path did not race the still-active engine. */
    CHECK(word_at(&m, c.info.canary_src) == 0xa5a5a5a5u);
    CHECK(word_at(&m, c.info.canary_dst) == 0x5a5a5a5au);
    mock_fini(&m);
}

static void test_cache_timeout_quarantines(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    /* The first prepare flush also writes this register, so arm the stuck cache
     * only after one ordinary initial flush in the write callback would be hard.
     * A permanently stuck cache instead proves the pre-seed refusal is write-free. */
    m.cache_stuck = 1;
    CHECK(rv100_prepare(&c, &d, d.bar[0].start, 4096000u,
                        1280u, 800u, &ops) == -1);
    CHECK(c.info.blocker == RV100_CACHE_TIMEOUT);
    CHECK(!c.info.quarantined && c.info.vram_writes == 0);
    mock_fini(&m);
}

static void test_post_command_cache_timeout_quarantines(void)
{
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    m.stick_cache_on_flush = 2u; /* initial clean succeeds; fill flush sticks */
    CHECK(rv100_prepare(&c, &d, d.bar[0].start, 4096000u,
                        1280u, 800u, &ops) == -1);
    CHECK(c.info.blocker == RV100_CACHE_TIMEOUT);
    CHECK(c.info.quarantined && !c.info.restore_ok && m.triggers == 1u);
    CHECK(word_at(&m, c.info.canary_src) == 0xc001d00du);
    CHECK(word_at(&m, c.info.canary_dst) == 0x5a5a5a5au);
    mock_fini(&m);
}

#if !defined(RV100_NEGCTL_BROAD_ID) && !defined(RV100_NEGCTL_UNBOUNDED_SURFACE)
/* The oracle addresses a padded, nonzero-offset scanout directly and compares
 * the entire surrounding allocation.  It does not decode the driver's pitch
 * helper or derive expected pixels from its MMIO stream. */
static void test_present_pixels(void)
{
    enum { W = 23, H = 9, PITCH = 128, STRIDE = 29, FRONT = 4096 };
    struct mock m;
    struct rv100_context c;
    struct rv100_device d = good_device();
    mock_init(&m);
    struct rv100_ops ops = good_ops(&m);
    uint32_t pixels[STRIDE * H];
    uint8_t expected[FRONT + PITCH * H + 1024];
    for (unsigned i = 0; i < STRIDE * H; i++) pixels[i] = 0xf0a50000u ^ (i * 7231u);
    CHECK(rv100_prepare(&c, &d, d.bar[0].start + FRONT,
                        PITCH * H, W, H, &ops) == 0);
    CHECK(c.info.staging_bytes == PITCH * H && !(c.info.staging_offset & 1023u));
    CHECK(c.info.staging_offset >= c.info.canary_dst + 1024u);
    memset(m.vram, 0x6a, sizeof expected);
    memset(expected, 0x6a, sizeof expected);
    for (unsigned y = 2; y < 7; y++)
        for (unsigned x = 3; x < 20; x++)
            memcpy(expected + FRONT + y * PITCH + x * 4u,
                   &pixels[y * STRIDE + x], 4u);
    unsigned triggers = m.triggers;
    (void)triggers;
    int rc = rv100_present(&c, pixels, STRIDE, 3, 2, 17, 5);
#ifdef RV100_NEGCTL_PRESENT_CORRUPT
    /* One intentional failure demonstrates this image oracle is reached. */
    CHECK(rc == 0 && !memcmp(m.vram, expected, sizeof expected));
    mock_fini(&m);
    return;
#else
    CHECK(rc == 0 && !memcmp(m.vram, expected, sizeof expected));
    CHECK(c.info.present_verified && c.info.presents == 1 && c.info.copy_presents == 1);
    CHECK(c.info.uploaded_bytes == 17u * 5u * 4u && c.info.gpu_pixels == 17u * 5u);
    CHECK(m.triggers == triggers + 1u);
    CHECK(reg_get(&m, SRC_PITCH_OFFSET) != reg_get(&m, DST_PITCH_OFFSET));
    int staging_ok = 1;
    for (unsigned y = 2; y < 7; y++)
        for (unsigned x = 3; x < 20; x++)
            if (word_at(&m, c.info.staging_offset + y * PITCH + x * 4u) !=
                pixels[y * STRIDE + x]) staging_ok = 0;
    CHECK(staging_ok);
    unsigned reads = c.info.vram_reads;
    for (unsigned i = 0; i < STRIDE * H; i++) pixels[i] = 0x80123456u;
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++)
            memcpy(expected + FRONT + y * PITCH + x * 4u, pixels, 4u);
    CHECK(rv100_present(&c, pixels, STRIDE, 0, 0, W, H) == 0);
    CHECK(!memcmp(m.vram, expected, sizeof expected));
    CHECK(c.info.solid_presents == 1 && c.info.presents == 2 &&
          c.info.uploaded_bytes == 340 && c.info.vram_reads == reads);
    CHECK(c.info.gpu_pixels == 85 + W * H);
    triggers = m.triggers;
    CHECK(rv100_present(&c, pixels, W - 1u, 0, 0, 1, 1) == -1);
    CHECK(rv100_present(&c, pixels, STRIDE, W, 0, 1, 1) == -1);
    CHECK(rv100_present(&c, pixels, STRIDE, 0, H, 1, 1) == -1);
    CHECK(rv100_present(&c, pixels, STRIDE, UINT32_MAX, 0, 2, 1) == -1);
    CHECK(rv100_present(&c, pixels, STRIDE, 0, 0, 0, 1) == -1);
    CHECK(rv100_present(&c, NULL, STRIDE, 0, 0, 1, 1) == -1);
    CHECK(rv100_present(&c, (const uint32_t *)(uintptr_t)(UINTPTR_MAX - 3u),
                        STRIDE, 0, 1, 1, 1) == -1);
    CHECK(m.triggers == triggers && !memcmp(m.vram, expected, sizeof expected));
    c.lock = 1;
    struct rv100_info old_info = c.info;
    CHECK(rv100_present(&c, pixels, STRIDE, 0, 0, 1, 1) == -2);
    CHECK(rv100_fill(&c, 0, 0, 1, 1, 0) == -1);
    CHECK(rv100_copy(&c, 0, 0, 1, 1, 1, 1) == -1);
    CHECK(!memcmp(&old_info, &c.info, sizeof old_info) && m.triggers == triggers);
    c.lock = 0;
    c.info.stage = RV100_BLOCKED;
    CHECK(rv100_present(&c, pixels, STRIDE, 0, 0, 1, 1) == -1);
    CHECK(rv100_fill(&c, 0, 0, 1, 1, 0) == -1);
    CHECK(rv100_copy(&c, 0, 0, 1, 1, 1, 1) == -1);
    CHECK(c.lock == 0 && m.triggers == triggers);
    mock_fini(&m);
#endif
}

#ifndef RV100_NEGCTL_PRESENT_CORRUPT
static void test_present_failure_contract(void)
{
    uint32_t pixels[32 * 8];
    for (unsigned i = 0; i < 32 * 8; i++) pixels[i] = i + 31u;
    for (unsigned scenario = 0; scenario < 7; scenario++) {
        struct mock m;
        struct rv100_context c;
        struct rv100_device d = good_device();
        mock_init(&m);
        struct rv100_ops ops = good_ops(&m);
        /* Enough diagnostic scratch but no full-screen staging. */
        if (scenario == 0) reg_set(&m, CNFG_MEMSIZE, 1024u + 2048u);
        CHECK(rv100_prepare(&c, &d, d.bar[0].start, 1024, 32, 8, &ops) == 0);
        unsigned writes = c.info.vram_writes, triggers = m.triggers;
        if (scenario == 1) m.engine_stuck = 1;
        if (scenario == 2) m.cache_stuck = 1;
        if (scenario == 3) m.stick_engine_after_trigger = 1;
        if (scenario == 4) m.stick_cache_on_flush = m.cache_flushes + 2u;
        if (scenario == 5) m.ignore_command = 1;
        if (scenario == 6) m.empty_fifo_on_upload = 1;
        int rc = rv100_present(&c, pixels, 32, 0, 0, 32, 8);
        CHECK(rc == ((scenario == 0 || scenario >= 5) ? -1 : -2));
        CHECK(!c.info.present_verified && c.info.presents == 0 && c.lock == 0);
        if (scenario < 3) CHECK(c.info.vram_writes == writes && m.triggers == triggers);
        else CHECK(c.info.uploaded_bytes == 1024 &&
                   m.triggers == triggers + (scenario == 6 ? 0u : 1u));
        if (scenario == 0) CHECK(!c.info.staging_bytes && !c.info.quarantined);
        else if (scenario >= 5) CHECK(c.info.stage == RV100_BLOCKED && !c.info.quarantined);
        else CHECK(c.info.stage == RV100_BLOCKED && c.info.quarantined);
        writes = c.info.vram_writes; triggers = m.triggers;
        CHECK(rv100_present(&c, pixels, 32, 0, 0, 32, 8) == rc);
        CHECK(c.info.vram_writes == writes && m.triggers == triggers);
        mock_fini(&m);
    }
}

#endif

#endif

int main(void)
{
    test_prepare_and_runtime();
    test_modern_zero_map();
    test_validation_before_map();
    test_actual_vram_not_bar_aperture();
    test_safe_mismatch_restore();
    test_timeout_quarantines_without_cpu_restore();
    test_cache_timeout_quarantines();
    test_post_command_cache_timeout_quarantines();
#if !defined(RV100_NEGCTL_BROAD_ID) && !defined(RV100_NEGCTL_UNBOUNDED_SURFACE)
    test_present_pixels();
#ifndef RV100_NEGCTL_PRESENT_CORRUPT
    test_present_failure_contract();
#endif
#endif
    printf("AMD_RV100: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
