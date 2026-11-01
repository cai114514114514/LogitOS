#include "amd/polaris/memory/gmc_aperture.h"

/* Register word indices from Linux v6.12
 * drivers/gpu/drm/amd/include/asic_reg/gmc/gmc_8_1_d.h and oss/oss_3_0_d.h;
 * BAR5 byte offset = index * 4. */
#define REGISTER_WORD_BYTES 4u
#define BYTE_OFFSET(word_index) ((uint32_t)(word_index) * REGISTER_WORD_BYTES)
#define MM_MC_VM_AGP_TOP                       0x80au
#define MM_MC_VM_AGP_BOT                       0x80bu
#define MM_MC_VM_SYSTEM_APERTURE_LOW_ADDR      0x80du
#define MM_MC_VM_SYSTEM_APERTURE_HIGH_ADDR     0x80eu
#define MM_MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR  0x80fu
#define MM_MC_VM_MX_L1_TLB_CNTL                0x819u
#define MM_MC_SHARED_BLACKOUT_CNTL             0x82bu
#define MM_VM_CONTEXT0_CNTL                    0x504u
#define MM_VM_CONTEXT1_CNTL                    0x505u
#define MM_VM_INVALIDATE_REQUEST               0x51eu
#define MM_VM_CONTEXT0_PROTECTION_FAULT_STATUS 0x536u
#define MM_SRBM_STATUS                         0x394u

/* gmc_8_1_sh_mask.h MC_VM_MX_L1_TLB_CNTL fields. The Linux value is
 * SYSTEM_ACCESS_MODE = 3 (NOT_IN_SYS, radeon r600d.h names), unmapped access 0
 * (PASS_THRU) and the advanced driver model on (gmc_v8_0.c gart_enable). */
#define L1_SYSTEM_ACCESS_MODE_MASK             0x18u
#define L1_SYSTEM_ACCESS_MODE_NOT_IN_SYSTEM    0x18u
#define L1_SYSTEM_APERTURE_UNMAPPED_ACCESS     0x20u
#define L1_ENABLE_ADVANCED_DRIVER_MODEL        0x40u
#define L1_ACCESS_FIELDS_MASK (L1_SYSTEM_ACCESS_MODE_MASK | L1_SYSTEM_APERTURE_UNMAPPED_ACCESS | \
                               L1_ENABLE_ADVANCED_DRIVER_MODEL)
#define L1_ACCESS_FIELDS_LINUX (L1_SYSTEM_ACCESS_MODE_NOT_IN_SYSTEM | L1_ENABLE_ADVANCED_DRIVER_MODEL)
#define L1_ACCESS_FIELDS_GOP   0u     /* PA_ONLY, pass-through, no driver model */

#define VM_CONTEXT_ENABLE_CONTEXT              0x1u   /* VM_CONTEXTn_CNTL bit 0 */
#define MC_SHARED_BLACKOUT_MODE_MASK           0x7u
/* oss_3_0_sh_mask.h: the busy bits gmc_v8_0_wait_for_idle polls. */
#define SRBM_STATUS_MEMORY_CONTROLLER_BUSY     (0x100u | 0x200u | 0x400u | 0x800u | 0x1000u | 0x2000u)
/* adev->usec_timeout, AMDGPU_MAX_USEC_TIMEOUT in amdgpu.h. */
#define MC_IDLE_TIMEOUT_US                     100000u
/* VM_INVALIDATE_REQUEST bits 0-15 are VM contexts 0-15 (gmc_v8_0_flush_gpu_tlb). */
#define INVALIDATE_VM_CONTEXT0                 0x1u
/* Aperture registers hold 4 KiB page numbers; AGP TOP/BOT hold 4 MiB units
 * (gmc_v8_0.c mc_program writes agp_end >> 22). */
#define APERTURE_PAGE_SHIFT                    12u
#define AGP_UNIT_SHIFT                         22u

/* Why a named hook per watched mutation: each negative control removes or
 * reorders exactly one decision, and the gate watches its assertion go red. */
static int check_predicate(void)
{
#ifdef GMC_APERTURE_NEGCTL_SKIP_PREDICATE
    return 0;
#else
    return 1;
#endif
}

