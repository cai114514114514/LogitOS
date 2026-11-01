#include "amd/polaris/display/crtc.h"
#include "kprintf.h"

/* Register dword indices from Linux v6.12 include/asic_reg/dce/dce_11_2_d.h.
 * The CRTC and GRPH blocks repeat per CRTC at the dword strides in
 * crtc_offset_dwords (vid.h CRTCn_REGISTER_OFFSET); the two VGA/viewport
 * registers below are read once, as gmc_v8_0_get_vbios_fb_size does. */
#define MM_CRTC_H_TOTAL 0x1b80u
#define MM_CRTC_H_BLANK_START_END 0x1b81u
#define MM_CRTC_V_TOTAL 0x1b87u
#define MM_CRTC_V_BLANK_START_END 0x1b8du
#define MM_CRTC_CONTROL 0x1b9cu
#define MM_CRTC_STATUS_FRAME_COUNT 0x1ba6u
#define MM_GRPH_ENABLE 0x1a00u
#define MM_GRPH_CONTROL 0x1a01u
#define MM_GRPH_PRIMARY_SURFACE_ADDRESS 0x1a04u
#define MM_GRPH_PITCH 0x1a06u
#define MM_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH 0x1a07u
#define MM_CUR_CONTROL 0x1a66u
#define MM_CUR_SURFACE_ADDRESS 0x1a67u
#define MM_CUR_SIZE 0x1a68u
#define MM_CUR_SURFACE_ADDRESS_HIGH 0x1a69u
#define MM_VIEWPORT_SIZE 0x1b5du
#define MM_D1VGA_CONTROL 0x00ccu

/* dce_11_2_sh_mask.h field masks. */
#define CRTC_MASTER_EN_MASK 0x1u
#define GRPH_ENABLE_MASK 0x1u
#define GRPH_DEPTH_MASK 0x3u
#define GRPH_ARRAY_MODE_SHIFT 20u
#define GRPH_ARRAY_MODE_MASK 0xfu
#define GRPH_PITCH_MASK 0x7fffu
#define GRPH_SURFACE_LOW_MASK 0xffffff00u
#define GRPH_SURFACE_HIGH_MASK 0xffu
#define CRTC_TOTAL_MASK 0x3fffu
#define CRTC_BLANK_FIELD_MASK 0x3fffu
#define CRTC_BLANK_END_SHIFT 16u
#define CUR_CONTROL_CURSOR_EN_MASK 0x1u
#define CUR_SIZE_HEIGHT_MASK 0x7fu
#define CUR_SIZE_WIDTH_SHIFT 16u
#define CUR_SIZE_WIDTH_MASK 0x7fu
#define CUR_SURFACE_HIGH_MASK 0xffu
#define VIEWPORT_HEIGHT_MASK 0x3fffu
#define VIEWPORT_WIDTH_SHIFT 16u
#define VIEWPORT_WIDTH_MASK 0x3fffu
#define D1VGA_MODE_ENABLE_MASK 0x1u
#define FRAME_COUNT_MASK 0xffffffu

/* dce_11_2_enum.h ArrayMode: ARRAY_LINEAR_GENERAL and ARRAY_LINEAR_ALIGNED are
 * the two layouts whose size is simply pitch * height. */
#define ARRAY_LINEAR_GENERAL 0u
#define ARRAY_LINEAR_ALIGNED 1u

#define CURSOR_BYTES_PER_PIXEL 4u
#define DWORD_BYTES 4u

/* vid.h CRTC0..CRTC5_REGISTER_OFFSET: (0x1b9c, 0x1d9c, 0x1f9c, 0x419c, 0x439c,
 * 0x459c) minus 0x1b9c. */
static const uint32_t crtc_offset_dwords[POLARIS_DISPLAY_CRTC_COUNT] = {
    0x0000u, 0x0200u, 0x0400u, 0x2600u, 0x2800u, 0x2a00u
};

_Static_assert(POLARIS_DISPLAY_BAR5_BYTES_NEEDED ==
               (MM_CRTC_STATUS_FRAME_COUNT + 0x2a00u + 1u) * DWORD_BYTES,
               "BAR5 bound must end right after CRTC5 CRTC_STATUS_FRAME_COUNT");

