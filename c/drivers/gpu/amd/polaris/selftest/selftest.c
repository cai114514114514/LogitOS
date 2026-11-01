/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "amd/polaris/selftest/selftest.h"
#include "amd/polaris/pci_id.h"
#include "amd/polaris/memory/gmc_aperture.h"
#include "kprintf.h"

/* The stage exists only in HWHUD builds. The guard wraps the whole file, so a
 * plain kernel links an empty object and the host gate can prove it by looking
 * for the entry point in the symbol table. */
#ifdef POLARIS_SELFTEST_COMPILED

/* PCI Command register: byte offset 0x04 of the configuration header (PCI Local
 * Bus Specification 3.0, section 6.2.2). Bit 1 enables memory decode and bit 2
 * bus mastering; the native platform insists on both. */
#define PCI_COMMAND_OFFSET 0x04u
/* The longest stage detail is a failed smu line: error, reason, load status,
 * quarantine flag, upload time and the previous-response counters. */
#define LOG_DETAIL_BYTES 200
#define BIND_DIAGNOSTICS_BYTES 240
#define SMC_LABEL_BYTES 24
/* "<label>/<8 hex digits>" and its terminator. */
#define HUD_SMC_TEXT_BYTES (SMC_LABEL_BYTES + 12)
/* A 64-bit decimal count and its terminator. */
#define HUD_SDMA_TEXT_BYTES 24
/* " reason=" and the longest SMU loader error name ("unsupported-state"). */
#define SMU_REASON_TEXT_BYTES 32
/* " prev-first=0xffff" and its terminator, rounded up. */
#define SMU_FIRST_RESPONSE_TEXT_BYTES 24
/* "upload-us=18446744073709551615 prev-anomalies=4294967295" is 56 characters,
 * plus the first-response text and a terminator. */
#define SMU_FACTS_TEXT_BYTES (56 + SMU_FIRST_RESPONSE_TEXT_BYTES + 1)
/* " pre=cold-unprotected fw=ffffffff clk=ffffffff pc=ffffffff rst=ffffffff
 * fl=ffffffff" is 83 characters; one more for the terminator, rounded up. */
#define HUD_PREFLIGHT_TEXT_BYTES 96
#define SMC_FILE_PREFIX "polaris10_"
#define SMC_FILE_SUFFIX ".bin"

/* SDMA0 ring read and write pointers, BAR5 byte offsets: Linux v6.12
 * sdma_v3_0.c reads mmSDMA0_GFX_RB_RPTR (0x3483) and writes
 * mmSDMA0_GFX_RB_WPTR (0x3484) from drivers/gpu/drm/amd/include/asic_reg/oss/
 * oss_3_0_d.h; sdma/queue.c uses the same two with the same index * 4. */
#define SDMA0_RING_READ_POINTER_REGISTER 0xd20cu
#define SDMA0_RING_WRITE_POINTER_REGISTER 0xd210u

/* Return value of a platform callback the lease guard refused to forward. */
enum guard_result {
    GUARD_ACCESS_REFUSED = -1
};

static const char *const stage_names[POLARIS_SELFTEST_STAGE_COUNT] = {
    [POLARIS_SELFTEST_STAGE_GATE] = "gate",
    [POLARIS_SELFTEST_STAGE_LEASE] = "lease",
    [POLARIS_SELFTEST_STAGE_MAP] = "map",
    [POLARIS_SELFTEST_STAGE_BIND] = "bind",
    [POLARIS_SELFTEST_STAGE_MAPPING] = "mapping",
    [POLARIS_SELFTEST_STAGE_KEY] = "key",
    [POLARIS_SELFTEST_STAGE_FIRMWARE] = "firmware",
    [POLARIS_SELFTEST_STAGE_SMU] = "smu",
    [POLARIS_SELFTEST_STAGE_SDMA] = "sdma",
    [POLARIS_SELFTEST_STAGE_QUIESCE] = "quiesce"
};

static const char *const result_names[POLARIS_SELFTEST_RESULT_COUNT] = {
    [POLARIS_SELFTEST_NOT_RUN] = "not-run",
    [POLARIS_SELFTEST_OK] = "ok",
    [POLARIS_SELFTEST_SKIPPED_UNKNOWN_DEVICE] = "skipped-unknown-device",
    [POLARIS_SELFTEST_SKIPPED_NOGPU] = "skipped-nogpu",
    [POLARIS_SELFTEST_SKIPPED_NO_ARENA] = "skipped-no-arena",
    [POLARIS_SELFTEST_SKIPPED_NO_FIRMWARE] = "skipped-no-firmware",
    [POLARIS_SELFTEST_FAILED_BAD_ARGUMENT] = "failed-bad-argument",
    [POLARIS_SELFTEST_FAILED_LEASE] = "failed-lease",
    [POLARIS_SELFTEST_FAILED_REGISTER_MAP] = "failed-register-map",
    [POLARIS_SELFTEST_FAILED_ARENA_MAP] = "failed-arena-map",
    [POLARIS_SELFTEST_FAILED_BIND] = "failed-bind",
    [POLARIS_SELFTEST_FAILED_MAPPING_CHECK] = "failed-mapping-check",
    [POLARIS_SELFTEST_FAILED_KEY_UNREADABLE] = "failed-key-unreadable",
    [POLARIS_SELFTEST_FAILED_FIRMWARE] = "failed-firmware",
    [POLARIS_SELFTEST_FAILED_NO_MEMORY] = "failed-no-memory",
    [POLARIS_SELFTEST_FAILED_RUNTIME_REFUSED] = "failed-runtime-refused",
    [POLARIS_SELFTEST_FAILED_MEMORY_PLAN] = "failed-memory-plan",
    [POLARIS_SELFTEST_FAILED_STAGING] = "failed-firmware-staging",
    [POLARIS_SELFTEST_FAILED_SMU] = "failed-smu",
    [POLARIS_SELFTEST_FAILED_SDMA] = "failed-sdma",
    [POLARIS_SELFTEST_FAILED_SDMA_BUSY] = "failed-sdma-busy",
    [POLARIS_SELFTEST_FAILED_QUARANTINED] = "failed-quarantined"
};

/* Why a table and not a switch: a missing entry shows up as a NULL the host
 * gate checks for, instead of a silent "unknown". */
const char *polaris_selftest_stage_name(enum polaris_selftest_stage stage)
{
    if ((unsigned)stage >= POLARIS_SELFTEST_STAGE_COUNT || !stage_names[stage]) {
        return "unknown";
    }
    return stage_names[stage];
}

/* Same table lookup for the result; see polaris_selftest_stage_name. */
const char *polaris_selftest_result_name(enum polaris_selftest_result result)
{
    if ((unsigned)result >= POLARIS_SELFTEST_RESULT_COUNT || !result_names[result]) {
        return "unknown";
    }
    return result_names[result];
}

/* ---- the lease guard ------------------------------------------------------ */

/* Every platform callback the runtime uses goes through this wrapper, so the
 * rule "no register access without the ownership lease" holds for the SMU
 * loader and the SDMA engine as well as for the key read, without trusting
 * each of them to check. The runtime keeps a copy of the platform struct for
 * the life of the system, hence static storage. */
struct lease_guard {
    struct polaris_platform inner;
    const struct polaris_selftest_ops *ops;
    unsigned refusals;
};

static struct lease_guard guard;
static struct polaris_platform guarded_platform;
/* Whether guarded_platform wraps a bound platform in this run. Quiesce reads
 * registers through it only then, and a stale guard from an earlier run must
 * not count. */
