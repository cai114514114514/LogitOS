#ifndef LOGIT_POLARIS_GMC_APERTURE_H
#define LOGIT_POLARIS_GMC_APERTURE_H
#include <stdint.h>
#include "amd/polaris/memory/layout.h"

/* The memory controller's system aperture and L1 access mode, programmed the
 * way Linux v6.12 gmc_v8_0.c does it (mc_program, then gart_enable), and
 * nothing else of the GMC.
 *
 * WHY THIS EXISTS. On the real X79-VG4 (2026-11-01) the RX 580 was initialised
 * only by the board's UEFI GOP, which leaves MC_VM_MX_L1_TLB_CNTL = 0x503
 * (SYSTEM_ACCESS_MODE 0, PA_ONLY) and a zero system aperture. Linux never runs
 * SDMA or the SMU's firmware DMA in that state: it always points the aperture
 * at exactly VRAM first and then selects mode 3 (NOT_IN_SYS: inside the
 * aperture is physical, outside goes through the VMID0 page table), with the
 * GOP scanout still live and no blackout. The design said "do not take over
 * the GMC"; the owner approved this one narrow exception on 2026-11-01
 * (docs/superpowers/specs/2026-11-01-rx580-driver-design.md, section 4
 * addendum): these four registers and a TLB invalidate, only after the
 * read-only checks below pass, restored in reverse order on failure and at
 * quiesce.
 *
 * Pure logic over a register port, so the host gate drives it with a model and
 * records every access and its order. Not verified on X79-VG4 hardware. */

/* One 32-bit register port in BAR5 byte offsets, plus the clock for the
 * bounded idle wait. read/write return 0 on success. */
struct gmc_register_port {
    void *context;
    int (*read)(void *context, uint32_t byte_offset, uint32_t *value);
    int (*write)(void *context, uint32_t byte_offset, uint32_t value);
    uint64_t (*now_us)(void *context);
};

/* What the firmware had, so the exact values can be put back. */
struct gmc_aperture_snapshot {
    uint32_t aperture_low_page;
    uint32_t aperture_high_page;
    uint32_t aperture_default_page;
    uint32_t l1_tlb_control;
    int programmed;                  /* nonzero once our values are in place */
};

enum gmc_aperture_outcome {
    GMC_APERTURE_PROGRAMMED = 0,
    GMC_APERTURE_ALREADY_CONFIGURED,         /* Linux state already present: no write */
    GMC_APERTURE_REFUSED_BAD_ARGUMENT,
    GMC_APERTURE_REFUSED_UNKNOWN_MODE,       /* neither the GOP state nor the Linux state */
    GMC_APERTURE_REFUSED_CONTEXT_ENABLED,    /* VM context 0 or 1 already translating */
    GMC_APERTURE_REFUSED_AGP_OVERLAPS_VRAM,
    GMC_APERTURE_REFUSED_PROTECTION_FAULT,   /* a VMID0 fault is already latched */
    GMC_APERTURE_REFUSED_BLACKOUT,           /* the MC is in a blackout */
    GMC_APERTURE_REFUSED_READBACK_MISMATCH,  /* a write did not stick; undone */
    GMC_APERTURE_FAILED_REGISTER_ACCESS,
    GMC_APERTURE_OUTCOME_COUNT
};

/* Check, then (only if every check passes) program the aperture to exactly
 * VRAM and select the Linux access mode. VRAM is the MC_VM_FB_LOCATION window;
 * DEFAULT_ADDRESS is a VRAM page for unmapped accesses (Linux uses its scratch
 * page). *MC_IDLE_TIMED_OUT is set when the bounded idle wait expired: like
 * Linux, that is reported and not fatal. On any refusal nothing was written. */
enum gmc_aperture_outcome gmc_aperture_prepare(const struct gmc_register_port *port,
                                               struct polaris_memory_range vram,
                                               uint64_t default_address,
                                               struct gmc_aperture_snapshot *snapshot,
                                               int *mc_idle_timed_out);

/* Put the firmware's values back in reverse order: L1 mode first (so a VRAM
 * address is never routed through the page table), invalidate, then the
 * aperture. Nothing to do unless SNAPSHOT says we programmed. 0 on success. */
int gmc_aperture_restore(const struct gmc_register_port *port,
                         struct gmc_aperture_snapshot *snapshot);

const char *gmc_aperture_outcome_name(enum gmc_aperture_outcome outcome);

#endif