/* Same reason; see GMC_APERTURE_NEGCTL_MODE_BEFORE_APERTURE in the gate. */
static int mode_goes_first(void)
{
#ifdef GMC_APERTURE_NEGCTL_MODE_BEFORE_APERTURE
    return 1;
#else
    return 0;
#endif
}

/* Same reason; see GMC_APERTURE_NEGCTL_RESTORE_APERTURE_FIRST in the gate. */
static int restore_aperture_first(void)
{
#ifdef GMC_APERTURE_NEGCTL_RESTORE_APERTURE_FIRST
    return 1;
#else
    return 0;
#endif
}

/* Why the half-open test: both ranges are [start, end) in MC bytes. */
static int ranges_overlap(uint64_t first_start, uint64_t first_end,
                          uint64_t second_start, uint64_t second_end)
{
    return first_start < second_end && second_start < first_end;
}

/* Read one register into *VALUE; nonzero when the port refused. */
static int read_register(const struct gmc_register_port *port, uint32_t word_index, uint32_t *value)
{
    return port->read(port->context, BYTE_OFFSET(word_index), value);
}

/* Write one register; nonzero when the port refused. */
static int write_register(const struct gmc_register_port *port, uint32_t word_index, uint32_t value)
{
    return port->write(port->context, BYTE_OFFSET(word_index), value);
}

/* Write, then read back: a write that does not stick must be seen here, not
 * discovered by a DMA engine later. Nonzero on access failure or mismatch. */
static int write_and_confirm(const struct gmc_register_port *port, uint32_t word_index, uint32_t value)
{
    uint32_t read_back = 0;
    if (write_register(port, word_index, value)) {
        return 1;
    }
    if (read_register(port, word_index, &read_back)) {
        return 1;
    }
    return read_back != value;
}

/* The bounded wait of gmc_v8_0_wait_for_idle before mc_program writes the
 * aperture. Linux only warns when it expires, and so does this. Returns
 * nonzero when the wait expired (or SRBM_STATUS could not be read). */
static int wait_for_memory_controller_idle(const struct gmc_register_port *port)
{
    uint64_t started_us = port->now_us(port->context);
    for (;;) {
        uint32_t status = 0;
        if (read_register(port, MM_SRBM_STATUS, &status)) {
            return 1;
        }
        if (!(status & SRBM_STATUS_MEMORY_CONTROLLER_BUSY)) {
            return 0;
        }
        if (port->now_us(port->context) - started_us >= MC_IDLE_TIMEOUT_US) {
            return 1;
        }
    }
}

/* Why: one checked pass over the read-only facts that decide whether touching
 * these registers is safe. Returns GMC_APERTURE_PROGRAMMED when it is, or the
 * named refusal. */
static enum gmc_aperture_outcome check_state(const struct gmc_register_port *port,
                                             uint64_t vram_start, uint64_t vram_end,
                                             uint32_t l1_tlb_control)
{
    uint32_t context0 = 0;
    uint32_t context1 = 0;
    uint32_t agp_top = 0;
    uint32_t agp_bottom = 0;
    uint32_t fault_status = 0;
    uint32_t blackout = 0;
    if (read_register(port, MM_VM_CONTEXT0_CNTL, &context0) ||
        read_register(port, MM_VM_CONTEXT1_CNTL, &context1) ||
        read_register(port, MM_MC_VM_AGP_TOP, &agp_top) ||
        read_register(port, MM_MC_VM_AGP_BOT, &agp_bottom) ||
        read_register(port, MM_VM_CONTEXT0_PROTECTION_FAULT_STATUS, &fault_status) ||
        read_register(port, MM_MC_SHARED_BLACKOUT_CNTL, &blackout)) {
        return GMC_APERTURE_FAILED_REGISTER_ACCESS;
    }
    if (!check_predicate()) {
        return GMC_APERTURE_PROGRAMMED;
    }
    if ((l1_tlb_control & L1_ACCESS_FIELDS_MASK) != L1_ACCESS_FIELDS_GOP) {
        return GMC_APERTURE_REFUSED_UNKNOWN_MODE;
    }
    if ((context0 & VM_CONTEXT_ENABLE_CONTEXT) || (context1 & VM_CONTEXT_ENABLE_CONTEXT)) {
        return GMC_APERTURE_REFUSED_CONTEXT_ENABLED;
    }
    /* AGP is disabled when BOT > TOP (mc_program writes TOP = 0, BOT = all
     * ones to disable it); an enabled AGP range must not overlap VRAM. */
    int agp_enabled = agp_bottom <= agp_top;
    uint64_t agp_start = (uint64_t)agp_bottom << AGP_UNIT_SHIFT;
    uint64_t agp_end = ((uint64_t)agp_top + 1u) << AGP_UNIT_SHIFT;
    if (agp_enabled && ranges_overlap(agp_start, agp_end, vram_start, vram_end)) {
        return GMC_APERTURE_REFUSED_AGP_OVERLAPS_VRAM;
    }
    if (fault_status) {
        return GMC_APERTURE_REFUSED_PROTECTION_FAULT;
    }
    if (blackout & MC_SHARED_BLACKOUT_MODE_MASK) {
        return GMC_APERTURE_REFUSED_BLACKOUT;
    }
    return GMC_APERTURE_PROGRAMMED;
}