static int guard_is_installed;
/* The memory-controller values the firmware had before gmc_aperture_prepare
 * changed them (memory/gmc_aperture.h). Static, because quiesce runs from a
 * later call, at reboot or power-off. */
static struct gmc_aperture_snapshot memory_controller_snapshot;

/* Why one function: the watched mutation removes exactly this decision, and
 * the gate must see the unleased write get through. */
static int lease_allows_access(const struct lease_guard *checked)
{
#ifdef POLARIS_SELFTEST_NEGCTL_NO_LEASE_GUARD
    (void)checked;
    return 1;
#else
    return checked->ops->lease_held(checked->ops->context);
#endif
}

/* Why a wrapper per callback and not one macro: each forwards a different
 * signature to the platform underneath, and each must refuse the same way. */
static int guard_read_identity(void *opaque, uint32_t *value)
{
    struct lease_guard *checked = opaque;
    if (!lease_allows_access(checked)) {
        ++checked->refusals;
        return GUARD_ACCESS_REFUSED;
    }
    return checked->inner.read_identity(checked->inner.opaque, value);
}

/* Reads need the lease too: a register read can have side effects on this
 * hardware (the SMC indirect port), and an unleased owner has no business there. */
static int guard_read_register(void *opaque, uint32_t byte_offset, uint32_t *value)
{
    struct lease_guard *checked = opaque;
    if (!lease_allows_access(checked)) {
        ++checked->refusals;
        return GUARD_ACCESS_REFUSED;
    }
    return checked->inner.read_reg(checked->inner.opaque, byte_offset, value);
}

/* The write path the whole rule exists for. */
static int guard_write_register(void *opaque, uint32_t byte_offset, uint32_t value)
{
    struct lease_guard *checked = opaque;
    if (!lease_allows_access(checked)) {
        ++checked->refusals;
        return GUARD_ACCESS_REFUSED;
    }
    return checked->inner.write_reg(checked->inner.opaque, byte_offset, value);
}

/* Address translation is answered from live memory-controller reads (native.c
 * check_mapping), so it is a device access like the others. */
static int guard_resolve_mapping(void *opaque, uint64_t gpu_address, uint64_t bytes,
                                 volatile uint8_t **cpu)
{
    struct lease_guard *checked = opaque;
    if (!lease_allows_access(checked)) {
        ++checked->refusals;
        return GUARD_ACCESS_REFUSED;
    }
    return checked->inner.resolve_mapping(checked->inner.opaque, gpu_address, bytes, cpu);
}

/* sync flushes the HDP with register writes (native.c sync_mapping), so it is a
 * write path like any other. */
static int guard_sync(void *opaque, enum polaris_sdma_sync direction,
                      uint64_t gpu_address, uint64_t bytes)
{
    struct lease_guard *checked = opaque;
    if (!lease_allows_access(checked)) {
        ++checked->refusals;
        return GUARD_ACCESS_REFUSED;
    }
    return checked->inner.sync(checked->inner.opaque, direction, gpu_address, bytes);
}

/* The clock is not the device; it passes straight through. */
static uint64_t guard_now_us(void *opaque)
{
    struct lease_guard *checked = opaque;
    return checked->inner.now_us(checked->inner.opaque);
}

/* Replace the guard with one around INNER. Called once per run, after the
 * lease is held and the native platform is bound. */
static void guard_install(const struct polaris_platform *inner,
                          const struct polaris_selftest_ops *ops)
{
    guard_is_installed = 1;
    guard = (struct lease_guard){.inner = *inner, .ops = ops};
    guarded_platform = (struct polaris_platform){
        .opaque = &guard,
        .read_identity = guard_read_identity,
        .read_reg = guard_read_register,
        .write_reg = guard_write_register,
        .resolve_mapping = guard_resolve_mapping,
        .sync = guard_sync,
        .now_us = guard_now_us
    };
}

/* ---- logging -------------------------------------------------------------- */

/* One serial line per stage. DETAIL may be empty. The same text, shortened, is
 * what the HUD shows (polaris_selftest_format_hud), so the report is updated
 * in record() and nowhere else; this only prints, for the one stage line that
 * must not replace an earlier failure on the HUD (quiesce after a failed run). */
static void log_stage_line(enum polaris_selftest_stage stage,
                           enum polaris_selftest_result result, const char *detail)
{
    kprintf("[amd-polaris-runtime] stage=%s result=%s%s%s\n",
            polaris_selftest_stage_name(stage), polaris_selftest_result_name(result),
            detail[0] ? " " : "", detail);
}

/* Why the report is updated here as well: the HUD and the serial line must
 * never disagree about the last stage. */
static void record(struct polaris_selftest_report *report,
                   enum polaris_selftest_stage stage,
                   enum polaris_selftest_result result, const char *detail)
{
    report->stage = stage;
    report->result = result;
    log_stage_line(stage, result, detail);
}

/* ---- adapters between the environment and the native platform -------------- */

/* The native platform re-reads PCI configuration at the start of every batch of
 * register accesses (native.c) to notice a function that moved or vanished;
 * this hands it the environment's reader. */
static int native_read_pci32(void *opaque, uint16_t byte_offset, uint32_t *value)
{
    const struct polaris_selftest_ops *ops = opaque;
    return ops->read_pci_config(ops->context, byte_offset, value);
}

/* The native platform and the runtime time their waits with this clock. */
static uint64_t native_now_us(void *opaque)
{
    const struct polaris_selftest_ops *ops = opaque;
    return ops->now_us(ops->context);
}

/* ---- the stages ------------------------------------------------------------ */

struct run_state {
    const struct polaris_selftest_inputs *inputs;
    const struct polaris_selftest_ops *ops;
    struct polaris_selftest_report *report;
    volatile uint8_t *registers;     /* BAR5, uncached */
    volatile uint8_t *bar0_origin;   /* where BAR0 byte 0 would be; only the arena is mapped */
    struct polaris_native_resources resources;
    struct polaris_runtime_request request;
    struct polaris_platform native_platform;
};

/* Why one function: the owner's switch is obeyed in exactly one place, so the
 * watched mutation can make the marker be read and then ignored and the gate
 * can watch the run go ahead on a stick that carries it. */
static int opt_out_stops_the_run(const struct run_state *state)
{
#ifdef POLARIS_SELFTEST_NEGCTL_IGNORE_NOGPU
    (void)state;
    return 0;
#else
    return state->ops->opted_out(state->ops->context);
#endif
}

/* Why the order: the cheapest, hardware-free refusals come first, so a machine
 * that opted out or has some other GPU is not touched at all (no lease, no
 * mapping, no register read). The opt-out is asked even when the arena is
 * missing, so that a user who created \LOGIT\NOGPU sees "skipped-nogpu". */
static enum polaris_selftest_result stage_gate(struct run_state *state)
{
    const struct polaris_selftest_inputs *inputs = state->inputs;
    uint32_t identity = ((uint32_t)inputs->pci_device << 16) | inputs->pci_vendor;
    enum polaris_selftest_result result = POLARIS_SELFTEST_OK;
    if (!polaris10_identity_known(identity)) {
        result = POLARIS_SELFTEST_SKIPPED_UNKNOWN_DEVICE;
    } else if (opt_out_stops_the_run(state)) {
        result = POLARIS_SELFTEST_SKIPPED_NOGPU;
    } else if (!inputs->arena_plan) {
        result = POLARIS_SELFTEST_SKIPPED_NO_ARENA;
    } else if (!inputs->firmware_bundle_bytes) {
        result = POLARIS_SELFTEST_SKIPPED_NO_FIRMWARE;
    }
    char detail[LOG_DETAIL_BYTES];
    ksnprintf(detail, sizeof detail, "device=%04x:%04x rev=%02x",
              inputs->pci_vendor, inputs->pci_device, inputs->pci_revision);
    record(state->report, POLARIS_SELFTEST_STAGE_GATE, result, detail);
    return result;
}

