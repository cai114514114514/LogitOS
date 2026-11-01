#ifndef LOGIT_POLARIS_DISPLAY_CRTC_H
#define LOGIT_POLARIS_DISPLAY_CRTC_H

#include <stddef.h>
#include <stdint.h>

/* Read-only scan of the six DCE 11.2 CRTCs of a Polaris10 board, to learn what
 * the firmware (GOP) left scanning out before any driver takes over. Register
 * names and field layouts follow Linux v6.12:
 *   drivers/gpu/drm/amd/amdgpu/vid.h                      CRTCn_REGISTER_OFFSET
 *   drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_d.h       mm* dword indices
 *   drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_sh_mask.h field masks
 *   drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_2_enum.h    ArrayMode
 *   drivers/gpu/drm/amd/amdgpu/dce_v11_0.c   vblank_get_counter, cursor and
 *                                            GRPH_PITCH (pixels) usage
 *   drivers/gpu/drm/amd/display/dc/dce110/dce110_timing_generator.c
 *                                            program_blanking (blank = active)
 * There is deliberately NO write callback: the scan cannot change the display,
 * and the unit gate pins the shape of struct polaris_display_ops to prove it.
 * Model evidence only; not verified on X79-VG4 hardware. */

#define POLARIS_DISPLAY_CRTC_COUNT 6u

/* Smallest BAR5 that holds every register this scan reads. The farthest one is
 * CRTC5's CRTC_STATUS_FRAME_COUNT: dword (0x1ba6 + 0x2a00) * 4 + 4. CRTC5's
 * cursor registers (CUR_UPDATE, byte 0x111b8) end lower, so one bound also
 * covers the hardware-cursor stage; native.c alone only demands 0x5494. */
#define POLARIS_DISPLAY_BAR5_BYTES_NEEDED 0x1169cu

/* Polling bounds for the refresh measurement (see measure_refresh). The
 * measurement runs on the boot CPU while the desktop is already drawn, so every
 * millisecond here is a frozen UI. Both ends of the window sit on a frame-count
 * edge, so the error is one poll interval whatever the window length; 100 ms is
 * therefore enough, at 60 Hz about six frames. The edge timeout must still cover
 * one whole frame at the 10 Hz floor of the plausibility check (100 ms), and
 * 120 ms does. The worst case, a counter that stops, costs one timeout. */
#define POLARIS_REFRESH_WINDOW_NS UINT64_C(100000000)
#define POLARIS_REFRESH_EDGE_TIMEOUT_NS UINT64_C(120000000)

struct polaris_display_ops {
    void *ctx;
    /* byte_offset is relative to BAR5; the callback performs one 32-bit read. */
    uint32_t (*read32)(void *ctx, uint32_t byte_offset);
    /* Monotonic nanoseconds. The kernel passes time_mono_ns(), which is TSC
     * backed once the TSC clocksource is calibrated. */
    uint64_t (*now_ns)(void *ctx);
#ifdef POLARIS_DISPLAY_NEGCTL_WRITE
    /* Watched mutation only: the production struct has no write member. */
    void (*write32)(void *ctx, uint32_t byte_offset, uint32_t value);
#endif
};

enum polaris_display_result {
    POLARIS_DISPLAY_OK = 0,
    POLARIS_DISPLAY_BAD_ARGUMENT = -1,
    POLARIS_DISPLAY_BAR5_TOO_SMALL = -2,
    POLARIS_DISPLAY_DEAD_READ = -3
};

struct polaris_crtc_state {
    unsigned index;
    unsigned master_enabled;   /* CRTC_CONTROL.CRTC_MASTER_EN */
    unsigned graphics_enabled; /* GRPH_ENABLE.GRPH_ENABLE */
    unsigned active;           /* both of the above: this CRTC is scanning out */
    /* Linear, 8/16/32/64 bpp and a sane pitch and height. An active CRTC with
     * this clear has a scanout whose size cannot be computed. */
    unsigned scanout_known;
    uint32_t horizontal_total; /* stored value + 1 */
    uint32_t vertical_total;
    uint32_t width;            /* BLANK_START - BLANK_END, i.e. the active area */
    uint32_t height;
    unsigned bits_per_pixel;
    uint32_t pitch_bytes;      /* GRPH_PITCH is in pixels */
    uint64_t surface_mc;       /* GRPH_PRIMARY_SURFACE_ADDRESS(_HIGH), MC address */
    uint64_t scanout_bytes;    /* pitch_bytes * height, before any rounding */
    unsigned cursor_enabled;   /* CUR_CONTROL.CURSOR_EN */
    uint64_t cursor_mc;
    uint64_t cursor_bytes;     /* ARGB, width * height * 4 */
    uint32_t frame_count;      /* CRTC_STATUS_FRAME_COUNT, first sample */
};

