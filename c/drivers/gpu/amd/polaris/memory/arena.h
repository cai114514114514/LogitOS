#ifndef LOGIT_POLARIS_MEMORY_ARENA_H
#define LOGIT_POLARIS_MEMORY_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include "amd/polaris/display/crtc.h"
#include "amd/polaris/memory/layout.h"
#include "amd/polaris/resource/atom.h"

/* VRAM reservation inventory and arena selection from what the live hardware
 * and the VBIOS say is in use. Pure planning: no MMIO, no allocation, and no
 * ownership grant (layout.h: an arena is only usable once the platform really
 * owns it). The result is evidence for the HUD and the input B3 needs.
 *
 * Sources of the inventory:
 *   - every active scanout and enabled cursor surface, from crtc.c, widened to
 *     64 KiB boundaries;
 *   - the whole GOP framebuffer the firmware handed over (Multiboot LFB), which
 *     can be taller than the CRTC's active area: the CRTC protects only
 *     pitch x active height, while fb.c keeps drawing into every byte of the
 *     LFB. The CRTC range alone would leave the unscanned tail free to hand out;
 *   - VRAM_UsageByFirmware, from the existing atom.c parser;
 *   - the VBIOS framebuffer at the start of VRAM, sized exactly as Linux v6.12
 *     gmc_v8_0_get_vbios_fb_size() and amdgpu_ttm.c (stolen_vga_memory at
 *     offset 0): 9 MiB (AMDGPU_VBIOS_VGA_ALLOCATION, amdgpu.h) when
 *     D1VGA_MODE_ENABLE is set, else VIEWPORT_WIDTH * VIEWPORT_HEIGHT * 4. */

#define POLARIS_ARENA_ALIGNMENT (UINT64_C(2) << 20)
#define POLARIS_ARENA_RESERVE_GRANULE (UINT64_C(64) << 10)
#define POLARIS_ARENA_VGA_ALLOCATION (UINT64_C(9) << 20)
/* runtime.c rejects staging_bytes < 8192, and present.c wants the same. */
#define POLARIS_ARENA_STAGING_BYTES 8192u

enum polaris_arena_reserve_kind {
    POLARIS_ARENA_RESERVE_SCANOUT,
    POLARIS_ARENA_RESERVE_CURSOR,
    POLARIS_ARENA_RESERVE_VBIOS_FIRMWARE,
    POLARIS_ARENA_RESERVE_VBIOS_DRIVER_SCRATCH,
    POLARIS_ARENA_RESERVE_VBIOS_FRAMEBUFFER,
    POLARIS_ARENA_RESERVE_GOP_FRAMEBUFFER
};

/* Parallel arrays so range[] can be handed straight to polaris_memory_request
 * as its `reserved` list. */
struct polaris_arena_inventory {
    struct polaris_memory_range range[POLARIS_MEMORY_MAX_RESERVED];
    enum polaris_arena_reserve_kind kind[POLARIS_MEMORY_MAX_RESERVED];
    unsigned crtc[POLARIS_MEMORY_MAX_RESERVED]; /* only meaningful for CRTC items */
    size_t count;
};

enum polaris_arena_result {
    POLARIS_ARENA_OK = 0,
    POLARIS_ARENA_BAD_ARGUMENT = -1,
    POLARIS_ARENA_NO_GOP = -2,           /* the GOP CRTC was not identified */
    POLARIS_ARENA_VBIOS_UNKNOWN = -3,    /* no parsed VRAM_UsageByFirmware */
    POLARIS_ARENA_UNKNOWN_SCANOUT = -4,  /* an active CRTC has no computable size */
    POLARIS_ARENA_TOO_MANY = -5,         /* more reservations than the planner takes */
    POLARIS_ARENA_OUTSIDE_VRAM = -6,     /* a reservation leaves the VRAM range */
    POLARIS_ARENA_MISALIGNED = -7,       /* aperture not 2 MiB aligned (MC or CPU) */
    POLARIS_ARENA_REQUIREMENT = -8,      /* the runtime layout cannot be sized */
    POLARIS_ARENA_NO_FIT = -9,           /* no free 2 MiB aligned window */
    POLARIS_ARENA_PLAN_REJECTED = -10,   /* polaris_memory_plan refused the result */
    POLARIS_ARENA_GOP_UNSIZED = -11      /* the GOP framebuffer length is unknown */
};