/* Refresh measurement budgets. The poll budget bounds the work when the clock
 * itself misbehaves; the stall run catches a clock that no longer advances long
 * before the budget is spent. 2^16 flat polls is about 65 ms of PCIe reads,
 * well over the 10 ms a coarse clock tick needs to move, so only a clock that
 * has really stopped reaches the stall run. */
#define REFRESH_POLL_BUDGET (1u << 24)
#define REFRESH_STALL_RUN (1u << 16)
#define REFRESH_MIN_MILLIHERTZ 10000u
#define REFRESH_MAX_MILLIHERTZ 1000000u
/* 1e9 ns per second times 1e3 mHz per Hz. */
#define MILLIHERTZ_TIMES_NANOSECONDS UINT64_C(1000000000000)

/* One CRTC register: the block base is shared, the CRTC stride is added. */
static uint32_t read_crtc_register(const struct polaris_display_ops *ops,
                                   unsigned crtc_index, uint32_t dword_index)
{
    uint32_t dword = dword_index + crtc_offset_dwords[crtc_index];
    return ops->read32(ops->ctx, dword * DWORD_BYTES);
}

struct crtc_raw_registers {
    uint32_t control;
    uint32_t graphics_enable;
    uint32_t graphics_control;
    uint32_t graphics_pitch;
    uint32_t surface_low;
    uint32_t surface_high;
    uint32_t horizontal_total;
    uint32_t horizontal_blank;
    uint32_t vertical_total;
    uint32_t vertical_blank;
    uint32_t cursor_control;
    uint32_t cursor_low;
    uint32_t cursor_high;
    uint32_t cursor_size;
    uint32_t frame_count;
};

/* Turn raw register values into the decoded state. Kept free of register
 * access so the decode rules (stored totals are total-1, the active size is
 * BLANK_START - BLANK_END, GRPH_PITCH counts pixels) are exercised by value. */
static void decode_crtc(unsigned crtc_index, const struct crtc_raw_registers *raw,
                        struct polaris_crtc_state *crtc)
{
    *crtc = (struct polaris_crtc_state){0};
    crtc->index = crtc_index;
    crtc->master_enabled = raw->control & CRTC_MASTER_EN_MASK;
    crtc->graphics_enabled = raw->graphics_enable & GRPH_ENABLE_MASK;
    crtc->active = crtc->master_enabled && crtc->graphics_enabled;
    crtc->horizontal_total = (raw->horizontal_total & CRTC_TOTAL_MASK) + 1u;
    crtc->vertical_total = (raw->vertical_total & CRTC_TOTAL_MASK) + 1u;
    uint32_t horizontal_start = raw->horizontal_blank & CRTC_BLANK_FIELD_MASK;
    uint32_t horizontal_end =
        (raw->horizontal_blank >> CRTC_BLANK_END_SHIFT) & CRTC_BLANK_FIELD_MASK;
    uint32_t vertical_start = raw->vertical_blank & CRTC_BLANK_FIELD_MASK;
    uint32_t vertical_end =
        (raw->vertical_blank >> CRTC_BLANK_END_SHIFT) & CRTC_BLANK_FIELD_MASK;
    crtc->width = horizontal_start > horizontal_end ?
                  horizontal_start - horizontal_end : 0;
    crtc->height = vertical_start > vertical_end ?
                   vertical_start - vertical_end : 0;
    crtc->bits_per_pixel = 8u << (raw->graphics_control & GRPH_DEPTH_MASK);
    crtc->pitch_bytes = (raw->graphics_pitch & GRPH_PITCH_MASK) *
                        (crtc->bits_per_pixel / 8u);
    crtc->surface_mc =
        ((uint64_t)(raw->surface_high & GRPH_SURFACE_HIGH_MASK) << 32) |
        (raw->surface_low & GRPH_SURFACE_LOW_MASK);
    crtc->scanout_bytes = (uint64_t)crtc->pitch_bytes * crtc->height;
    unsigned array_mode =
        (raw->graphics_control >> GRPH_ARRAY_MODE_SHIFT) & GRPH_ARRAY_MODE_MASK;
    crtc->scanout_known =
        (array_mode == ARRAY_LINEAR_GENERAL || array_mode == ARRAY_LINEAR_ALIGNED) &&
        crtc->width && crtc->height &&
        crtc->pitch_bytes >= crtc->width * (crtc->bits_per_pixel / 8u);
    crtc->cursor_enabled = raw->cursor_control & CUR_CONTROL_CURSOR_EN_MASK;
    crtc->cursor_mc =
        ((uint64_t)(raw->cursor_high & CUR_SURFACE_HIGH_MASK) << 32) |
        raw->cursor_low;
    /* CUR_SIZE stores width-1 and height-1: dce_v11_0_cursor_move_locked writes
     * ((cursor_width - 1) << 16) | (cursor_height - 1). */
    uint64_t cursor_width =
        ((raw->cursor_size >> CUR_SIZE_WIDTH_SHIFT) & CUR_SIZE_WIDTH_MASK) + 1u;
    uint64_t cursor_height = (raw->cursor_size & CUR_SIZE_HEIGHT_MASK) + 1u;
    crtc->cursor_bytes = cursor_width * cursor_height * CURSOR_BYTES_PER_PIXEL;
    crtc->frame_count = raw->frame_count & FRAME_COUNT_MASK;
}

