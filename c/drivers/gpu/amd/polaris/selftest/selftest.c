/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "amd/polaris/selftest/selftest.h"
#include "amd/polaris/pci_id.h"
#include "kprintf.h"

/* The stage exists only in HWHUD builds. The guard wraps the whole file, so a
 * plain kernel links an empty object and the host gate can prove it by looking
 * for the entry point in the symbol table. */
#ifdef POLARIS_SELFTEST_COMPILED

/* PCI Command register: byte offset 0x04 of the configuration header (PCI Local
 * Bus Specification 3.0, section 6.2.2). Bit 1 enables memory decode and bit 2
 * bus mastering; the native platform insists on both. */
#define PCI_COMMAND_OFFSET 0x04u
#define LOG_DETAIL_BYTES 120
#define BIND_DIAGNOSTICS_BYTES 240
#define SMC_LABEL_BYTES 24
/* "<label>/<8 hex digits>" and its terminator. */
#define HUD_SMC_TEXT_BYTES (SMC_LABEL_BYTES + 12)
/* A 64-bit decimal count and its terminator. */
#define HUD_SDMA_TEXT_BYTES 24
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
    [POLARIS_SELFTEST_FAILED_SDMA_BUSY] = "failed-sdma-busy"
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
 * here and nowhere else. */
static void record(struct polaris_selftest_report *report,
                   enum polaris_selftest_stage stage,
                   enum polaris_selftest_result result, const char *detail)
{
    report->stage = stage;
    report->result = result;
    kprintf("[amd-polaris-runtime] stage=%s result=%s%s%s\n",
            polaris_selftest_stage_name(stage), polaris_selftest_result_name(result),
            detail[0] ? " " : "", detail);
}

/* ---- adapters between the environment and the native platform -------------- */

/* The native platform re-reads PCI configuration before every register access
 * to notice a function that moved or vanished; this hands it the environment's
 * reader. */
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
    } else if (state->ops->opted_out(state->ops->context)) {
        result = POLARIS_SELFTEST_SKIPPED_NOGPU;
    } else if (!inputs->arena_plan) {
        result = POLARIS_SELFTEST_SKIPPED_NO_ARENA;
    } else if (!inputs->firmware_bundle_bytes) {
        result = POLARIS_SELFTEST_SKIPPED_NO_FIRMWARE;
    }
#ifdef POLARIS_SELFTEST_NEGCTL_IGNORE_NOGPU
    /* Watched mutation: the opt-out marker is read but never obeyed. */
    if (result == POLARIS_SELFTEST_SKIPPED_NOGPU) {
        result = POLARIS_SELFTEST_OK;
    }
#endif
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
    [DIAGNOSTIC_MC_FB_LOCATION] = {"mc_fb", 0x2024u},
    [DIAGNOSTIC_MC_SYSTEM_APERTURE_LOW] = {"sys_low", 0x2034u},
    [DIAGNOSTIC_MC_SYSTEM_APERTURE_HIGH] = {"sys_high", 0x2038u},
    [DIAGNOSTIC_MC_VM_L1_TLB_CONTROL] = {"l1", 0x2064u},
    [DIAGNOSTIC_MC_SHARED_BLACKOUT] = {"blackout", 0x20acu},
    [DIAGNOSTIC_HDP_NONSURFACE_BASE] = {"hdp_base", 0x2c04u},
    [DIAGNOSTIC_HDP_NONSURFACE_INFO] = {"hdp_info", 0x2c08u},
    [DIAGNOSTIC_HDP_MISC_CNTL] = {"hdp_misc", 0x2f4cu},
    [DIAGNOSTIC_CONFIG_MEMSIZE] = {"memsize", 0x5428u},
    [DIAGNOSTIC_BIF_FB_EN] = {"bif_fb", 0x5490u}
};

/* Why a separate line: it is not a stage, so the one-line-per-stage rule and
 * the HUD stay clean, and it is printed only when a bind has just failed. */
static void log_bind_diagnostics(const struct run_state *state)
{
    char text[BIND_DIAGNOSTICS_BYTES];
    int used = 0;
    text[0] = '\0';
    for (unsigned index = 0; index < DIAGNOSTIC_REGISTER_COUNT; ++index) {
        uint32_t value = 0;
        if (state->ops->read_diagnostic_register(state->ops->context,
                                                 diagnostic_registers[index].byte_offset,
                                                 &value)) {
            continue;
        }
        used += ksnprintf(text + used, (int)sizeof text - used, "%s=%08x ",
                          diagnostic_registers[index].name, value);
        if (used >= (int)sizeof text) {
            break;
        }
    }
    kprintf("[amd-polaris-runtime] bind-diagnostics %s\n", text);
}

/* Why read-only and why PCI Command in the failure text: the native platform
 * refuses any layout it cannot prove (native.c), among them a card whose Bus
 * Master bit is off. The command word is the one fact that cannot be recovered
 * from a bare "refused" and is cheap to read. */