/* Why before anything else touches the card: the lease is the proof that the
 * driver, and no other, may write; the guard checks it on every access. */
static enum polaris_selftest_result stage_lease(struct run_state *state)
{
    if (state->ops->acquire_lease(state->ops->context)) {
        record(state->report, POLARIS_SELFTEST_STAGE_LEASE,
               POLARIS_SELFTEST_FAILED_LEASE, "");
        return POLARIS_SELFTEST_FAILED_LEASE;
    }
    record(state->report, POLARIS_SELFTEST_STAGE_LEASE, POLARIS_SELFTEST_OK, "");
    return POLARIS_SELFTEST_OK;
}

/* Why only the arena: BAR0 holds the live GOP scanout in its first megabytes,
 * mapped write-combining by fb_init. Mapping the whole BAR uncached would
 * replace those page-table entries and make every desktop frame slow (this is
 * the P12 lesson, resource/device.c). The arena is a separate 2 MiB window far
 * from the scanout, so mapping just it leaves the scanout alone. */
static enum polaris_selftest_result stage_map(struct run_state *state)
{
    const struct polaris_arena_plan *plan = state->inputs->arena_plan;
    uint64_t arena_offset = plan->arena.base - plan->aperture.base;
    volatile uint8_t *window = 0;
    if (state->ops->map_registers(state->ops->context, &state->registers) ||
        !state->registers) {
        record(state->report, POLARIS_SELFTEST_STAGE_MAP,
               POLARIS_SELFTEST_FAILED_REGISTER_MAP, "");
        return POLARIS_SELFTEST_FAILED_REGISTER_MAP;
    }
    if (state->ops->map_arena_window(state->ops->context, arena_offset,
                                     plan->arena.bytes, &window) || !window) {
        record(state->report, POLARIS_SELFTEST_STAGE_MAP,
               POLARIS_SELFTEST_FAILED_ARENA_MAP, "");
        return POLARIS_SELFTEST_FAILED_ARENA_MAP;
    }
    state->bar0_origin = (volatile uint8_t *)((uintptr_t)window - (uintptr_t)arena_offset);
    char detail[LOG_DETAIL_BYTES];
    ksnprintf(detail, sizeof detail, "arena-offset=0x%llx arena-bytes=0x%llx",
              (unsigned long long)arena_offset, (unsigned long long)plan->arena.bytes);
    record(state->report, POLARIS_SELFTEST_STAGE_MAP, POLARIS_SELFTEST_OK, detail);
    return POLARIS_SELFTEST_OK;
}

/* Describe the arena and the boot surface to the runtime. Mirrors the request
 * polaris_arena_plan_build() validated, so the runtime's own planner arrives at
 * the layout the mapping check compares against. */
static void fill_memory_request(struct run_state *state)
{
    const struct polaris_selftest_inputs *inputs = state->inputs;
    const struct polaris_arena_plan *plan = inputs->arena_plan;
    struct polaris_runtime_request *request = &state->request;
    *request = (struct polaris_runtime_request){0};
    request->memory.vram = inputs->vram;
    request->memory.aperture = plan->aperture;
    request->memory.arena = plan->arena;
    request->memory.scanout = plan->scanout;
    request->memory.aperture_cpu_base = inputs->bar0_physical;
    request->memory.reserved = plan->inventory.range;
    request->memory.reserved_count = plan->inventory.count;
    request->memory.firmware_bytes = inputs->firmware_bundle_bytes;
    request->memory.staging_bytes = POLARIS_ARENA_STAGING_BYTES;
    request->width = inputs->width;
    request->height = inputs->height;
    request->pitch = inputs->pitch;
}

/* Describe the two mappings and the arena to the native platform. BAR0 is given
 * as its virtual origin with only the arena window behind it; the platform
 * hands out pointers inside the arena (UC window) or the scanout (the existing
 * write-combining mapping at the same offset) and nowhere else. */
static void fill_resources(struct run_state *state)
{
    const struct polaris_selftest_inputs *inputs = state->inputs;
    const struct polaris_arena_plan *plan = inputs->arena_plan;
    state->resources = (struct polaris_native_resources){
        .opaque = (void *)state->ops,
        .read_pci32 = native_read_pci32,
        .now_us = native_now_us,
        .bar0 = state->bar0_origin,
        .bar5 = state->registers,
        .bar0_physical = inputs->bar0_physical,
        .bar0_bytes = plan->aperture.bytes,
        .bar5_physical = inputs->bar5_physical,
        .bar5_bytes = inputs->bar5_bytes,
        .arena = plan->arena,
        .scanout = plan->scanout
    };
}

/* The memory-controller, HDP and BIF registers the native platform compares
 * before it agrees to bind (native.c check_mapping), BAR5 byte offsets, from
 * Linux v6.12 gmc_v8_0.c / vi.c / oss_3_0_d.h / bif_5_0_d.h as native.c cites
 * them. A refused bind says only "refused"; this one extra line says which of
 * them differed from what the check wants, so a single run on the real card is
 * enough to fix it. */
struct diagnostic_register {
    const char *name;
    uint32_t byte_offset;
};

/* Linux numbers registers in 32-bit words (the mm* constants of the asic_reg
 * headers); BAR5 is addressed in bytes. */
#define REGISTER_WORD_BYTES 4u
#define BAR5_BYTE_OFFSET(word_index) ((uint32_t)(word_index) * REGISTER_WORD_BYTES)

/* Word indices, Linux v6.12 drivers/gpu/drm/amd/include/asic_reg/. */
/* gmc/gmc_8_1_d.h: first and last 16 MiB unit of VRAM in the MC address space. */
#define MM_MC_VM_FB_LOCATION 0x809u
/* gmc/gmc_8_1_d.h: first and last 4 KiB page of the system aperture. */
#define MM_MC_VM_SYSTEM_APERTURE_LOW_ADDR 0x80du
#define MM_MC_VM_SYSTEM_APERTURE_HIGH_ADDR 0x80eu
/* gmc/gmc_8_1_d.h: MX L1 TLB control (system access mode, advanced driver model). */
#define MM_MC_VM_MX_L1_TLB_CNTL 0x819u
/* gmc/gmc_8_1_d.h: memory-controller blackout control. */
#define MM_MC_SHARED_BLACKOUT_CNTL 0x82bu
/* oss/oss_3_0_d.h: HDP non-surface aperture base (256-byte units) and info. */
#define MM_HDP_NONSURFACE_BASE 0xb01u
#define MM_HDP_NONSURFACE_INFO 0xb02u
/* oss/oss_3_0_d.h: HDP miscellaneous control. */
#define MM_HDP_MISC_CNTL 0xbd3u
/* bif/bif_5_0_d.h: installed VRAM size in MiB. */
#define MM_CONFIG_MEMSIZE 0x150au
/* bif/bif_5_0_d.h: framebuffer read/write enable at the bus interface. */
#define MM_BIF_FB_EN 0x1524u

