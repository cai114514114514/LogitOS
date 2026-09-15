#include "amd/rv100/engine.h"

#include <stddef.h>
#include <stdint.h>

/* Radeon RV100 MMIO register subset.  Values are pinned to Linux radeon_reg.h
 * at 587858367581b9c55c3690f4e63382ad622719d4 and QEMU 11 ati_regs.h. */
#define CNFG_MEMSIZE             0x00f8u
#define RBBM_STATUS              0x0e40u
#define SRC_PITCH_OFFSET         0x1428u
#define DST_PITCH_OFFSET         0x142cu
#define SRC_Y_X                  0x1434u
#define DST_Y_X                  0x1438u
#define DST_HEIGHT_WIDTH         0x143cu
#define DP_GUI_MASTER_CNTL       0x146cu
#define DP_BRUSH_FRGD_CLR        0x147cu
#define SC_LEFT                  0x1640u
#define SC_RIGHT                 0x1644u
#define SC_TOP                   0x1648u
#define SC_BOTTOM                0x164cu
#define DP_CNTL                  0x16c0u
#define DP_DATATYPE              0x16c4u
#define DP_MIX                   0x16c8u
#define DP_WRITE_MASK            0x16ccu
#define DSTCACHE_CTLSTAT         0x1714u

#define RBBM_FIFOCNT_MASK        0x0000007fu
#define RBBM_ACTIVE              0x80000000u
#define RB2D_DC_FLUSH_ALL        0x0000000fu
#define RB2D_DC_BUSY             0x80000000u

#define GMC_SRC_PITCH_OFFSET     0x00000001u
#define GMC_DST_PITCH_OFFSET     0x00000002u
#define GMC_SRC_CLIPPING         0x00000004u
#define GMC_DST_CLIPPING         0x00000008u
#define GMC_BRUSH_SOLID          0x000000d0u
#define GMC_BRUSH_NONE           0x000000f0u
#define GMC_DST_32BPP            0x00000600u
#define GMC_SRC_COLOR            0x00003000u
#define ROP3_SRCCOPY             0x00cc0000u
#define ROP3_PATCOPY             0x00f00000u
#define DP_SRC_MEMORY            0x02000000u
#define GMC_CLR_CMP_DIS          0x10000000u
#define GMC_WR_MSK_DIS           0x40000000u

#define DP_LEFT_TO_RIGHT         0x00000001u
#define DP_TOP_TO_BOTTOM         0x00000002u

#define CANARY_W                 13u
#define CANARY_H                 7u
#define CANARY_PITCH             64u
#define CANARY_BYTES             (CANARY_H * CANARY_PITCH)
#define CANARY_WORDS             (CANARY_BYTES / 4u)
#define CANARY_BLOCK             1024u
#define CANARY_FILL              0xc001d00du
#define CANARY_SRC_SEED          0xa5a5a5a5u
#define CANARY_DST_SEED          0x5a5a5a5au
#define CANARY_TRIGGER           0x0007000du

#define WAIT_NS                  5000000ull
#define WAIT_POLLS               100000u

struct saved_regs {
    uint32_t src_pitch_offset;
    uint32_t dst_pitch_offset;
    uint32_t src_y_x;
    uint32_t dst_y_x;
    uint32_t gui_master;
    uint32_t brush_frgd;
    uint32_t sc_left;
    uint32_t sc_right;
    uint32_t sc_top;
    uint32_t sc_bottom;
    uint32_t dp_cntl;
    uint32_t dp_datatype;
    uint32_t dp_mix;
    uint32_t dp_write_mask;
};

static void bytes_zero(void *ptr, size_t bytes)
{
    uint8_t *p = ptr;
    while (bytes--) *p++ = 0;
}

static int fail(struct rv100_context *ctx, enum rv100_blocker why)
{
    ctx->info.stage = RV100_BLOCKED;
    ctx->info.blocker = why;
    ctx->info.software_fallback = 1;
    return -1;
}

static int sane_mem_bar(const struct rv100_resource *r, uint64_t minimum)
{
    return r && (r->flags & RV100_RES_MEM) && !(r->flags & RV100_RES_IO) &&
           r->start && r->size >= minimum && r->size <= UINT64_MAX - r->start;
}

static int contains(uint64_t base, uint64_t size, uint64_t pos, uint64_t len)
{
    if (pos < base || len > size) return 0;
    return pos - base <= size - len;
}

static uint32_t mmio_read(struct rv100_context *c, uint32_t off)
{
    c->info.mmio_reads++;
    return c->ops.mmio_read32(c->ops.ctx, c->mmio_base, off);
}