/* The scan reads and decodes; it is the first thing the driver learns about the
 * display, so it records facts and judges nothing. */
int polaris_display_scan(const struct polaris_display_ops *ops,
                         uint64_t bar5_bytes, struct polaris_display_scan *out)
{
    if (!ops || !ops->read32 || !out) {
        return POLARIS_DISPLAY_BAD_ARGUMENT;
    }
#ifndef POLARIS_DISPLAY_NEGCTL_NO_BAR5_CHECK
    /* The cursor and frame-counter registers of CRTC5 sit past the 0x5494 that
     * the SDMA bring-up needs; an undersized BAR would fault the read. */
    if (bar5_bytes < POLARIS_DISPLAY_BAR5_BYTES_NEEDED) {
        return POLARIS_DISPLAY_BAR5_TOO_SMALL;
    }
#else
    (void)bar5_bytes;
#endif
    struct polaris_display_scan scan = {0};
    for (unsigned index = 0; index < POLARIS_DISPLAY_CRTC_COUNT; ++index) {
        struct crtc_raw_registers raw;
        raw.control = read_crtc_register(ops, index, MM_CRTC_CONTROL);
        if (raw.control == UINT32_MAX) {
            return POLARIS_DISPLAY_DEAD_READ;
        }
        raw.graphics_enable = read_crtc_register(ops, index, MM_GRPH_ENABLE);
        raw.graphics_control = read_crtc_register(ops, index, MM_GRPH_CONTROL);
        raw.graphics_pitch = read_crtc_register(ops, index, MM_GRPH_PITCH);
        raw.surface_low =
            read_crtc_register(ops, index, MM_GRPH_PRIMARY_SURFACE_ADDRESS);
        raw.surface_high =
            read_crtc_register(ops, index, MM_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH);
        raw.horizontal_total = read_crtc_register(ops, index, MM_CRTC_H_TOTAL);
        raw.horizontal_blank =
            read_crtc_register(ops, index, MM_CRTC_H_BLANK_START_END);
        raw.vertical_total = read_crtc_register(ops, index, MM_CRTC_V_TOTAL);
        raw.vertical_blank =
            read_crtc_register(ops, index, MM_CRTC_V_BLANK_START_END);
        raw.cursor_control = read_crtc_register(ops, index, MM_CUR_CONTROL);
        raw.cursor_low = read_crtc_register(ops, index, MM_CUR_SURFACE_ADDRESS);
        raw.cursor_high =
            read_crtc_register(ops, index, MM_CUR_SURFACE_ADDRESS_HIGH);
        raw.cursor_size = read_crtc_register(ops, index, MM_CUR_SIZE);
        raw.frame_count =
            read_crtc_register(ops, index, MM_CRTC_STATUS_FRAME_COUNT);
        decode_crtc(index, &raw, &scan.crtc[index]);
        if (scan.crtc[index].active) {
            ++scan.active_count;
        }
#ifdef POLARIS_DISPLAY_NEGCTL_WRITE
        /* Watched mutation: a naive "make sure the CRTC is enabled" write-back
         * of the value just read. The gate must see this as a register write. */
        ops->write32(ops->ctx,
                     (MM_CRTC_CONTROL + crtc_offset_dwords[index]) * DWORD_BYTES,
                     raw.control);
#endif
    }
    uint32_t vga_control = ops->read32(ops->ctx, MM_D1VGA_CONTROL * DWORD_BYTES);
    uint32_t viewport = ops->read32(ops->ctx, MM_VIEWPORT_SIZE * DWORD_BYTES);
    scan.vga_mode_enabled = vga_control & D1VGA_MODE_ENABLE_MASK;
    scan.viewport_width = (viewport >> VIEWPORT_WIDTH_SHIFT) & VIEWPORT_WIDTH_MASK;
    scan.viewport_height = viewport & VIEWPORT_HEIGHT_MASK;
    *out = scan;
    return POLARIS_DISPLAY_OK;
}