enum diagnostic_register_index {
    DIAGNOSTIC_MC_FB_LOCATION,
    DIAGNOSTIC_MC_SYSTEM_APERTURE_LOW,
    DIAGNOSTIC_MC_SYSTEM_APERTURE_HIGH,
    DIAGNOSTIC_MC_VM_L1_TLB_CONTROL,
    DIAGNOSTIC_MC_SHARED_BLACKOUT,
    DIAGNOSTIC_HDP_NONSURFACE_BASE,
    DIAGNOSTIC_HDP_NONSURFACE_INFO,
    DIAGNOSTIC_HDP_MISC_CNTL,
    DIAGNOSTIC_CONFIG_MEMSIZE,
    DIAGNOSTIC_BIF_FB_EN,
    DIAGNOSTIC_REGISTER_COUNT
};

static const struct diagnostic_register diagnostic_registers[DIAGNOSTIC_REGISTER_COUNT] = {
    [DIAGNOSTIC_MC_FB_LOCATION] = {"mc_fb", BAR5_BYTE_OFFSET(MM_MC_VM_FB_LOCATION)},
    [DIAGNOSTIC_MC_SYSTEM_APERTURE_LOW] =
        {"sys_low", BAR5_BYTE_OFFSET(MM_MC_VM_SYSTEM_APERTURE_LOW_ADDR)},
    [DIAGNOSTIC_MC_SYSTEM_APERTURE_HIGH] =
        {"sys_high", BAR5_BYTE_OFFSET(MM_MC_VM_SYSTEM_APERTURE_HIGH_ADDR)},
    [DIAGNOSTIC_MC_VM_L1_TLB_CONTROL] = {"l1", BAR5_BYTE_OFFSET(MM_MC_VM_MX_L1_TLB_CNTL)},
    [DIAGNOSTIC_MC_SHARED_BLACKOUT] =
        {"blackout", BAR5_BYTE_OFFSET(MM_MC_SHARED_BLACKOUT_CNTL)},
    [DIAGNOSTIC_HDP_NONSURFACE_BASE] =
        {"hdp_base", BAR5_BYTE_OFFSET(MM_HDP_NONSURFACE_BASE)},
    [DIAGNOSTIC_HDP_NONSURFACE_INFO] =
        {"hdp_info", BAR5_BYTE_OFFSET(MM_HDP_NONSURFACE_INFO)},
    [DIAGNOSTIC_HDP_MISC_CNTL] = {"hdp_misc", BAR5_BYTE_OFFSET(MM_HDP_MISC_CNTL)},
    [DIAGNOSTIC_CONFIG_MEMSIZE] = {"memsize", BAR5_BYTE_OFFSET(MM_CONFIG_MEMSIZE)},
    [DIAGNOSTIC_BIF_FB_EN] = {"bif_fb", BAR5_BYTE_OFFSET(MM_BIF_FB_EN)}
};

/* The rest of the memory-controller state that decides whether the aperture
 * may be programmed (gmc_8_1_d.h, oss_3_0_d.h word indices), so a refusal on
 * the real board can be read off one boot log. */
#define MM_MC_VM_AGP_TOP 0x80au
#define MM_MC_VM_AGP_BOT 0x80bu
#define MM_MC_VM_AGP_BASE 0x80cu
#define MM_MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR 0x80fu
#define MM_VM_L2_CNTL 0x500u
#define MM_VM_CONTEXT0_CNTL 0x504u
#define MM_VM_CONTEXT1_CNTL 0x505u
#define MM_VM_CONTEXT0_PROTECTION_FAULT_STATUS 0x536u
#define MM_VM_CONTEXT0_PAGE_TABLE_BASE_ADDR 0x54fu
#define MM_SRBM_STATUS 0x394u

enum memory_controller_register_index {
    MEMORY_CONTROLLER_AGP_TOP,
    MEMORY_CONTROLLER_AGP_BOTTOM,
    MEMORY_CONTROLLER_AGP_BASE,
    MEMORY_CONTROLLER_APERTURE_DEFAULT,
    MEMORY_CONTROLLER_L2_CONTROL,
    MEMORY_CONTROLLER_CONTEXT0_CONTROL,
    MEMORY_CONTROLLER_CONTEXT1_CONTROL,
    MEMORY_CONTROLLER_CONTEXT0_FAULT_STATUS,
    MEMORY_CONTROLLER_CONTEXT0_PAGE_TABLE,
    MEMORY_CONTROLLER_SRBM_STATUS,
    MEMORY_CONTROLLER_REGISTER_COUNT
};

static const struct diagnostic_register memory_controller_registers[MEMORY_CONTROLLER_REGISTER_COUNT] = {
    [MEMORY_CONTROLLER_AGP_TOP] = {"agp_top", BAR5_BYTE_OFFSET(MM_MC_VM_AGP_TOP)},
    [MEMORY_CONTROLLER_AGP_BOTTOM] = {"agp_bot", BAR5_BYTE_OFFSET(MM_MC_VM_AGP_BOT)},
    [MEMORY_CONTROLLER_AGP_BASE] = {"agp_base", BAR5_BYTE_OFFSET(MM_MC_VM_AGP_BASE)},
    [MEMORY_CONTROLLER_APERTURE_DEFAULT] =
        {"sys_default", BAR5_BYTE_OFFSET(MM_MC_VM_SYSTEM_APERTURE_DEFAULT_ADDR)},
    [MEMORY_CONTROLLER_L2_CONTROL] = {"l2", BAR5_BYTE_OFFSET(MM_VM_L2_CNTL)},
    [MEMORY_CONTROLLER_CONTEXT0_CONTROL] = {"ctx0", BAR5_BYTE_OFFSET(MM_VM_CONTEXT0_CNTL)},
    [MEMORY_CONTROLLER_CONTEXT1_CONTROL] = {"ctx1", BAR5_BYTE_OFFSET(MM_VM_CONTEXT1_CNTL)},
    [MEMORY_CONTROLLER_CONTEXT0_FAULT_STATUS] =
        {"ctx0_fault", BAR5_BYTE_OFFSET(MM_VM_CONTEXT0_PROTECTION_FAULT_STATUS)},
    [MEMORY_CONTROLLER_CONTEXT0_PAGE_TABLE] =
        {"ctx0_ptb", BAR5_BYTE_OFFSET(MM_VM_CONTEXT0_PAGE_TABLE_BASE_ADDR)},
    [MEMORY_CONTROLLER_SRBM_STATUS] = {"srbm", BAR5_BYTE_OFFSET(MM_SRBM_STATUS)}
};

/* Registers per log line: a klog record keeps 112 bytes of text
 * (c/kernel/diag/klog.h KLOG_TEXT), and the first real-board bind failure lost
 * everything after the fifth register when all ten shared one line. */
#define REGISTERS_PER_LOG_LINE 4u

/* Why one helper: every register dump is printed the same way, a few
 * registers per line so no klog record is cut off. */
static void log_register_table(const struct run_state *state, const char *label,
                               const struct diagnostic_register *table, unsigned count)
{
    for (unsigned first = 0; first < count; first += REGISTERS_PER_LOG_LINE) {
        char text[BIND_DIAGNOSTICS_BYTES];
        int used = 0;
        text[0] = '\0';
        for (unsigned index = first; index < count && index < first + REGISTERS_PER_LOG_LINE; ++index) {
            uint32_t value = 0;
            if (state->ops->read_diagnostic_register(state->ops->context, table[index].byte_offset,
                                                     &value)) {
                continue;
            }
            used += ksnprintf(text + used, (int)sizeof text - used, "%s=%08x ", table[index].name,
                              value);
            if (used >= (int)sizeof text) {
                break;
            }
        }
        kprintf("[amd-polaris-runtime] %s %s\n", label, text);
    }
}