static void mmio_write(struct rv100_context *c, uint32_t off, uint32_t value)
{
    c->info.mmio_writes++;
    c->ops.mmio_write32(c->ops.ctx, c->mmio_base, off, value);
}

static uint32_t vram_read(struct rv100_context *c, uint32_t off)
{
    c->info.vram_reads++;
    return c->ops.vram_read32(c->ops.ctx, c->vram_base, off);
}

static void vram_write(struct rv100_context *c, uint32_t off, uint32_t value)
{
    c->info.vram_writes++;
    c->ops.vram_write32(c->ops.ctx, c->vram_base, off, value);
}

static void barrier(struct rv100_context *c)
{
    if (c->ops.barrier) c->ops.barrier(c->ops.ctx);
}

static int wait_status(struct rv100_context *c, uint32_t fifo,
                       uint32_t forbidden)
{
    uint64_t begin = c->ops.now_ns(c->ops.ctx);
    for (uint32_t i = 0; i < WAIT_POLLS; i++) {
        uint32_t status = mmio_read(c, RBBM_STATUS);
        if (status != UINT32_MAX &&
            (status & RBBM_FIFOCNT_MASK) >= fifo && !(status & forbidden))
            return 0;
        if (c->ops.now_ns(c->ops.ctx) - begin >= WAIT_NS) return -1;
        if (c->ops.relax) c->ops.relax(c->ops.ctx);
    }
    return -1; /* A stalled/broken clock cannot make this loop unbounded. */
}

static int wait_fifo(struct rv100_context *c, uint32_t entries)
{
    return wait_status(c, entries, 0);
}

static int wait_idle(struct rv100_context *c)
{
    return wait_status(c, 64u, RBBM_ACTIVE);
}

static int flush_dstcache(struct rv100_context *c)
{
    if (wait_fifo(c, 1u)) return -1;
    mmio_write(c, DSTCACHE_CTLSTAT, RB2D_DC_FLUSH_ALL);
    uint64_t begin = c->ops.now_ns(c->ops.ctx);
    for (uint32_t i = 0; i < WAIT_POLLS; i++) {
        uint32_t status = mmio_read(c, DSTCACHE_CTLSTAT);
        /* QEMU 11 does not implement 0x1714 and returns zero.  Real RV100
         * exposes RB2D_DC_BUSY at bit 31.  All-ones is an absent/broken BAR. */
        if (status != UINT32_MAX && !(status & RB2D_DC_BUSY)) return 0;
        if (c->ops.now_ns(c->ops.ctx) - begin >= WAIT_NS) return -1;
        if (c->ops.relax) c->ops.relax(c->ops.ctx);
    }
    return -1;
}

static uint32_t pitch_offset(uint32_t offset, uint32_t pitch)
{
    /* RV100 stores offset in 1 KiB units and pitch in 64-byte units at bit 22. */
    return (offset >> 10) | ((pitch >> 6) << 22);
}

static void save_regs(struct rv100_context *c, struct saved_regs *r)
{
    r->src_pitch_offset = mmio_read(c, SRC_PITCH_OFFSET);
    r->dst_pitch_offset = mmio_read(c, DST_PITCH_OFFSET);
    r->src_y_x = mmio_read(c, SRC_Y_X);
    r->dst_y_x = mmio_read(c, DST_Y_X);
    r->gui_master = mmio_read(c, DP_GUI_MASTER_CNTL);
    r->brush_frgd = mmio_read(c, DP_BRUSH_FRGD_CLR);
    r->sc_left = mmio_read(c, SC_LEFT);
    r->sc_right = mmio_read(c, SC_RIGHT);
    r->sc_top = mmio_read(c, SC_TOP);
    r->sc_bottom = mmio_read(c, SC_BOTTOM);
    r->dp_cntl = mmio_read(c, DP_CNTL);
    r->dp_datatype = mmio_read(c, DP_DATATYPE);
    r->dp_mix = mmio_read(c, DP_MIX);
    r->dp_write_mask = mmio_read(c, DP_WRITE_MASK);
}