/* Log lines name the refusal instead of printing a bare number, so a photo of
 * the HUD is enough to tell a dead register read from an undersized BAR5. */
const char *polaris_display_result_name(int result)
{
    switch (result) {
    case POLARIS_DISPLAY_OK:
        return "scanned";
    case POLARIS_DISPLAY_BAR5_TOO_SMALL:
        return "bar5-too-small-for-display-registers";
    case POLARIS_DISPLAY_DEAD_READ:
        return "crtc-registers-read-all-ones";
    default:
        return "bad-argument";
    }
}

/* The CRTC may be one of several clones of the same surface, so keep looking
 * after a near miss and only report the most specific failure at the end. */
int polaris_display_find_gop(const struct polaris_display_scan *scan,
                             const struct polaris_display_gop *gop,
                             unsigned *crtc_index)
{
    if (!scan || !gop || !crtc_index || gop->lfb < gop->bar0_physical ||
        gop->lfb - gop->bar0_physical > UINT64_MAX - gop->mc_base) {
        return POLARIS_GOP_BAD_ARGUMENT;
    }
    /* The GOP framebuffer is a BAR0 offset; the CRTC holds an MC address.
     * Their difference from the respective bases is the shared VRAM offset. */
    uint64_t expected_surface = gop->mc_base + (gop->lfb - gop->bar0_physical);
    int worst = POLARIS_GOP_NO_CRTC;
    for (unsigned index = 0; index < POLARIS_DISPLAY_CRTC_COUNT; ++index) {
        const struct polaris_crtc_state *crtc = &scan->crtc[index];
        if (!crtc->active || crtc->surface_mc != expected_surface) {
            continue;
        }
        if (!crtc->scanout_known || crtc->bits_per_pixel != 32u) {
            worst = POLARIS_GOP_NOT_LINEAR_32BPP;
            continue;
        }
#ifndef POLARIS_DISPLAY_NEGCTL_GOP_IGNORE_PITCH
        if (crtc->pitch_bytes != gop->pitch_bytes) {
            if (worst == POLARIS_GOP_NO_CRTC) {
                worst = POLARIS_GOP_PITCH_MISMATCH;
            }
            continue;
        }
#endif
        *crtc_index = index;
        return POLARIS_GOP_MATCH;
    }
    return worst;
}

/* Same purpose as polaris_display_result_name, for the GOP cross-check. */
const char *polaris_gop_result_name(int result)
{
    switch (result) {
    case POLARIS_GOP_MATCH:
        return "gop-crtc-matched";
    case POLARIS_GOP_NO_CRTC:
        return "no-crtc-scans-gop-address";
    case POLARIS_GOP_PITCH_MISMATCH:
        return "gop-pitch-mismatch";
    case POLARIS_GOP_NOT_LINEAR_32BPP:
        return "gop-surface-not-linear-32bpp";
    default:
        return "bad-argument";
    }
}

struct poll_budget {
    unsigned polls;
    unsigned flat_run;
    uint64_t last_ns;
};

/* Decide whether polling must stop because the time source is not usable: the
 * clock went backwards, stood still for REFRESH_STALL_RUN polls, or the total
 * budget is spent. Without this a stuck clock would turn a bounded wait into a
 * boot hang. Returns nonzero to stop. */
static int clock_unusable(struct poll_budget *budget, uint64_t now_ns)
{
    if (now_ns < budget->last_ns) {
        return 1;
    }
    if (now_ns == budget->last_ns) {
        ++budget->flat_run;
    } else {
        budget->flat_run = 0;
    }
    budget->last_ns = now_ns;
    ++budget->polls;
    return budget->flat_run >= REFRESH_STALL_RUN ||
           budget->polls >= REFRESH_POLL_BUDGET;
}