/* Why a separate line: it is not a stage, so the one-line-per-stage rule and
 * the HUD stay clean, and it is printed only when a bind has just failed. */
static void log_bind_diagnostics(const struct run_state *state)
{
    log_register_table(state, "bind-diagnostics", diagnostic_registers, DIAGNOSTIC_REGISTER_COUNT);
    log_register_table(state, "gmc-diagnostics", memory_controller_registers,
                       MEMORY_CONTROLLER_REGISTER_COUNT);
}

/* Why a port of its own: the aperture is programmed BEFORE the native platform
 * exists (its bind is what needs the aperture), so these accesses go through
 * the environment's own BAR5 accessors, and still only while the lease is
 * held, like every other register access of the self-test. */
static int memory_controller_read(void *context, uint32_t byte_offset, uint32_t *value)
{
    const struct run_state *state = context;
    if (!state->ops->lease_held(state->ops->context)) {
        return GUARD_ACCESS_REFUSED;
    }
    return state->ops->read_diagnostic_register(state->ops->context, byte_offset, value);
}

/* The write half of the same port; see memory_controller_read. */
static int memory_controller_write(void *context, uint32_t byte_offset, uint32_t value)
{
    const struct run_state *state = context;
    if (!state->ops->write_register || !state->ops->lease_held(state->ops->context)) {
        return GUARD_ACCESS_REFUSED;
    }
    return state->ops->write_register(state->ops->context, byte_offset, value);
}

/* The clock for the bounded idle wait. */
static uint64_t memory_controller_now_us(void *context)
{
    const struct run_state *state = context;
    return state->ops->now_us(state->ops->context);
}

/* The page the memory controller answers unmapped accesses with. Linux uses
 * its own VRAM scratch page (mem_scratch); the VBIOS driver-scratch area is
 * that page here, and the arena's first page stands in when the VBIOS named
 * none. Both are inside VRAM and kept out of every other use by B2. */
static uint64_t memory_controller_default_address(const struct run_state *state)
{
    const struct polaris_arena_plan *plan = state->inputs->arena_plan;
    for (size_t index = 0; index < plan->inventory.count; ++index) {
        if (plan->inventory.kind[index] == POLARIS_ARENA_RESERVE_VBIOS_DRIVER_SCRATCH) {
            return plan->inventory.range[index].base;
        }
    }
    return plan->arena.base;
}

/* Why before bind: the native platform refuses (native.c check_mapping) any
 * memory controller not in the state Linux leaves it in, and the board's GOP
 * leaves it in another one. The owner approved programming exactly the system
 * aperture and the L1 access mode the Linux way (memory/gmc_aperture.h). The
 * outcome is always logged, with the register dump when it was refused. */
static enum gmc_aperture_outcome prepare_memory_controller(struct run_state *state)
{
    struct gmc_register_port port = {
        .context = state,
        .read = memory_controller_read,
        .write = memory_controller_write,
        .now_us = memory_controller_now_us,
    };
    int idle_timed_out = 0;
    enum gmc_aperture_outcome outcome =
        gmc_aperture_prepare(&port, state->inputs->vram, memory_controller_default_address(state),
                             &memory_controller_snapshot, &idle_timed_out);
    kprintf("[amd-polaris-runtime] gmc result=%s firmware-l1=%08x firmware-aperture=%08x-%08x "
            "mc-idle=%s\n",
            gmc_aperture_outcome_name(outcome), memory_controller_snapshot.l1_tlb_control,
            memory_controller_snapshot.aperture_low_page, memory_controller_snapshot.aperture_high_page,
            idle_timed_out ? "timeout" : "ok");
    if (outcome != GMC_APERTURE_PROGRAMMED && outcome != GMC_APERTURE_ALREADY_CONFIGURED) {
        log_register_table(state, "gmc-diagnostics", memory_controller_registers,
                           MEMORY_CONTROLLER_REGISTER_COUNT);
    }
    return outcome;
}

/* Why: whatever stage fails after the aperture was programmed, the card goes
 * back to the firmware's values before the self-test gives up, in the reverse
 * order (memory/gmc_aperture.h). The lease is still held here. */
static void restore_memory_controller(struct run_state *state)
{
    struct gmc_register_port port = {
        .context = state,
        .read = memory_controller_read,
        .write = memory_controller_write,
        .now_us = memory_controller_now_us,
    };
    if (!memory_controller_snapshot.programmed) {
        return;
    }
    int failed = gmc_aperture_restore(&port, &memory_controller_snapshot);
    kprintf("[amd-polaris-runtime] gmc result=%s\n",
            failed ? "restore-incomplete" : "restored-firmware-values");
}

/* Why read-only and why PCI Command in the failure text: the native platform
 * refuses any layout it cannot prove (native.c), among them a card whose Bus
 * Master bit is off. The command word is the one fact that cannot be recovered
 * from a bare "refused" and is cheap to read. */
static enum polaris_selftest_result stage_bind(struct run_state *state)
{
    fill_memory_request(state);
    fill_resources(state);
    enum gmc_aperture_outcome memory_controller = prepare_memory_controller(state);
    if (memory_controller != GMC_APERTURE_PROGRAMMED &&
        memory_controller != GMC_APERTURE_ALREADY_CONFIGURED) {
        char detail[LOG_DETAIL_BYTES];
        ksnprintf(detail, sizeof detail, "gmc=%s", gmc_aperture_outcome_name(memory_controller));
        record(state->report, POLARIS_SELFTEST_STAGE_BIND, POLARIS_SELFTEST_FAILED_BIND, detail);
        return POLARIS_SELFTEST_FAILED_BIND;
    }
    if (polaris_desktop_bind(&state->resources, &state->request,
                             &state->native_platform)) {
        uint32_t command = 0;
        char detail[LOG_DETAIL_BYTES];
        if (state->ops->read_pci_config(state->ops->context, PCI_COMMAND_OFFSET, &command)) {
            command = 0;
        }
        ksnprintf(detail, sizeof detail, "pci-command=%04x", command & 0xffffu);
        record(state->report, POLARIS_SELFTEST_STAGE_BIND,
               POLARIS_SELFTEST_FAILED_BIND, detail);
        log_bind_diagnostics(state);
        return POLARIS_SELFTEST_FAILED_BIND;
    }
    guard_install(&state->native_platform, state->ops);
    record(state->report, POLARIS_SELFTEST_STAGE_BIND, POLARIS_SELFTEST_OK, "");
    return POLARIS_SELFTEST_OK;
}

/* Why one function: the comparison of the platform's translation with the plan
 * is the whole safety argument of the mapping stage, so the watched mutation
 * removes exactly it and the gate watches a wrong base slip through. Nonzero
 * when the translation disagrees with the plan. */
static int arena_mapping_mismatches(const struct run_state *state)
{
#ifdef POLARIS_SELFTEST_NEGCTL_SKIP_MAPPING_CHECK
    (void)state;
    return 0;
#else
    return polaris_arena_check_mapping(state->inputs->arena_plan,
                                       guarded_platform.resolve_mapping,
                                       guarded_platform.opaque,
                                       state->bar0_origin);
#endif
}

/* Why before the first write: if the platform's address translation disagrees
 * with the plan, everything the runtime writes into the arena lands somewhere
 * else in VRAM, possibly on the scanout. polaris_arena_check_mapping() asks the
 * very callback the runtime will use (through the guard) and compares. */