static int restore_regs(struct rv100_context *c, const struct saved_regs *r)
{
    if (wait_fifo(c, 14u)) return -1;
    /* MASTER aliases pitch/offset, scissor, DATATYPE and MIX state.  Program
     * it first; every directly addressable register is restored afterwards. */
    mmio_write(c, DP_GUI_MASTER_CNTL, r->gui_master);
    mmio_write(c, SRC_PITCH_OFFSET, r->src_pitch_offset);
    mmio_write(c, DST_PITCH_OFFSET, r->dst_pitch_offset);
    mmio_write(c, SRC_Y_X, r->src_y_x);
    mmio_write(c, DST_Y_X, r->dst_y_x);
    mmio_write(c, DP_BRUSH_FRGD_CLR, r->brush_frgd);
    mmio_write(c, SC_LEFT, r->sc_left);
    mmio_write(c, SC_RIGHT, r->sc_right);
    mmio_write(c, SC_TOP, r->sc_top);
    mmio_write(c, SC_BOTTOM, r->sc_bottom);
    mmio_write(c, DP_CNTL, r->dp_cntl);
    mmio_write(c, DP_DATATYPE, r->dp_datatype);
    mmio_write(c, DP_MIX, r->dp_mix);
    mmio_write(c, DP_WRITE_MASK, r->dp_write_mask);
    barrier(c);
    if (mmio_read(c, SRC_PITCH_OFFSET) != r->src_pitch_offset ||
        mmio_read(c, DST_PITCH_OFFSET) != r->dst_pitch_offset ||
        mmio_read(c, SRC_Y_X) != r->src_y_x ||
        mmio_read(c, DST_Y_X) != r->dst_y_x ||
        mmio_read(c, DP_GUI_MASTER_CNTL) != r->gui_master ||
        mmio_read(c, DP_BRUSH_FRGD_CLR) != r->brush_frgd ||
        mmio_read(c, SC_LEFT) != r->sc_left ||
        mmio_read(c, SC_RIGHT) != r->sc_right ||
        mmio_read(c, SC_TOP) != r->sc_top ||
        mmio_read(c, SC_BOTTOM) != r->sc_bottom ||
        mmio_read(c, DP_CNTL) != r->dp_cntl ||
        mmio_read(c, DP_DATATYPE) != r->dp_datatype ||
        mmio_read(c, DP_MIX) != r->dp_mix ||
        mmio_read(c, DP_WRITE_MASK) != r->dp_write_mask)
        return -1;
    return 0;
}

static int issue_fill(struct rv100_context *c, uint32_t offset,
                      uint32_t pitch, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height, uint32_t color)
{
    if (wait_fifo(c, 11u)) return -1;
    mmio_write(c, DP_WRITE_MASK, UINT32_MAX);
    mmio_write(c, DP_CNTL, DP_LEFT_TO_RIGHT | DP_TOP_TO_BOTTOM);
    mmio_write(c, SC_LEFT, 0u);
    mmio_write(c, SC_RIGHT, 0x1fffu);
    mmio_write(c, SC_TOP, 0u);
    mmio_write(c, SC_BOTTOM, 0x1fffu);
    mmio_write(c, DST_PITCH_OFFSET, pitch_offset(offset, pitch));
    mmio_write(c, DST_Y_X, (y << 16) | x);
    mmio_write(c, DP_BRUSH_FRGD_CLR, color);
    mmio_write(c, DP_GUI_MASTER_CNTL,
               GMC_DST_PITCH_OFFSET | GMC_SRC_CLIPPING | GMC_DST_CLIPPING |
               GMC_BRUSH_SOLID | GMC_DST_32BPP | ROP3_PATCOPY |
               DP_SRC_MEMORY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS);
    barrier(c);
    mmio_write(c, DST_HEIGHT_WIDTH, (height << 16) | width);
    c->info.commands++;
    return 0;
}

static int issue_copy(struct rv100_context *c, uint32_t dst_offset,
                      uint32_t src_offset, uint32_t pitch,
                      uint32_t dst_x, uint32_t dst_y,
                      uint32_t src_x, uint32_t src_y,
                      uint32_t width, uint32_t height, uint32_t direction)
{
    if (wait_fifo(c, 12u)) return -1;
    mmio_write(c, DP_WRITE_MASK, UINT32_MAX);
    mmio_write(c, DP_CNTL, direction);
    mmio_write(c, SC_LEFT, 0u);
    mmio_write(c, SC_RIGHT, 0x1fffu);
    mmio_write(c, SC_TOP, 0u);
    mmio_write(c, SC_BOTTOM, 0x1fffu);
    mmio_write(c, SRC_PITCH_OFFSET, pitch_offset(src_offset, pitch));
    mmio_write(c, DST_PITCH_OFFSET, pitch_offset(dst_offset, pitch));
    mmio_write(c, SRC_Y_X, (src_y << 16) | src_x);
    mmio_write(c, DST_Y_X, (dst_y << 16) | dst_x);
    mmio_write(c, DP_GUI_MASTER_CNTL,
               GMC_SRC_PITCH_OFFSET | GMC_DST_PITCH_OFFSET |
               GMC_SRC_CLIPPING | GMC_DST_CLIPPING | GMC_BRUSH_NONE |
               GMC_DST_32BPP | GMC_SRC_COLOR | ROP3_SRCCOPY |
               DP_SRC_MEMORY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS);
    barrier(c);
    mmio_write(c, DST_HEIGHT_WIDTH, (height << 16) | width);
    c->info.commands++;
    return 0;
}