enum edge_wait_result {
    EDGE_WAIT_CHANGED,
    EDGE_WAIT_TIMEOUT,
    EDGE_WAIT_CLOCK_STUCK
};

/* The single register the refresh measurement polls, masked to its 24 bits
 * (CRTC_STATUS_FRAME_COUNT__CRTC_FRAME_COUNT_MASK) so wraparound is exact. */
static uint32_t read_frame_count(const struct polaris_display_ops *ops,
                                 unsigned crtc_index)
{
    return read_crtc_register(ops, crtc_index, MM_CRTC_STATUS_FRAME_COUNT) &
           FRAME_COUNT_MASK;
}

/* Wait for the frame counter to leave `previous`. A counter that never moves is
 * an ordinary outcome here (CRTC off, blanked, or clock gated), reported as a
 * timeout; only a broken time source is reported as stuck. */
static enum edge_wait_result wait_for_frame_edge(
    const struct polaris_display_ops *ops, unsigned crtc_index,
    uint32_t previous, uint32_t *current, uint64_t *edge_ns)
{
    uint64_t started_ns = ops->now_ns(ops->ctx);
    struct poll_budget budget = {0, 0, started_ns};
    for (;;) {
        uint32_t value = read_frame_count(ops, crtc_index);
        uint64_t now_ns = ops->now_ns(ops->ctx);
        if (value != previous) {
            *current = value;
            *edge_ns = now_ns;
            return EDGE_WAIT_CHANGED;
        }
        if (clock_unusable(&budget, now_ns)) {
            return EDGE_WAIT_CLOCK_STUCK;
        }
        if (now_ns - started_ns >= POLARIS_REFRESH_EDGE_TIMEOUT_NS) {
            return EDGE_WAIT_TIMEOUT;
        }
    }
}

/* Idle until the deadline, still reading the counter on every pass: an MMIO
 * read costs far more than a clock read, which keeps the poll budget a real
 * time bound instead of a few milliseconds of spinning. */
static int wait_until(const struct polaris_display_ops *ops, unsigned crtc_index,
                      uint64_t deadline_ns)
{
    struct poll_budget budget = {0, 0, ops->now_ns(ops->ctx)};
    for (;;) {
        (void)read_frame_count(ops, crtc_index);
        uint64_t now_ns = ops->now_ns(ops->ctx);
        if (now_ns >= deadline_ns) {
            return 0;
        }
        if (clock_unusable(&budget, now_ns)) {
            return -1;
        }
    }
}

/* Refresh is the only quantity the CRTC registers cannot give directly: the
 * pixel clock is not readable back on the TMDS path (dce_clock_source.c), so the
 * frame counter is timed against the monotonic clock instead. Both ends sit on
 * a counter edge, which makes the error one poll interval rather than a frame.
 * The BAR5 bound is checked here and not only in polaris_display_scan, because
 * this function polls a register at the far end of the display block and must
 * not rely on a caller having scanned first. */