/* Why: the three aperture writes in Linux mc_program order (LOW, HIGH,
 * DEFAULT), each confirmed. Nonzero when one did not stick. */
static int program_aperture(const struct gmc_register_port *port, uint32_t low_page,
                            uint32_t high_page, uint32_t default_page)
{
    if (write_and_confirm(port, MM_MC_VM_SYSTEM_APERTURE_LOW_ADDR, low_page)) {
        return 1;
    }
    if (write_and_confirm(port, MM_MC_VM_SYSTEM_APERTURE_HIGH_ADDR, high_page)) {
        return 1;
    }
    return write_and_confirm(port, MM_MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, default_page);
}

/* Why: gart_enable's L1 access fields, then drop VMID0's cached translations
 * the way gmc_v8_0_flush_gpu_tlb does. Nonzero on failure or mismatch. */
static int program_access_mode(const struct gmc_register_port *port, uint32_t l1_tlb_control)
{
    uint32_t linux_value = (l1_tlb_control & ~L1_ACCESS_FIELDS_MASK) | L1_ACCESS_FIELDS_LINUX;
    if (write_and_confirm(port, MM_MC_VM_MX_L1_TLB_CNTL, linux_value)) {
        return 1;
    }
    return write_register(port, MM_VM_INVALIDATE_REQUEST, INVALIDATE_VM_CONTEXT0);
}

enum gmc_aperture_outcome gmc_aperture_prepare(const struct gmc_register_port *port,
                                               struct polaris_memory_range vram,
                                               uint64_t default_address,
                                               struct gmc_aperture_snapshot *snapshot,
                                               int *mc_idle_timed_out)
{
    if (!port || !port->read || !port->write || !port->now_us || !snapshot || !mc_idle_timed_out ||
        !vram.bytes || vram.base + vram.bytes < vram.base ||
        (vram.base | vram.bytes) & ((UINT64_C(1) << APERTURE_PAGE_SHIFT) - 1u) ||
        default_address < vram.base || default_address >= vram.base + vram.bytes) {
        return GMC_APERTURE_REFUSED_BAD_ARGUMENT;
    }
    *snapshot = (struct gmc_aperture_snapshot){0};
    *mc_idle_timed_out = 0;
    uint64_t vram_end = vram.base + vram.bytes;
    uint32_t low_page = (uint32_t)(vram.base >> APERTURE_PAGE_SHIFT);
    uint32_t high_page = (uint32_t)((vram_end - 1u) >> APERTURE_PAGE_SHIFT);  /* inclusive, as vram_end */
    uint32_t default_page = (uint32_t)(default_address >> APERTURE_PAGE_SHIFT);
    if (read_register(port, MM_MC_VM_SYSTEM_APERTURE_LOW_ADDR, &snapshot->aperture_low_page) ||
        read_register(port, MM_MC_VM_SYSTEM_APERTURE_HIGH_ADDR, &snapshot->aperture_high_page) ||
        read_register(port, MM_MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, &snapshot->aperture_default_page) ||
        read_register(port, MM_MC_VM_MX_L1_TLB_CNTL, &snapshot->l1_tlb_control)) {
        return GMC_APERTURE_FAILED_REGISTER_ACCESS;
    }
    int linux_mode = (snapshot->l1_tlb_control & L1_ACCESS_FIELDS_MASK) == L1_ACCESS_FIELDS_LINUX;
    int aperture_covers_vram = snapshot->aperture_low_page <= low_page &&
                               snapshot->aperture_high_page >= high_page;
    if (linux_mode && aperture_covers_vram) {
        return GMC_APERTURE_ALREADY_CONFIGURED;
    }
    enum gmc_aperture_outcome checked = check_state(port, vram.base, vram_end,
                                                    snapshot->l1_tlb_control);
    if (checked != GMC_APERTURE_PROGRAMMED) {
        return checked;
    }
    *mc_idle_timed_out = wait_for_memory_controller_idle(port);
    /* From here on registers change: restore() must undo whatever got in. */
    snapshot->programmed = 1;
    int failed;
    if (mode_goes_first()) {
        failed = program_access_mode(port, snapshot->l1_tlb_control) ||
                 program_aperture(port, low_page, high_page, default_page);
    } else {
        failed = program_aperture(port, low_page, high_page, default_page) ||
                 program_access_mode(port, snapshot->l1_tlb_control);
    }
    if (failed) {
        (void)gmc_aperture_restore(port, snapshot);
        return GMC_APERTURE_REFUSED_READBACK_MISMATCH;
    }
    return GMC_APERTURE_PROGRAMMED;
}