static int command_complete(struct rv100_context *c)
{
    if (wait_idle(c)) return RV100_ENGINE_TIMEOUT;
    if (flush_dstcache(c)) return RV100_CACHE_TIMEOUT;
    barrier(c);
    return RV100_OK;
}

static void save_vram(struct rv100_context *c, uint32_t base,
                      uint32_t out[CANARY_WORDS])
{
    for (uint32_t i = 0; i < CANARY_WORDS; i++)
        out[i] = vram_read(c, base + i * 4u);
}

static void seed_vram(struct rv100_context *c, uint32_t base, uint32_t value)
{
    for (uint32_t i = 0; i < CANARY_WORDS; i++)
        vram_write(c, base + i * 4u, value);
}

static int verify_rect(struct rv100_context *c, uint32_t base,
                       uint32_t inside, uint32_t padding)
{
    for (uint32_t y = 0; y < CANARY_H; y++) {
        for (uint32_t x = 0; x < CANARY_PITCH / 4u; x++) {
            uint32_t want = x < CANARY_W ? inside : padding;
            if (vram_read(c, base + y * CANARY_PITCH + x * 4u) != want)
                return 0;
        }
    }
    return 1;
}

static int restore_vram(struct rv100_context *c, uint32_t src, uint32_t dst,
                        const uint32_t old_src[CANARY_WORDS],
                        const uint32_t old_dst[CANARY_WORDS])
{
    for (uint32_t i = 0; i < CANARY_WORDS; i++) {
        vram_write(c, src + i * 4u, old_src[i]);
        vram_write(c, dst + i * 4u, old_dst[i]);
    }
    barrier(c);
    for (uint32_t i = 0; i < CANARY_WORDS; i++) {
        if (vram_read(c, src + i * 4u) != old_src[i] ||
            vram_read(c, dst + i * 4u) != old_dst[i])
            return 0;
    }
    return 1;
}

static int restore_all(struct rv100_context *c, const struct saved_regs *regs,
                       uint32_t src, uint32_t dst,
                       const uint32_t old_src[CANARY_WORDS],
                       const uint32_t old_dst[CANARY_WORDS])
{
    int vram_ok = restore_vram(c, src, dst, old_src, old_dst);
    int regs_ok = restore_regs(c, regs) == 0;
    return vram_ok && regs_ok;
}

static int surface_ok(const struct rv100_context *c, uint32_t x, uint32_t y,
                      uint32_t width, uint32_t height)
{
#ifdef RV100_NEGCTL_UNBOUNDED_SURFACE
    (void)c; (void)x; (void)y; (void)width; (void)height;
    return 1;
#else
    if (!width || !height || x > 0x3fffu || y > 0x3fffu ||
        width > 0x3fffu || height > 0x3fffu || width > UINT32_MAX / 4u)
        return 0;
    if (x > c->info.width || width > c->info.width - x ||
        y > c->info.height || height > c->info.height - y)
        return 0;
    uint64_t last = (uint64_t)(y + height - 1u) * c->info.pitch +
                    (uint64_t)(x + width) * 4u;
    return width * 4u <= c->info.pitch && last <= c->info.scanout_bytes &&
           c->info.scanout_offset <= c->info.vram_bytes - last;
#endif
}