struct polaris_display_scan {
    struct polaris_crtc_state crtc[POLARIS_DISPLAY_CRTC_COUNT];
    unsigned active_count;
    unsigned vga_mode_enabled; /* D1VGA_CONTROL.D1VGA_MODE_ENABLE */
    uint32_t viewport_width;   /* VIEWPORT_SIZE of CRTC0, as gmc_v8_0 reads it */
    uint32_t viewport_height;
};

/* Read every CRTC once. Tells the caller what is displayed; it never decides
 * whether that is acceptable. bar5_bytes is the real BAR5 length, checked here
 * because the display registers sit far above what native.c requires. Returns
 * DEAD_READ when CRTC_CONTROL reads all ones (device not answering). Failure
 * leaves *out untouched. */
int polaris_display_scan(const struct polaris_display_ops *ops,
                         uint64_t bar5_bytes, struct polaris_display_scan *out);

const char *polaris_display_result_name(int result);

struct polaris_display_gop {
    uint64_t lfb;           /* GOP framebuffer, CPU physical */
    uint64_t bar0_physical; /* CPU physical base of BAR0 */
    uint64_t mc_base;       /* MC_VM_FB_LOCATION base of the VRAM */
    uint32_t pitch_bytes;   /* lfb_bytes / height from the boot information */
};

enum polaris_gop_result {
    POLARIS_GOP_MATCH = 0,
    POLARIS_GOP_BAD_ARGUMENT = -1,
    POLARIS_GOP_NO_CRTC = -2,        /* no active CRTC scans the GOP address */
    POLARIS_GOP_PITCH_MISMATCH = -3, /* address matches, pitch does not */
    POLARIS_GOP_NOT_LINEAR_32BPP = -4
};

/* Find the CRTC that scans out the GOP framebuffer: its surface minus the MC
 * base must equal the LFB minus the BAR0 base, and its pitch must equal the
 * GOP's. A mismatch means the scan cannot be trusted to describe the screen the
 * user is looking at, and the caller must refuse to take the display over. */
int polaris_display_find_gop(const struct polaris_display_scan *scan,
                             const struct polaris_display_gop *gop,
                             unsigned *crtc_index);
const char *polaris_gop_result_name(int result);

enum polaris_refresh_result {
    POLARIS_REFRESH_OK = 0,
    POLARIS_REFRESH_BAD_ARGUMENT = -1,
    POLARIS_REFRESH_COUNTER_STOPPED = -2, /* no frame edge within the timeout */
    POLARIS_REFRESH_CLOCK_STUCK = -3,     /* the time source stopped advancing */
    POLARIS_REFRESH_IMPLAUSIBLE = -4,     /* outside 10 Hz .. 1 kHz */
    POLARIS_REFRESH_BAR5_TOO_SMALL = -5   /* the frame counter lies past BAR5 */
};

struct polaris_refresh {
    int result;
    uint32_t frames;
    uint64_t elapsed_ns;
    uint32_t millihertz;
};

/* Count frames of one CRTC over at least POLARIS_REFRESH_WINDOW_NS. Both ends
 * are aligned to a frame-count edge so the quotient is exact to one poll
 * interval instead of one frame. Every wait is bounded by time and by a poll
 * budget, so a counter that stops, a CRTC that is off or a clock that stops
 * returns an error instead of hanging boot. bar5_bytes is the real BAR5 length
 * and is checked here, like polaris_display_scan does, so this entry point is
 * safe to call on its own: the frame counter of CRTC5 sits at the very end of
 * the display registers. Read-only. */
int polaris_display_measure_refresh(const struct polaris_display_ops *ops,
                                    uint64_t bar5_bytes, unsigned crtc_index,
                                    struct polaris_refresh *out);
const char *polaris_refresh_result_name(int result);

/* One log or HUD line body for a CRTC, without the "[amd-dce] " prefix.
 * refresh may be NULL for a CRTC whose refresh was not measured. Uses the
 * kernel formatter ksnprintf so the serial line and the HUD cannot differ. */
void polaris_display_format_crtc(char *out, int capacity,
                                 const struct polaris_crtc_state *crtc,
                                 const struct polaris_refresh *refresh,
                                 int is_gop);

#endif