/* Why: the three aperture registers back to the firmware's values. */
static int restore_aperture(const struct gmc_register_port *port,
                            const struct gmc_aperture_snapshot *snapshot)
{
    int failed = write_register(port, MM_MC_VM_SYSTEM_APERTURE_HIGH_ADDR, snapshot->aperture_high_page);
    failed |= write_register(port, MM_MC_VM_SYSTEM_APERTURE_LOW_ADDR, snapshot->aperture_low_page);
    failed |= write_register(port, MM_MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR,
                             snapshot->aperture_default_page);
    return failed;
}

/* Why: the L1 access fields back to the firmware's value, then invalidate. */
static int restore_access_mode(const struct gmc_register_port *port,
                               const struct gmc_aperture_snapshot *snapshot)
{
    int failed = write_register(port, MM_MC_VM_MX_L1_TLB_CNTL, snapshot->l1_tlb_control);
    failed |= write_register(port, MM_VM_INVALIDATE_REQUEST, INVALIDATE_VM_CONTEXT0);
    return failed;
}

int gmc_aperture_restore(const struct gmc_register_port *port,
                         struct gmc_aperture_snapshot *snapshot)
{
    if (!port || !snapshot) {
        return -1;
    }
    if (!snapshot->programmed) {
        return 0;
    }
    int failed;
    if (restore_aperture_first()) {
        failed = restore_aperture(port, snapshot);
        failed |= restore_access_mode(port, snapshot);
    } else {
        failed = restore_access_mode(port, snapshot);
        failed |= restore_aperture(port, snapshot);
    }
    snapshot->programmed = 0;
    return failed ? -1 : 0;
}

const char *gmc_aperture_outcome_name(enum gmc_aperture_outcome outcome)
{
    static const char *const names[GMC_APERTURE_OUTCOME_COUNT] = {
        [GMC_APERTURE_PROGRAMMED] = "programmed",
        [GMC_APERTURE_ALREADY_CONFIGURED] = "already-linux",
        [GMC_APERTURE_REFUSED_BAD_ARGUMENT] = "refused:bad-argument",
        [GMC_APERTURE_REFUSED_UNKNOWN_MODE] = "refused:unknown-l1-mode",
        [GMC_APERTURE_REFUSED_CONTEXT_ENABLED] = "refused:vm-context-enabled",
        [GMC_APERTURE_REFUSED_AGP_OVERLAPS_VRAM] = "refused:agp-overlaps-vram",
        [GMC_APERTURE_REFUSED_PROTECTION_FAULT] = "refused:vmid0-fault-latched",
        [GMC_APERTURE_REFUSED_BLACKOUT] = "refused:mc-blackout",
        [GMC_APERTURE_REFUSED_READBACK_MISMATCH] = "refused:write-did-not-stick",
        [GMC_APERTURE_FAILED_REGISTER_ACCESS] = "failed:register-access",
    };
    if ((unsigned)outcome >= GMC_APERTURE_OUTCOME_COUNT || !names[outcome]) {
        return "unknown";
    }
    return names[outcome];
}