int rv100_prepare(struct rv100_context *ctx, const struct rv100_device *dev,
                  uint64_t lfb_phys, uint64_t lfb_bytes,
                  uint32_t width, uint32_t height,
                  const struct rv100_ops *ops)
{
    if (!ctx) return -1;
    bytes_zero(ctx, sizeof *ctx);
    ctx->info.software_fallback = 1;

    /* This exact identity gate precedes even validation of callbacks.  Modern
     * AMD families cannot accidentally reach an RV100 BAR mapping. */
    if (!dev || dev->vendor != RV100_VENDOR_ID ||
#ifndef RV100_NEGCTL_BROAD_ID
        dev->device != RV100_DEVICE_ID ||
#endif
        dev->class_code != RV100_CLASS_DISPLAY ||
        (dev->subclass != 0x00u && dev->subclass != 0x02u) ||
        dev->header_type != 0u)
        return fail(ctx, RV100_WRONG_DEVICE);
    ctx->info.stage = RV100_IDENTIFIED;

    if (dev->command == UINT16_MAX || !(dev->command & RV100_PCI_CMD_MEM))
        return fail(ctx, RV100_BAD_COMMAND);
    if (dev->pm_cap &&
        (dev->pm_cap < 0x40u || dev->pm_cap > 0xf8u ||
         (dev->pm_cap & 3u) || (dev->pmcsr & 3u)))
        return fail(ctx, RV100_BAD_PM);
    if (!sane_mem_bar(&dev->bar[0], 1u)) return fail(ctx, RV100_BAD_BAR0);
    if (!sane_mem_bar(&dev->bar[2], 0x4000u)) return fail(ctx, RV100_BAD_BAR2);
    if (!ops || !ops->map_bar || !ops->mmio_read32 || !ops->mmio_write32 ||
        !ops->vram_read32 || !ops->vram_write32 || !ops->now_ns)
        return fail(ctx, RV100_MAP_FAILED);
    ctx->ops = *ops;

    /* RV100's documented DEFAULT_SC maxima are 0x1fff inclusive. */
    if (!width || !height || width > 0x2000u || height > 0x2000u ||
        lfb_bytes > UINT32_MAX || !contains(dev->bar[0].start, dev->bar[0].size,
                                           lfb_phys, lfb_bytes) ||
        lfb_bytes % height)
        return fail(ctx, RV100_BAD_SCANOUT);
    uint64_t pitch64 = lfb_bytes / height;
    uint64_t scanout_off64 = lfb_phys - dev->bar[0].start;
    if (!pitch64 || pitch64 > 0x3fc0u || (pitch64 & 63u) ||
        width > UINT32_MAX / 4u || width * 4u > pitch64 ||
        scanout_off64 > UINT32_MAX || (scanout_off64 & 1023u))
        return fail(ctx, RV100_BAD_SCANOUT);

    ctx->info.scanout_offset = (uint32_t)scanout_off64;
    ctx->info.scanout_bytes = (uint32_t)lfb_bytes;
    ctx->info.pitch = (uint32_t)pitch64;
    ctx->info.width = width;
    ctx->info.height = height;

    ctx->mmio_base = ops->map_bar(ops->ctx, dev, 2);
    if (!ctx->mmio_base) return fail(ctx, RV100_MAP_FAILED);
    ctx->info.stage = RV100_MMIO_MAPPED;
    uint32_t vram_bytes = mmio_read(ctx, CNFG_MEMSIZE);
    if (!vram_bytes || vram_bytes == UINT32_MAX || vram_bytes > dev->bar[0].size ||
        ctx->info.scanout_offset > vram_bytes ||
        ctx->info.scanout_bytes > vram_bytes - ctx->info.scanout_offset)
        return fail(ctx, RV100_BAD_VRAM_SIZE);
    ctx->info.vram_bytes = vram_bytes;

    uint64_t scanout_end = scanout_off64 + lfb_bytes;
    uint64_t src64 = (scanout_end + CANARY_BLOCK - 1u) &
                     ~(uint64_t)(CANARY_BLOCK - 1u);
    uint64_t dst64 = src64 + CANARY_BLOCK;
    if (scanout_end > UINT32_MAX || src64 < scanout_end || dst64 < src64 ||
        dst64 > UINT32_MAX || CANARY_BYTES > vram_bytes ||
        dst64 > (uint64_t)vram_bytes - CANARY_BYTES)
        return fail(ctx, RV100_NO_OFFSCREEN_SPACE);
    ctx->info.canary_src = (uint32_t)src64;
    ctx->info.canary_dst = (uint32_t)dst64;
    /* Keep full padded scanout staging beyond both canary blocks.  This keeps
     * runtime uploads independent of the diagnostic scratch lifetime. */
    uint64_t staging64 = dst64 + CANARY_BLOCK;
    if (lfb_bytes <= vram_bytes && staging64 <= (uint64_t)vram_bytes - lfb_bytes) {
        ctx->info.staging_offset = (uint32_t)staging64;
        ctx->info.staging_bytes = (uint32_t)lfb_bytes;
    }

    ctx->vram_base = ops->map_bar(ops->ctx, dev, 0);
    if (!ctx->vram_base) return fail(ctx, RV100_MAP_FAILED);
    ctx->info.stage = RV100_VRAM_MAPPED;

    /* Establish ownership of an idle and cache-clean 2D engine before touching
     * off-screen VRAM.  A firmware dirty line must not overwrite our seed. */
    if (wait_idle(ctx)) return fail(ctx, RV100_ENGINE_TIMEOUT);
    if (flush_dstcache(ctx)) return fail(ctx, RV100_CACHE_TIMEOUT);

    struct saved_regs regs;
    uint32_t old_src[CANARY_WORDS], old_dst[CANARY_WORDS];
    save_regs(ctx, &regs);
    save_vram(ctx, ctx->info.canary_src, old_src);
    save_vram(ctx, ctx->info.canary_dst, old_dst);
    seed_vram(ctx, ctx->info.canary_src, CANARY_SRC_SEED);
    seed_vram(ctx, ctx->info.canary_dst, CANARY_DST_SEED);
    barrier(ctx);

    ctx->info.stage = RV100_CANARY_FILL;
    if (issue_fill(ctx, ctx->info.canary_src, CANARY_PITCH, 0, 0,
                   CANARY_W, CANARY_H, CANARY_FILL))
        goto safe_engine_failure;
    {
        int done = command_complete(ctx);
        if (done != RV100_OK) {
            ctx->info.quarantined = 1;
            return fail(ctx, (enum rv100_blocker)done);
        }
    }
    if (!verify_rect(ctx, ctx->info.canary_src,
                     CANARY_FILL, CANARY_SRC_SEED))
        goto fill_mismatch;
    ctx->info.fill_ok = 1;

    ctx->info.stage = RV100_CANARY_COPY;
    if (issue_copy(ctx, ctx->info.canary_dst, ctx->info.canary_src,
                   CANARY_PITCH, 0, 0, 0, 0, CANARY_W, CANARY_H,
                   DP_LEFT_TO_RIGHT | DP_TOP_TO_BOTTOM))
        goto safe_engine_failure;
    {
        int done = command_complete(ctx);
        if (done != RV100_OK) {
            ctx->info.quarantined = 1;
            return fail(ctx, (enum rv100_blocker)done);
        }
    }
    if (!verify_rect(ctx, ctx->info.canary_dst,
                     CANARY_FILL, CANARY_DST_SEED))
        goto copy_mismatch;
    ctx->info.copy_ok = 1;

    if (!restore_all(ctx, &regs, ctx->info.canary_src, ctx->info.canary_dst,
                     old_src, old_dst))
        return fail(ctx, RV100_RESTORE_MISMATCH);
    ctx->info.restore_ok = 1;
    ctx->info.stage = RV100_ACTIVE;
    ctx->info.blocker = RV100_OK;
    /* Formerly this only enabled a boot canary.  Runtime present can now
     * consume the prepared engine, but desktop activation belongs to its
     * caller and is not implied by passing this off-screen proof. */
    ctx->info.canary_ready = 1;
    ctx->info.software_fallback = 1;
    return 0;

fill_mismatch:
    if (!restore_all(ctx, &regs, ctx->info.canary_src, ctx->info.canary_dst,
                     old_src, old_dst))
        return fail(ctx, RV100_RESTORE_MISMATCH);
    ctx->info.restore_ok = 1;
    return fail(ctx, RV100_FILL_MISMATCH);

copy_mismatch:
    if (!restore_all(ctx, &regs, ctx->info.canary_src, ctx->info.canary_dst,
                     old_src, old_dst))
        return fail(ctx, RV100_RESTORE_MISMATCH);
    ctx->info.restore_ok = 1;
    return fail(ctx, RV100_COPY_MISMATCH);

safe_engine_failure:
    /* FIFO admission failed before the trigger, so no new command is in flight. */
    if (!restore_all(ctx, &regs, ctx->info.canary_src, ctx->info.canary_dst,
                     old_src, old_dst))
        return fail(ctx, RV100_RESTORE_MISMATCH);
    ctx->info.restore_ok = 1;
    return fail(ctx, RV100_ENGINE_TIMEOUT);
}