static enum polaris_selftest_result stage_mapping(struct run_state *state)
{
    int mismatch = arena_mapping_mismatches(state);
    if (mismatch) {
        record(state->report, POLARIS_SELFTEST_STAGE_MAPPING,
               POLARIS_SELFTEST_FAILED_MAPPING_CHECK, "");
        return POLARIS_SELFTEST_FAILED_MAPPING_CHECK;
    }
    record(state->report, POLARIS_SELFTEST_STAGE_MAPPING, POLARIS_SELFTEST_OK, "");
    return POLARIS_SELFTEST_OK;
}

/* Why one function: which SMC file is loaded follows from the key and from
 * nothing else, so the watched mutation replaces exactly this value and the
 * gate watches the wrong file get chosen for a card whose key is 0. */
static unsigned security_key_to_use(unsigned key_read_from_hardware)
{
#ifdef POLARIS_SELFTEST_NEGCTL_ASSUME_KEY_ONE
    (void)key_read_from_hardware;
    return 1;
#else
    return key_read_from_hardware;
#endif
}

/* Why here: the key is the first register write of the run, so everything that
 * could make it unsafe (lease, bind, mapping check) has already passed. */
static enum polaris_selftest_result stage_key(struct run_state *state)
{
    struct polaris_security_key key = {0};
    struct polaris_security_key_ops key_ops = {
        .context = guarded_platform.opaque,
        .read_register = guarded_platform.read_reg,
        .write_register = guarded_platform.write_reg
    };
    int read_result = polaris_security_key_read(&key_ops, &key);
    char detail[LOG_DETAIL_BYTES];
    if (read_result) {
        state->report->detail_code = read_result;
        ksnprintf(detail, sizeof detail, "reason=%s",
                  polaris_security_key_result_name(read_result));
        record(state->report, POLARIS_SELFTEST_STAGE_KEY,
               POLARIS_SELFTEST_FAILED_KEY_UNREADABLE, detail);
        return POLARIS_SELFTEST_FAILED_KEY_UNREADABLE;
    }
    unsigned security_key = security_key_to_use(key.security_key);
    state->report->key_known = 1;
    state->report->security_key = security_key;
    state->report->smc_path = polaris_resource_file_path(
        POLARIS_FW_SMC, state->inputs->pci_device, state->inputs->pci_revision,
        security_key);
    ksnprintf(detail, sizeof detail, "key=%u protected-mode=%u smu-firmware=0x%08x",
              security_key, key.protected_mode, key.firmware_register);
    record(state->report, POLARIS_SELFTEST_STAGE_KEY, POLARIS_SELFTEST_OK, detail);
    return POLARIS_SELFTEST_OK;
}

/* Name of an SMU loader error for the log (smu/loader.h). Only an error the
 * SMU loader returned belongs here: the runtime's memory-plan and staging
 * failures carry other numbers, and naming those with this table would print a
 * plausible but wrong word (stage_runtime asks only for failed-smu). */
static const char *smu_error_name(int error)
{
    switch (error) {
    case POLARIS_SMU_LOAD_INVALID:
        return "invalid";
    case POLARIS_SMU_LOAD_BUSY:
        return "busy";
    case POLARIS_SMU_LOAD_IO:
        return "io";
    case POLARIS_SMU_LOAD_TIMEOUT:
        return "timeout";
    case POLARIS_SMU_LOAD_UNSUPPORTED:
        return "unsupported-state";
    case POLARIS_SMU_LOAD_REJECTED:
        return "rejected";
    case POLARIS_SMU_LOAD_QUARANTINED:
        return "quarantined";
    default:
        return "other";
    }
}

/* Words for the start-up branch the SMU loader chose (smu/loader.h), shared by
 * the serial line and the HUD so the two cannot disagree. */
static const char *smu_branch_name(enum polaris_smu_start_branch branch)
{
    switch (branch) {
    case POLARIS_SMU_BRANCH_REFUSED:
        return "refused";
    case POLARIS_SMU_BRANCH_WARM:
        return "warm";
    case POLARIS_SMU_BRANCH_COLD_PROTECTED:
        return "cold-protected";
    case POLARIS_SMU_BRANCH_COLD_UNPROTECTED:
        return "cold-unprotected";
    default:
        return "none";
    }
}

/* Why a serial line of its own, printed from inside the loader: the SMC
 * sequences poll for up to seconds, and a card that hangs the machine there
 * leaves no later line. This one is printed after the registers were read and
 * the branch was chosen and before the loader's first write, so it is the last
 * evidence of a hang. It is not a stage line (the one-line-per-stage rule and
 * the stage counts stay as they are). The report keeps a copy for the HUD. */
static void log_smu_preflight(void *context, const struct polaris_smu_preflight *preflight)
{
    struct run_state *state = context;
    state->report->smu_preflight_known = 1;
    state->report->smu_preflight = *preflight;
    kprintf("[amd-polaris-runtime] smu-preflight smu-firmware=0x%08x smc-clock=0x%08x "
            "smc-pc=0x%08x smc-reset=0x%08x smc-flags=0x%08x branch=%s\n",
            preflight->smu_firmware, preflight->clock_control, preflight->program_counter,
            preflight->reset_control, preflight->firmware_flags,
            smu_branch_name(preflight->branch));
}

/* What the loader measured, for the smu and sdma lines: how long the firmware
 * upload took (0 when the SMC was already running) and whether any mailbox
 * message found a previous response other than 1. */
static void format_smu_facts(char *out, size_t capacity, const struct polaris_desktop_info *info)
{
    char first_response[SMU_FIRST_RESPONSE_TEXT_BYTES];
    first_response[0] = '\0';
    if (info->smu_previous_response_anomalies) {
        ksnprintf(first_response, sizeof first_response, " prev-first=0x%x",
                  info->smu_previous_response_first);
    }
    ksnprintf(out, (int)capacity, "upload-us=%llu prev-anomalies=%u%s",
              (unsigned long long)info->smu_upload_microseconds,
              info->smu_previous_response_anomalies, first_response);
}

/* Map the runtime's own report onto the stage that stopped it. The runtime
 * performs memory planning, firmware staging, the SMU load and the SDMA ring
 * with its canaries in one call, and tells us where it stopped. */
static enum polaris_selftest_result
failure_for_runtime_stage(const struct polaris_desktop_info *info)
{
    if (info->stage == POLARIS_RUNTIME_OFF) {
        return POLARIS_SELFTEST_FAILED_RUNTIME_REFUSED;
    }
    switch (info->failed_stage) {
    case POLARIS_RUNTIME_MEMORY:
        return POLARIS_SELFTEST_FAILED_MEMORY_PLAN;
    case POLARIS_RUNTIME_FIRMWARE:
        return POLARIS_SELFTEST_FAILED_STAGING;
    case POLARIS_RUNTIME_SMU:
        return POLARIS_SELFTEST_FAILED_SMU;
    default:
        return POLARIS_SELFTEST_FAILED_SDMA;
    }
}

/* Start the existing runtime in self-test mode and turn its outcome into the
 * smu and sdma stage lines. The firmware and the workspace are released by the
 * caller once this returns: the runtime copies what it needs to the card. */