struct polaris_arena_inputs {
    const struct polaris_display_scan *scan;
    int gop_crtc;                                  /* index into scan, or -1 */
    /* The whole GOP framebuffer in MC addresses (surface base, LFB length from
     * the boot information). Zero bytes is a refusal, not "absent": without it
     * the tail of a framebuffer taller than the CRTC's active area is unknown. */
    struct polaris_memory_range gop_framebuffer;
    const struct polaris_atom_reservations *vbios; /* NULL: not parsed */
    struct polaris_memory_range vram;              /* MC_VM_FB_LOCATION window */
    uint64_t aperture_bytes;                       /* BAR0 length */
    uint64_t aperture_cpu_base;                    /* BAR0 CPU physical base */
    uint64_t firmware_bytes;                       /* polaris_fw_bundle_plan bytes_used */
};

struct polaris_arena_plan {
    struct polaris_arena_inventory inventory;
    struct polaris_memory_range aperture; /* CPU-visible prefix of VRAM, MC addresses */
    /* Union of the GOP CRTC's scanout and the whole GOP framebuffer, which is
     * what polaris_memory_plan must keep the arena away from. */
    struct polaris_memory_range scanout;
    struct polaris_memory_range arena;    /* chosen window, MC addresses */
    uint64_t arena_cpu_physical;          /* BAR0 base + offset of arena.base */
    uint64_t required_bytes;              /* runtime layout size before rounding */
    struct polaris_memory_layout layout;  /* polaris_memory_plan of the arena */
};

/* Build the inventory and choose the arena. The inventory is filled whenever
 * it could be built, even if the arena is then refused, so the caller can show
 * what blocked it. A refusal is the safe answer; nothing here retries with a
 * smaller or less careful policy. Failure leaves arena/layout zeroed. */
int polaris_arena_plan_build(const struct polaris_arena_inputs *inputs,
                             struct polaris_arena_plan *out);
const char *polaris_arena_result_name(int result);

/* Check a mapping callback against the plan: for the arena and for each layout
 * object the callback (same shape as polaris_platform.resolve_mapping) must
 * return bar0_cpu + (gpu_address - aperture.base). Catches an adapter that
 * resolves MC addresses against the wrong base. Returns 0 when all agree. */
typedef int (*polaris_arena_resolve_fn)(void *ctx, uint64_t gpu_address,
                                        uint64_t bytes, volatile uint8_t **cpu);
int polaris_arena_check_mapping(const struct polaris_arena_plan *plan,
                                polaris_arena_resolve_fn resolve, void *ctx,
                                volatile uint8_t *bar0_cpu);

/* One log or HUD line body for the arena, without the "[amd-arena] " prefix.
 * result is what polaris_arena_plan_build returned. */
void polaris_arena_format(char *out, int capacity, int result,
                          const struct polaris_arena_plan *plan);


/* The whole read-only survey in one call, so the kernel glue and the host gate
 * run the same sequence: scan the CRTCs, identify the GOP CRTC, measure its
 * refresh, then inventory and choose the arena. A stage that fails stops the
 * stages that depend on it (no GOP CRTC means no arena) but never the ones that
 * do not (a frozen frame counter still lets the arena be planned). */
struct polaris_survey_inputs {
    const struct polaris_display_ops *ops;
    uint64_t bar5_bytes;
    struct polaris_display_gop gop;
    uint64_t lfb_bytes;                            /* GOP framebuffer length, bytes */
    const struct polaris_atom_reservations *vbios; /* NULL: not parsed */
    struct polaris_memory_range vram;
    uint64_t aperture_bytes;
    uint64_t aperture_cpu_base;
    uint64_t firmware_bytes;
};

struct polaris_survey {
    uint64_t bar5_bytes;
    int scan_result;
    struct polaris_display_scan scan;
    int gop_result;
    unsigned gop_crtc;
    unsigned refresh_attempted;
    struct polaris_refresh refresh;
    int arena_result;
    struct polaris_arena_plan plan;
};

void polaris_arena_survey(const struct polaris_survey_inputs *inputs,
                          struct polaris_survey *out);

/* Line body for the GOP CRTC (or the reason there is none), without the
 * "[amd-dce] " prefix; the serial log and the HUD print the same text. */
void polaris_survey_format_gop(char *out, int capacity,
                               const struct polaris_survey *survey);

#endif