static int lock_context(struct rv100_context *c)
{
    return __sync_lock_test_and_set(&c->lock, 1u) == 0u;
}

static void unlock_context(struct rv100_context *c)
{
    __sync_lock_release(&c->lock);
}

int rv100_fill(struct rv100_context *c, uint32_t x, uint32_t y,
               uint32_t width, uint32_t height, uint32_t color)
{
    if (!c || !lock_context(c)) return -1;
    if (c->info.stage != RV100_ACTIVE || c->info.quarantined) {
        unlock_context(c);
        return -1;
    }
    if (!surface_ok(c, x, y, width, height)) {
        c->info.blocker = RV100_BAD_SURFACE;
        unlock_context(c);
        return -1;
    }
    int rc = issue_fill(c, c->info.scanout_offset, c->info.pitch,
                        x, y, width, height, color);
    int done = rc ? RV100_ENGINE_TIMEOUT : command_complete(c);
    if (done != RV100_OK) {
        c->info.quarantined = 1;
        c->info.stage = RV100_BLOCKED;
        c->info.blocker = (enum rv100_blocker)done;
        c->info.software_fallback = 1;
        rc = -1;
    }
    if (!rc) c->info.blocker = RV100_OK;
    unlock_context(c);
    return rc;
}

int rv100_copy(struct rv100_context *c, uint32_t dx, uint32_t dy,
               uint32_t sx, uint32_t sy, uint32_t width, uint32_t height)
{
    if (!c || !lock_context(c)) return -1;
    if (c->info.stage != RV100_ACTIVE || c->info.quarantined) {
        unlock_context(c);
        return -1;
    }
    if (!surface_ok(c, dx, dy, width, height) ||
        !surface_ok(c, sx, sy, width, height)) {
        c->info.blocker = RV100_BAD_SURFACE;
        unlock_context(c);
        return -1;
    }

    uint32_t direction = DP_LEFT_TO_RIGHT | DP_TOP_TO_BOTTOM;
    uint32_t src_x = sx, src_y = sy, dst_x = dx, dst_y = dy;
    int vertical_overlap = dy < sy + height && sy < dy + height;
    if (vertical_overlap && dy > sy) {
        direction &= ~DP_TOP_TO_BOTTOM;
        src_y += height - 1u;
        dst_y += height - 1u;
    } else if (dy == sy && dx > sx && dx < sx + width) {
        direction &= ~DP_LEFT_TO_RIGHT;
        src_x += width - 1u;
        dst_x += width - 1u;
    }

    int rc = issue_copy(c, c->info.scanout_offset, c->info.scanout_offset,
                        c->info.pitch, dst_x, dst_y, src_x, src_y,
                        width, height, direction);
    int done = rc ? RV100_ENGINE_TIMEOUT : command_complete(c);
    if (done != RV100_OK) {
        c->info.quarantined = 1;
        c->info.stage = RV100_BLOCKED;
        c->info.blocker = (enum rv100_blocker)done;
        c->info.software_fallback = 1;
        rc = -1;
    }
    if (!rc) c->info.blocker = RV100_OK;
    unlock_context(c);
    return rc;
}