static enum polaris_selftest_result stage_runtime(struct run_state *state)
{
    struct polaris_selftest_report *report = state->report;
    int start_result = polaris_desktop_run(&guarded_platform, &state->request,
                                           POLARIS_DESKTOP_MODE_SELF_TEST);
    struct polaris_desktop_info info = {0};
    polaris_desktop_query(&info);
    char detail[LOG_DETAIL_BYTES];
    char facts[SMU_FACTS_TEXT_BYTES];
    format_smu_facts(facts, sizeof facts, &info);
    report->smu_load_status = info.smu_load_status;
    report->sdma_submitted = info.submitted;
    report->sdma_completed = info.completed;
    report->runtime_quarantined = info.quarantined;
    if (!start_result) {
        report->smu_loaded = 1;
        report->sdma_ran = 1;
        report->sdma_configured = 1;
        ksnprintf(detail, sizeof detail, "load-status=0x%x %s", info.smu_load_status, facts);
        record(report, POLARIS_SELFTEST_STAGE_SMU, POLARIS_SELFTEST_OK, detail);
        ksnprintf(detail, sizeof detail, "completed=%llu submitted=%llu",
                  (unsigned long long)info.completed, (unsigned long long)info.submitted);
        record(report, POLARIS_SELFTEST_STAGE_SDMA, POLARIS_SELFTEST_OK, detail);
        return POLARIS_SELFTEST_OK;
    }
    enum polaris_selftest_result failure = failure_for_runtime_stage(&info);
    report->detail_code = info.error;
    if (failure == POLARIS_SELFTEST_FAILED_SDMA) {
        report->smu_loaded = 1;
        report->sdma_configured = 1;
        ksnprintf(detail, sizeof detail, "load-status=0x%x %s", info.smu_load_status, facts);
        record(report, POLARIS_SELFTEST_STAGE_SMU, POLARIS_SELFTEST_OK, detail);
        ksnprintf(detail, sizeof detail,
                  "error=%d completed=%llu submitted=%llu quarantined=%u",
                  info.error, (unsigned long long)info.completed,
                  (unsigned long long)info.submitted, info.quarantined);
        record(report, POLARIS_SELFTEST_STAGE_SDMA, failure, detail);
        return failure;
    }
    char reason[SMU_REASON_TEXT_BYTES];
    reason[0] = '\0';
    if (failure == POLARIS_SELFTEST_FAILED_SMU) {
        ksnprintf(reason, sizeof reason, " reason=%s", smu_error_name(info.error));
    }
    ksnprintf(detail, sizeof detail, "error=%d%s load-status=0x%x quarantined=%u %s",
              info.error, reason, info.smu_load_status, info.quarantined, facts);
    record(report, POLARIS_SELFTEST_STAGE_SMU, failure, detail);
    return failure;
}

/* Load the files the key selects and run the runtime on them. The firmware
 * stage is where a missing or tampered file is reported; nothing has been
 * written to the card for it, only the key read has happened. */
static enum polaris_selftest_result stage_firmware_and_runtime(struct run_state *state)
{
    const struct polaris_selftest_ops *ops = state->ops;
    struct polaris_selftest_report *report = state->report;
    struct polaris_resource_files files = {0};
    char detail[LOG_DETAIL_BYTES];
    if (ops->load_firmware(ops->context, report->security_key, &files)) {
        ksnprintf(detail, sizeof detail, "smc=%s valid-mask=0x%x",
                  report->smc_path, files.valid_mask);
        record(report, POLARIS_SELFTEST_STAGE_FIRMWARE,
               POLARIS_SELFTEST_FAILED_FIRMWARE, detail);
        ops->release_firmware(ops->context, &files);
        return POLARIS_SELFTEST_FAILED_FIRMWARE;
    }
    void *workspace = ops->allocate_workspace(ops->context,
                                              (size_t)state->inputs->firmware_bundle_bytes);
    if (!workspace) {
        record(report, POLARIS_SELFTEST_STAGE_FIRMWARE,
               POLARIS_SELFTEST_FAILED_NO_MEMORY, "");
        ops->release_firmware(ops->context, &files);
        return POLARIS_SELFTEST_FAILED_NO_MEMORY;
    }
    ksnprintf(detail, sizeof detail, "smc=%s bundle-bytes=%llu", report->smc_path,
              (unsigned long long)state->inputs->firmware_bundle_bytes);
    record(report, POLARIS_SELFTEST_STAGE_FIRMWARE, POLARIS_SELFTEST_OK, detail);

    state->request.firmware = files.firmware;
    state->request.smc = files.smc;
    state->request.smc_security_key = report->security_key;
    state->request.workspace = workspace;
    state->request.workspace_bytes = (size_t)state->inputs->firmware_bundle_bytes;
    state->request.smu_preflight = log_smu_preflight;
    state->request.smu_preflight_context = state;
    enum polaris_selftest_result result = stage_runtime(state);
    ops->free_workspace(ops->context, workspace);
    ops->release_firmware(ops->context, &files);
    return result;
}

/* Why a straight line of stages: each one is a reason to stop, and the order is
 * the order the evidence is needed in (see the list in selftest.h). Nothing
 * after a failure runs, so nothing after a failure writes. The state is static
 * because the platform stored in the runtime keeps pointers into the
 * environment for the rest of the boot. */
/* Why split out: every stage from bind on may run after the memory controller
 * was programmed, and the caller undoes that on any failure in one place. */