int polaris_display_measure_refresh(const struct polaris_display_ops *ops,
                                    uint64_t bar5_bytes, unsigned crtc_index,
                                    struct polaris_refresh *out)
{
    if (!ops || !ops->read32 || !ops->now_ns || !out ||
        crtc_index >= POLARIS_DISPLAY_CRTC_COUNT) {
        return POLARIS_REFRESH_BAD_ARGUMENT;
    }
#ifndef POLARIS_DISPLAY_NEGCTL_REFRESH_NO_BAR5_CHECK
    if (bar5_bytes < POLARIS_DISPLAY_BAR5_BYTES_NEEDED) {
        *out = (struct polaris_refresh){.result = POLARIS_REFRESH_BAR5_TOO_SMALL};
        return POLARIS_REFRESH_BAR5_TOO_SMALL;
    }
#else
    /* Watched mutation: poll the frame counter without looking at BAR5's size. */
    (void)bar5_bytes;
#endif
    struct polaris_refresh result = {0};
    uint32_t start_count = 0;
    uint32_t end_count = 0;
    uint64_t start_ns = 0;
    uint64_t end_ns = 0;
    enum edge_wait_result edge = wait_for_frame_edge(
        ops, crtc_index, read_frame_count(ops, crtc_index), &start_count, &start_ns);
    if (edge == EDGE_WAIT_CHANGED) {
        if (wait_until(ops, crtc_index, start_ns + POLARIS_REFRESH_WINDOW_NS)) {
            edge = EDGE_WAIT_CLOCK_STUCK;
        } else {
            edge = wait_for_frame_edge(ops, crtc_index,
                                       read_frame_count(ops, crtc_index),
                                       &end_count, &end_ns);
        }
    }
    if (edge == EDGE_WAIT_TIMEOUT) {
#ifdef POLARIS_DISPLAY_NEGCTL_STOPPED_IS_OK
        /* Watched mutation: report a frozen counter as a finished measurement. */
        result.result = POLARIS_REFRESH_OK;
#else
        result.result = POLARIS_REFRESH_COUNTER_STOPPED;
#endif
    } else if (edge == EDGE_WAIT_CLOCK_STUCK) {
        result.result = POLARIS_REFRESH_CLOCK_STUCK;
    } else {
        result.frames = (end_count - start_count) & FRAME_COUNT_MASK;
        result.elapsed_ns = end_ns - start_ns;
        /* frames per second * 1000, rounded to nearest. frames is at most the
         * 24-bit counter range, so the product fits in 64 bits. The quotient
         * stays 64-bit through the plausibility test: a counter that jumps by
         * thousands of frames per window gives a value above 2^32 that would
         * wrap into the plausible band if it were narrowed first. */
        uint64_t millihertz =
            (result.frames * MILLIHERTZ_TIMES_NANOSECONDS +
             result.elapsed_ns / 2u) / result.elapsed_ns;
#ifdef POLARIS_DISPLAY_NEGCTL_NARROW_FIRST
        /* Watched mutation: narrow to 32 bits before the plausibility test. */
        millihertz = (uint32_t)millihertz;
#endif
        int plausible = millihertz >= REFRESH_MIN_MILLIHERTZ &&
                        millihertz <= REFRESH_MAX_MILLIHERTZ;
        /* The narrowing saturates, so an implausible value stays implausible
         * in the 32-bit field instead of wrapping into something else. */
        result.millihertz = (uint32_t)(millihertz > UINT32_MAX ?
                                       UINT32_MAX : millihertz);
        result.result = plausible ? POLARIS_REFRESH_OK : POLARIS_REFRESH_IMPLAUSIBLE;
    }
    *out = result;
    return result.result;
}

/* Same purpose as polaris_display_result_name, for the refresh measurement. */
const char *polaris_refresh_result_name(int result)
{
    switch (result) {
    case POLARIS_REFRESH_OK:
        return "measured";
    case POLARIS_REFRESH_COUNTER_STOPPED:
        return "counter-stopped";
    case POLARIS_REFRESH_CLOCK_STUCK:
        return "clock-stuck";
    case POLARIS_REFRESH_IMPLAUSIBLE:
        return "implausible";
    case POLARIS_REFRESH_BAR5_TOO_SMALL:
        return "bar5-too-small-for-display-registers";
    default:
        return "bad-argument";
    }
}

/* One formatter feeds both the serial log and the HUD, so the two can never
 * disagree about what was seen on the screen. */
void polaris_display_format_crtc(char *out, int capacity,
                                 const struct polaris_crtc_state *crtc,
                                 const struct polaris_refresh *refresh,
                                 int is_gop)
{
    char refresh_text[32];
    if (!refresh) {
        ksnprintf(refresh_text, sizeof refresh_text, "unmeasured");
    } else if (refresh->result == POLARIS_REFRESH_OK) {
        ksnprintf(refresh_text, sizeof refresh_text, "%u.%03uHz",
                  refresh->millihertz / 1000u, refresh->millihertz % 1000u);
    } else {
        ksnprintf(refresh_text, sizeof refresh_text, "%s",
                  polaris_refresh_result_name(refresh->result));
    }
    ksnprintf(out, capacity,
              "crtc=%u mode=%ux%u refresh=%s pitch=%u surface=0x%llx%s",
              crtc->index, crtc->width, crtc->height, refresh_text,
              crtc->pitch_bytes, (unsigned long long)crtc->surface_mc,
              is_gop ? " gop" : "");
}