/* A timeout is sticky: once an engine may still write, returning the ordinary
 * fallback code would let CPU presentation race that write.  Reset/reprobe is
 * required to leave quarantine; a later present must not silently retry. */
static int present_quarantine(struct rv100_context *c, enum rv100_blocker why)
{
    c->info.quarantined = 1;
    fail(c, why);
    unlock_context(c);
    return -2;
}

int rv100_present(struct rv100_context *c, const uint32_t *pixels,
                  uint32_t stride_pixels, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height)
{
    if (!c) return -1;
    /* A contended caller cannot infer whether the owner has rung the trigger.
     * It must not update even diagnostics owned by that same lock. */
    if (!lock_context(c)) return -2;
    if (c->info.quarantined) { unlock_context(c); return -2; }
    if (c->info.stage != RV100_ACTIVE) { unlock_context(c); return -1; }
    if (!pixels || !surface_ok(c, x, y, width, height) ||
        stride_pixels < c->info.width) {
        c->info.blocker = RV100_BAD_SURFACE;
        unlock_context(c);
        return -1;
    }
    /* surface_ok bounded y+height to the prepared hardware surface before
     * multiplication, so a hostile UINT32_MAX rectangle cannot wrap here. */
    uint64_t source_end = ((uint64_t)y + height - 1u) * stride_pixels +
                           (uint64_t)x + width;
    if (source_end > SIZE_MAX / sizeof *pixels ||
        (uintptr_t)pixels > UINTPTR_MAX - source_end * sizeof *pixels) {
        c->info.blocker = RV100_BAD_SURFACE;
        unlock_context(c);
        return -1;
    }
    /* Even solid rectangles require staging capability: one consumer should
     * not oscillate between GPU and CPU ownership based on the image content. */
    if (!c->info.staging_bytes ||
        c->info.staging_bytes < c->info.scanout_bytes ||
        c->info.staging_offset & 1023u ||
        c->info.staging_offset < (uint64_t)c->info.scanout_offset +
                                  c->info.scanout_bytes ||
        c->info.staging_bytes > c->info.vram_bytes ||
        c->info.staging_offset > c->info.vram_bytes - c->info.staging_bytes) {
        c->info.blocker = RV100_NO_OFFSCREEN_SPACE;
        unlock_context(c);
        return -1;
    }
    if (wait_idle(c)) return present_quarantine(c, RV100_ENGINE_TIMEOUT);
    if (flush_dstcache(c)) return present_quarantine(c, RV100_CACHE_TIMEOUT);
    barrier(c);

    uint32_t color = pixels[(size_t)y * stride_pixels + x];
    int solid = 1;
    for (uint32_t row = 0; row < height && solid; row++)
        for (uint32_t col = 0; col < width; col++)
            if (pixels[(size_t)(y + row) * stride_pixels + x + col] != color) {
                solid = 0;
                break;
            }
    int rc;
    if (solid) {
        rc = issue_fill(c, c->info.scanout_offset, c->info.pitch,
                        x, y, width, height, color);
    } else {
        /* Preserve all 32 source bits and upload only damage.  CPU stores never
         * target scanout; the engine owns that write after the final trigger. */
        for (uint32_t row = 0; row < height; row++)
            for (uint32_t col = 0; col < width; col++) {
                uint32_t value = pixels[(size_t)(y + row) * stride_pixels + x + col];
#ifdef RV100_NEGCTL_PRESENT_CORRUPT
                value ^= 1u; /* Runtime-only oracle must detect the wrong pixel. */
#endif
                vram_write(c, c->info.staging_offset + (y + row) *
                           c->info.pitch + (x + col) * 4u, value);
            }
        c->info.uploaded_bytes += (uint64_t)width * height * 4u;
        barrier(c);
        rc = issue_copy(c, c->info.scanout_offset, c->info.staging_offset,
                        c->info.pitch, x, y, x, y, width, height,
                        DP_LEFT_TO_RIGHT | DP_TOP_TO_BOTTOM);
    }
    if (rc) {
        /* FIFO admission failed before DST_HEIGHT_WIDTH; prior idle/cache was
         * established above and no runtime trigger has occurred. */
        fail(c, RV100_ENGINE_TIMEOUT);
        unlock_context(c);
        return -1;
    }
    int done = command_complete(c);
    if (done != RV100_OK)
        return present_quarantine(c, (enum rv100_blocker)done);
    /* The first real image, unlike the boot's constant-color canary, proves
     * RAM upload and scanout coordinates against actual pixels.  Readback is
     * paid once: repeated VRAM reads would defeat ordinary presentation. */
    if (!c->info.present_verified) {
        for (uint32_t row = 0; row < height; row++)
            for (uint32_t col = 0; col < width; col++)
                if (vram_read(c, c->info.scanout_offset + (y + row) *
                              c->info.pitch + (x + col) * 4u) !=
                    pixels[(size_t)(y + row) * stride_pixels + x + col]) {
                    /* Completion/cache is proven, so CPU fallback is safe;
                     * block subsequent runtime commands until reprepare. */
                    fail(c, RV100_COPY_MISMATCH);
                    unlock_context(c);
                    return -1;
                }
        c->info.present_verified = 1;
    }
    c->info.blocker = RV100_OK;
    c->info.presents++;
    c->info.gpu_pixels += (uint64_t)width * height;
    if (solid) c->info.solid_presents++; else c->info.copy_presents++;
    unlock_context(c);
    return 0;
}

const char *rv100_blocker_name(enum rv100_blocker b)
{
    switch (b) {
    case RV100_OK: return "none";
    case RV100_WRONG_DEVICE: return "wrong-device";
    case RV100_BAD_COMMAND: return "memory-decode";
    case RV100_BAD_PM: return "power-state";
    case RV100_BAD_BAR0: return "bad-vram-bar";
    case RV100_BAD_BAR2: return "bad-mmio-bar";
    case RV100_BAD_SCANOUT: return "bad-scanout";
    case RV100_MAP_FAILED: return "map-failed";
    case RV100_BAD_VRAM_SIZE: return "vram-size";
    case RV100_NO_OFFSCREEN_SPACE: return "offscreen-space";
    case RV100_ENGINE_TIMEOUT: return "engine-timeout";
    case RV100_CACHE_TIMEOUT: return "cache-timeout";
    case RV100_FILL_MISMATCH: return "fill-readback";
    case RV100_COPY_MISMATCH: return "copy-readback";
    case RV100_RESTORE_MISMATCH: return "restore-readback";
    case RV100_BAD_SURFACE: return "surface-bounds";
    case RV100_BUSY: return "command-busy";
    default: return "unknown";
    }
}