static enum polaris_selftest_result run_from_bind(struct run_state *state)
{
    enum polaris_selftest_result result = stage_bind(state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    result = stage_mapping(state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    result = stage_key(state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    return stage_firmware_and_runtime(state);
}

enum polaris_selftest_result
polaris_selftest_run(const struct polaris_selftest_inputs *inputs,
                     const struct polaris_selftest_ops *ops,
                     struct polaris_selftest_report *report)
{
    if (!report) {
        return POLARIS_SELFTEST_FAILED_BAD_ARGUMENT;
    }
    *report = (struct polaris_selftest_report){0};
    guard_is_installed = 0;
    memory_controller_snapshot = (struct gmc_aperture_snapshot){0};
    if (!inputs || !ops || !ops->opted_out || !ops->acquire_lease || !ops->lease_held ||
        !ops->retire_lease || !ops->map_registers || !ops->map_arena_window ||
        !ops->read_pci_config || !ops->read_diagnostic_register || !ops->now_us ||
        !ops->load_firmware ||
        !ops->release_firmware || !ops->allocate_workspace || !ops->free_workspace) {
        return POLARIS_SELFTEST_FAILED_BAD_ARGUMENT;
    }
    static struct run_state state;
    state = (struct run_state){.inputs = inputs, .ops = ops, .report = report};
    enum polaris_selftest_result result = stage_gate(&state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    result = stage_lease(&state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    result = stage_map(&state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    result = run_from_bind(&state);
    if (result != POLARIS_SELFTEST_OK) {
        restore_memory_controller(&state);
    }
    return result;
}

/* Why a function of its own: the answer "is the SDMA0 ring drained" is two
 * reads and a comparison, and a read that fails is as unconfirmed as a ring
 * with work still queued in it. Nonzero when idleness could not be confirmed. */
static int sdma_ring_unconfirmed(uint32_t *read_pointer, uint32_t *write_pointer)
{
    if (guarded_platform.read_reg(guarded_platform.opaque,
                                  SDMA0_RING_READ_POINTER_REGISTER, read_pointer)) {
        return 1;
    }
    if (guarded_platform.read_reg(guarded_platform.opaque,
                                  SDMA0_RING_WRITE_POINTER_REGISTER, write_pointer)) {
        return 1;
    }
    return *read_pointer != *write_pointer;
}

/* Why read-only: at shutdown nothing may be started or reconfigured. Two things
 * are asked. Is the SDMA ring still holding work (read pointer behind write
 * pointer), which would mean the engine could still be writing VRAM when the
 * platform resets; that is read whenever the runtime configured the ring, not
 * only after a clean run, because a run that failed after configuring it can
 * have left work queued. And did the runtime write to the card and then lose
 * track of it (quarantine): then nothing can say what the card is doing, and
 * the driver.h quiesce contract ("could not be confirmed stopped") says so. An
 * idle ring of a healthy run is simply left as it is. */
/* The register port for quiesce: the run is over, so registers are reached
 * through the lease-guarded platform the run installed. */
static int quiesce_read(void *context, uint32_t byte_offset, uint32_t *value)
{
    (void)context;
    return guarded_platform.read_reg(guarded_platform.opaque, byte_offset, value);
}

/* The write half of the quiesce port; see quiesce_read. */
static int quiesce_write(void *context, uint32_t byte_offset, uint32_t value)
{
    (void)context;
    return guarded_platform.write_reg(guarded_platform.opaque, byte_offset, value);
}

/* The clock half of the quiesce port. */
static uint64_t quiesce_now_us(void *context)
{
    (void)context;
    return guarded_platform.now_us(guarded_platform.opaque);
}

/* Why at quiesce and why only with an idle ring: before a reboot or power-off
 * the card goes back to the firmware's memory-controller values, but never
 * under a DMA engine that may still be fetching (a busy ring keeps the Linux
 * values, which are the safe ones for it; the reset that follows clears
 * them). Failure runs already restored in polaris_selftest_run. */
static void restore_memory_controller_at_quiesce(int ring_unconfirmed)
{
    struct gmc_register_port port = {
        .context = NULL,
        .read = quiesce_read,
        .write = quiesce_write,
        .now_us = quiesce_now_us,
    };
    if (!memory_controller_snapshot.programmed || !guard_is_installed) {
        return;
    }
    if (ring_unconfirmed) {
        kprintf("[amd-polaris-runtime] gmc result=kept-linux-values sdma-busy\n");
        return;
    }
    int failed = gmc_aperture_restore(&port, &memory_controller_snapshot);
    kprintf("[amd-polaris-runtime] gmc result=%s\n",
            failed ? "restore-incomplete" : "restored-firmware-values");
}

int polaris_selftest_quiesce(const struct polaris_selftest_ops *ops,
                             struct polaris_selftest_report *report)
{
    if (!ops || !report || !ops->lease_held(ops->context)) {
        return 0;
    }
    int ring_inspected = guard_is_installed && report->sdma_configured;
    int ring_unconfirmed = 0;
    uint32_t read_pointer = 0;
    uint32_t write_pointer = 0;
    if (ring_inspected) {
        ring_unconfirmed = sdma_ring_unconfirmed(&read_pointer, &write_pointer);
    }
    unsigned quarantined = report->runtime_quarantined != 0;
    if (ring_inspected || quarantined) {
        enum polaris_selftest_result result = POLARIS_SELFTEST_OK;
        if (ring_unconfirmed) {
            result = POLARIS_SELFTEST_FAILED_SDMA_BUSY;
        } else if (quarantined) {
            result = POLARIS_SELFTEST_FAILED_QUARANTINED;
        }
        char pointers[LOG_DETAIL_BYTES];
        char detail[LOG_DETAIL_BYTES];
        if (ring_inspected) {
            ksnprintf(pointers, sizeof pointers,
                      "sdma-read-pointer=0x%x sdma-write-pointer=0x%x",
                      read_pointer, write_pointer);
        } else {
            ksnprintf(pointers, sizeof pointers, "sdma-ring=not-configured");
        }
        ksnprintf(detail, sizeof detail, "%s runtime-quarantined=%u", pointers, quarantined);
        /* A clean run's last word is the quiesce line, on the HUD too. After a
         * failure the HUD keeps the failure: it is the more useful photo. */
        if (report->result == POLARIS_SELFTEST_OK) {
            record(report, POLARIS_SELFTEST_STAGE_QUIESCE, result, detail);
        } else {
            log_stage_line(POLARIS_SELFTEST_STAGE_QUIESCE, result, detail);
        }
    }
    restore_memory_controller_at_quiesce(ring_unconfirmed);
    ops->retire_lease(ops->context);
    return ring_unconfirmed || quarantined;
}

/* The HUD shows the SMC file as the part of its name that tells the variants
 * apart ("k_smc" for polaris10_k_smc.bin), because the whole name costs a third
 * of a HUD line. */
static void smc_label(char *out, size_t capacity, const char *path)
{
    const char *name = path;
    for (const char *cursor = path; *cursor; ++cursor) {
        if (*cursor == '/') {
            name = cursor + 1;
        }
    }
    const char *prefix = SMC_FILE_PREFIX;
    const char *cursor = name;
    while (*prefix && *cursor == *prefix) {
        ++prefix;
        ++cursor;
    }
    if (*prefix) {
        cursor = name;
    }
    size_t length = 0;
    while (cursor[length] && length + 1 < capacity) {
        out[length] = cursor[length];
        ++length;
    }
    out[length] = '\0';
    const size_t suffix_length = sizeof SMC_FILE_SUFFIX - 1;
    if (length >= suffix_length) {
        const char *tail = out + length - suffix_length;
        const char *suffix = SMC_FILE_SUFFIX;
        while (*suffix && *tail == *suffix) {
            ++tail;
            ++suffix;
        }
        if (!*suffix) {
            out[length - suffix_length] = '\0';
        }
    }
}

/* Why the same fields as the serial line, shortened: the HUD is what a photo of
 * the screen shows when there is no serial cable, and it must never say
 * something the log did not. */
void polaris_selftest_format_hud(char *out, int capacity,
                                 const struct polaris_selftest_report *report)
{
    char smc_text[HUD_SMC_TEXT_BYTES];
    char label[SMC_LABEL_BYTES];
    char sdma_text[HUD_SDMA_TEXT_BYTES];
    char preflight_text[HUD_PREFLIGHT_TEXT_BYTES];
    const char *key_text = !report->key_known ? "?" : report->security_key ? "1" : "0";
    if (report->smc_path) {
        smc_label(label, sizeof label, report->smc_path);
        if (report->smu_loaded) {
            ksnprintf(smc_text, sizeof smc_text, "%s/%x", label, report->smu_load_status);
        } else {
            ksnprintf(smc_text, sizeof smc_text, "%s/-", label);
        }
    } else {
        ksnprintf(smc_text, sizeof smc_text, "-");
    }
    if (report->sdma_ran) {
        ksnprintf(sdma_text, sizeof sdma_text, "%llu", (unsigned long long)report->sdma_completed);
    } else {
        ksnprintf(sdma_text, sizeof sdma_text, "-");
    }
    preflight_text[0] = '\0';
    if (report->smu_preflight_known) {
        const struct polaris_smu_preflight *seen = &report->smu_preflight;
        ksnprintf(preflight_text, sizeof preflight_text,
                  " pre=%s fw=%x clk=%x pc=%x rst=%x fl=%x", smu_branch_name(seen->branch),
                  seen->smu_firmware, seen->clock_control, seen->program_counter,
                  seen->reset_control, seen->firmware_flags);
    }
    ksnprintf(out, capacity, "stage=%s result=%s key=%s smc=%s sdma=%s%s",
              polaris_selftest_stage_name(report->stage),
              polaris_selftest_result_name(report->result), key_text, smc_text, sdma_text,
              preflight_text);
}

#endif /* POLARIS_SELFTEST_COMPILED */