static enum polaris_selftest_result stage_bind(struct run_state *state)
{
    fill_memory_request(state);
    fill_resources(state);
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

/* Why before the first write: if the platform's address translation disagrees
 * with the plan, everything the runtime writes into the arena lands somewhere
 * else in VRAM, possibly on the scanout. polaris_arena_check_mapping() asks the
 * very callback the runtime will use (through the guard) and compares. */
static enum polaris_selftest_result stage_mapping(struct run_state *state)
{
    int mismatch = 0;
#ifndef POLARIS_SELFTEST_NEGCTL_SKIP_MAPPING_CHECK
    mismatch = polaris_arena_check_mapping(state->inputs->arena_plan,
                                           guarded_platform.resolve_mapping,
                                           guarded_platform.opaque,
                                           state->bar0_origin);
#endif
    if (mismatch) {
        record(state->report, POLARIS_SELFTEST_STAGE_MAPPING,
               POLARIS_SELFTEST_FAILED_MAPPING_CHECK, "");
        return POLARIS_SELFTEST_FAILED_MAPPING_CHECK;
    }
    record(state->report, POLARIS_SELFTEST_STAGE_MAPPING, POLARIS_SELFTEST_OK, "");
    return POLARIS_SELFTEST_OK;
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
#ifdef POLARIS_SELFTEST_NEGCTL_ASSUME_KEY_ONE
    /* Watched mutation: whatever the hardware says, take the key-1 files. */
    key.security_key = 1;
#endif
    state->report->key_known = 1;
    state->report->security_key = key.security_key;
    state->report->smc_path = polaris_resource_file_path(
        POLARIS_FW_SMC, state->inputs->pci_device, state->inputs->pci_revision,
        key.security_key);
    ksnprintf(detail, sizeof detail, "key=%u protected-mode=%u smu-firmware=0x%08x",
              key.security_key, key.protected_mode, key.firmware_register);
    record(state->report, POLARIS_SELFTEST_STAGE_KEY, POLARIS_SELFTEST_OK, detail);
    return POLARIS_SELFTEST_OK;
}

/* Name of an SMU loader error for the log (smu/loader.h). */
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
    report->smu_load_status = info.smu_load_status;
    report->sdma_submitted = info.submitted;
    report->sdma_completed = info.completed;
    if (!start_result) {
        report->smu_loaded = 1;
        report->sdma_ran = 1;
        ksnprintf(detail, sizeof detail, "load-status=0x%x", info.smu_load_status);
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
        ksnprintf(detail, sizeof detail, "load-status=0x%x", info.smu_load_status);
        record(report, POLARIS_SELFTEST_STAGE_SMU, POLARIS_SELFTEST_OK, detail);
        ksnprintf(detail, sizeof detail,
                  "error=%d completed=%llu submitted=%llu quarantined=%u",
                  info.error, (unsigned long long)info.completed,
                  (unsigned long long)info.submitted, info.quarantined);
        record(report, POLARIS_SELFTEST_STAGE_SDMA, failure, detail);
        return failure;
    }
    ksnprintf(detail, sizeof detail, "error=%d reason=%s load-status=0x%x quarantined=%u",
              info.error, smu_error_name(info.error), info.smu_load_status,
              info.quarantined);
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
enum polaris_selftest_result
polaris_selftest_run(const struct polaris_selftest_inputs *inputs,
                     const struct polaris_selftest_ops *ops,
                     struct polaris_selftest_report *report)
{
    if (!report) {
        return POLARIS_SELFTEST_FAILED_BAD_ARGUMENT;
    }
    *report = (struct polaris_selftest_report){0};
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
    result = stage_bind(&state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    result = stage_mapping(&state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    result = stage_key(&state);
    if (result != POLARIS_SELFTEST_OK) {
        return result;
    }
    return stage_firmware_and_runtime(&state);
}

/* Why read-only: at shutdown nothing may be started or reconfigured. The only
 * question is whether the SDMA ring still has work queued (read pointer behind
 * write pointer), which would mean the engine could still be writing VRAM when
 * the platform resets. An idle ring is simply left as it is. */
int polaris_selftest_quiesce(const struct polaris_selftest_ops *ops,
                             struct polaris_selftest_report *report)
{
    int unconfirmed = 0;
    if (!ops || !report || !ops->lease_held(ops->context)) {
        return unconfirmed;
    }
    if (report->sdma_ran && report->result == POLARIS_SELFTEST_OK) {
        uint32_t read_pointer = 0;
        uint32_t write_pointer = 0;
        if (guarded_platform.read_reg(guarded_platform.opaque,
                                      SDMA0_RING_READ_POINTER_REGISTER, &read_pointer) ||
            guarded_platform.read_reg(guarded_platform.opaque,
                                      SDMA0_RING_WRITE_POINTER_REGISTER, &write_pointer) ||
            read_pointer != write_pointer) {
            unconfirmed = 1;
        }
        char detail[LOG_DETAIL_BYTES];
        ksnprintf(detail, sizeof detail, "sdma-read-pointer=0x%x sdma-write-pointer=0x%x",
                  read_pointer, write_pointer);
        record(report, POLARIS_SELFTEST_STAGE_QUIESCE,
               unconfirmed ? POLARIS_SELFTEST_FAILED_SDMA_BUSY : POLARIS_SELFTEST_OK,
               detail);
    }
    ops->retire_lease(ops->context);
    return unconfirmed;
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
    ksnprintf(out, capacity, "stage=%s result=%s key=%s smc=%s sdma=%s",
              polaris_selftest_stage_name(report->stage),
              polaris_selftest_result_name(report->result), key_text, smc_text, sdma_text);
}

#endif /* POLARIS_SELFTEST_COMPILED */
