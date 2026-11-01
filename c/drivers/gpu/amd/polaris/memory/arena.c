#include "amd/polaris/memory/arena.h"
#include "kprintf.h"

#define VBIOS_FRAMEBUFFER_BYTES_PER_PIXEL 4u

/* Power-of-two alignment helpers; all addresses here stay below 2^41, so the
 * additions cannot wrap. */
static uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1u);
}

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return align_down(value + alignment - 1u, alignment);
}

/* Half-open overlap, the same convention as layout.c. */
static int ranges_overlap(struct polaris_memory_range a,
                          struct polaris_memory_range b)
{
    return a.base < b.base + b.bytes && b.base < a.base + a.bytes;
}

/* Smallest single range holding both. The planner takes one scanout range, and
 * the GOP framebuffer and the CRTC's scanout start at the same surface, so the
 * hull is exactly the longer of the two; for ranges that do not touch it also
 * covers the gap, which is the safe side. Both inputs were checked against VRAM
 * by add_reservation, so the sums cannot wrap. */
static struct polaris_memory_range range_union(struct polaris_memory_range a,
                                               struct polaris_memory_range b)
{
    uint64_t base = a.base < b.base ? a.base : b.base;
    uint64_t a_end = a.base + a.bytes;
    uint64_t b_end = b.base + b.bytes;
    uint64_t end = a_end > b_end ? a_end : b_end;
    return (struct polaris_memory_range){base, end - base};
}

/* Record one in-use VRAM range, widened outward to 64 KiB so that a surface
 * that does not start or end on a boundary still keeps its whole last page out
 * of the arena. A zero-sized range is "absent" and is not an error. A range
 * that leaves VRAM is refused rather than clipped: the planner would reject it
 * anyway, and silently trimming hides a wrong address. */
static int add_reservation(struct polaris_arena_inventory *inventory,
                           struct polaris_memory_range vram,
                           enum polaris_arena_reserve_kind kind, unsigned crtc,
                           uint64_t base, uint64_t bytes)
{
    if (!bytes) {
        return POLARIS_ARENA_OK;
    }
    uint64_t vram_end = vram.base + vram.bytes;
    if (bytes > UINT64_MAX - base || base < vram.base ||
        base + bytes > vram_end) {
        return POLARIS_ARENA_OUTSIDE_VRAM;
    }
    if (inventory->count >= POLARIS_MEMORY_MAX_RESERVED) {
        return POLARIS_ARENA_TOO_MANY;
    }
    uint64_t first = align_down(base, POLARIS_ARENA_RESERVE_GRANULE);
    uint64_t last = align_up(base + bytes, POLARIS_ARENA_RESERVE_GRANULE);
    if (first < vram.base) {
        first = vram.base;
    }
    if (last > vram_end) {
        last = vram_end;
    }
    size_t slot = inventory->count++;
    inventory->range[slot] = (struct polaris_memory_range){first, last - first};
    inventory->kind[slot] = kind;
    inventory->crtc[slot] = crtc;
    return POLARIS_ARENA_OK;
}

/* gmc_v8_0_get_vbios_fb_size(): with the legacy VGA engine on, the pre-OS
 * buffer may be the whole 9 MiB VGA allocation; otherwise it is CRTC0's
 * viewport at four bytes per pixel. amdgpu_ttm.c keeps it at VRAM offset 0. */
static uint64_t vbios_framebuffer_bytes(const struct polaris_display_scan *scan)
{
#ifdef POLARIS_ARENA_NEGCTL_NO_VGA
    (void)scan;
    return 0;
#else
    if (scan->vga_mode_enabled) {
        return POLARIS_ARENA_VGA_ALLOCATION;
    }
    return (uint64_t)scan->viewport_width * scan->viewport_height *
           VBIOS_FRAMEBUFFER_BYTES_PER_PIXEL;
#endif
}

/* Collect everything the hardware and the VBIOS say is in use. An active CRTC
 * whose scanout size cannot be computed (tiled, zero pitch) fails the whole
 * inventory: guessing a size would be the one way to hand the arena a live
 * scanout. The GOP framebuffer is listed in full as well, because the CRTC only
 * names the part of it that is scanned out. */
static int build_inventory(const struct polaris_arena_inputs *inputs,
                           struct polaris_arena_inventory *inventory)
{
    const struct polaris_display_scan *scan = inputs->scan;
    struct polaris_memory_range vram = inputs->vram;
    int rc;
#ifdef POLARIS_ARENA_NEGCTL_DROP_SCANOUT
    int forgotten = -1;
    for (unsigned index = 0; index < POLARIS_DISPLAY_CRTC_COUNT; ++index) {
        if (scan->crtc[index].active) {
            forgotten = (int)index;
        }
    }
#endif
    for (unsigned index = 0; index < POLARIS_DISPLAY_CRTC_COUNT; ++index) {
        const struct polaris_crtc_state *crtc = &scan->crtc[index];
        if (crtc->active) {
            if (!crtc->scanout_known) {
                return POLARIS_ARENA_UNKNOWN_SCANOUT;
            }
#ifdef POLARIS_ARENA_NEGCTL_DROP_SCANOUT
            /* Watched mutation: the last active CRTC's scanout is forgotten. */
            if ((int)index == forgotten) {
                continue;
            }
#endif
            rc = add_reservation(inventory, vram, POLARIS_ARENA_RESERVE_SCANOUT,
                                 index, crtc->surface_mc, crtc->scanout_bytes);
            if (rc) {
                return rc;
            }
        }
        if (crtc->cursor_enabled) {
            rc = add_reservation(inventory, vram, POLARIS_ARENA_RESERVE_CURSOR,
                                 index, crtc->cursor_mc, crtc->cursor_bytes);
            if (rc) {
                return rc;
            }
        }
    }
    rc = add_reservation(inventory, vram, POLARIS_ARENA_RESERVE_VBIOS_FIRMWARE, 0,
                         inputs->vbios->firmware.base,
                         inputs->vbios->firmware.bytes);
    if (rc) {
        return rc;
    }
    rc = add_reservation(inventory, vram,
                         POLARIS_ARENA_RESERVE_VBIOS_DRIVER_SCRATCH, 0,
                         inputs->vbios->driver_scratch.base,
                         inputs->vbios->driver_scratch.bytes);
    if (rc) {
        return rc;
    }
    rc = add_reservation(inventory, vram,
                         POLARIS_ARENA_RESERVE_VBIOS_FRAMEBUFFER, 0, vram.base,
                         vbios_framebuffer_bytes(scan));
    if (rc) {
        return rc;
    }
#ifdef POLARIS_ARENA_NEGCTL_DROP_GOP_RANGE
    /* Watched mutation: protect the GOP framebuffer only through the CRTC's
     * pitch x active height, as the first version of this planner did. */
    return POLARIS_ARENA_OK;
#else
    /* Last, so the indexes of the CRTC and VBIOS items above stay fixed. */
    return add_reservation(inventory, vram, POLARIS_ARENA_RESERVE_GOP_FRAMEBUFFER,
                           0, inputs->gop_framebuffer.base,
                           inputs->gop_framebuffer.bytes);
#endif
}

/* Ask polaris_memory_plan, the function runtime.c itself calls, how many bytes
 * the TOC, SMU scratch, firmware, ring, fence and staging objects take. The
 * trial request gives it an arena far larger than any card so that only the
 * object sizes matter; the scanout sits just past the arena because the
 * planner insists on a non-overlapping scanout. This avoids copying the object
 * sizes into a second place that could drift from layout.c. */
static int runtime_requirement(uint64_t firmware_bytes, uint64_t *required)
{
    struct polaris_memory_request trial = {0};
    struct polaris_memory_layout layout;
    uint64_t half = POLARIS_MEMORY_LIMIT / 2u;
    trial.vram = (struct polaris_memory_range){0, POLARIS_MEMORY_LIMIT};
    trial.aperture = (struct polaris_memory_range){0, half};
    trial.arena = trial.aperture;
    trial.scanout = (struct polaris_memory_range){half, POLARIS_MEMORY_PAGE};
    trial.firmware_bytes = firmware_bytes;
    trial.staging_bytes = POLARIS_ARENA_STAGING_BYTES;
    if (polaris_memory_plan(&trial, &layout)) {
        return POLARIS_ARENA_REQUIREMENT;
    }
    *required = layout.bytes_used;
    return POLARIS_ARENA_OK;
}

/* Choose the highest 2 MiB aligned window of `size` bytes inside the aperture
 * that touches no reservation. Top-down keeps the arena away from the pre-OS
 * buffer and the scanouts, which firmware places at the start of VRAM. Whenever
 * a candidate collides, jump to just below the lowest colliding reservation, so
 * the search is a few steps and always terminates (the candidate only falls). */
static int find_arena_base(const struct polaris_arena_inventory *inventory,
                           struct polaris_memory_range aperture, uint64_t size,
                           uint64_t *base)
{
    uint64_t aperture_end = aperture.base + aperture.bytes;
    if (size > aperture.bytes) {
        return POLARIS_ARENA_NO_FIT;
    }
#ifdef POLARIS_ARENA_NEGCTL_NO_ALIGN
    /* Watched mutation: pack against the end of the aperture unaligned. */
    uint64_t candidate = aperture_end - size;
#else
    uint64_t candidate = align_down(aperture_end - size, POLARIS_ARENA_ALIGNMENT);
#endif
    for (;;) {
        struct polaris_memory_range window = {candidate, size};
        int blocked = 0;
        uint64_t lowest_blocker = UINT64_MAX;
        for (size_t i = 0; i < inventory->count; ++i) {
            if (ranges_overlap(window, inventory->range[i])) {
                blocked = 1;
                if (inventory->range[i].base < lowest_blocker) {
                    lowest_blocker = inventory->range[i].base;
                }
            }
        }
        if (!blocked) {
            *base = candidate;
            return POLARIS_ARENA_OK;
        }
        if (lowest_blocker < size) {
            return POLARIS_ARENA_NO_FIT;
        }
        uint64_t next = align_down(lowest_blocker - size, POLARIS_ARENA_ALIGNMENT);
        if (next < aperture.base || next >= candidate) {
            return POLARIS_ARENA_NO_FIT;
        }
        candidate = next;
    }
}

/* Publish the plan even on refusal: the inventory is what explains why. */
static int finish(struct polaris_arena_plan *out,
                  const struct polaris_arena_plan *plan, int result)
{
    *out = *plan;
    return result;
}

/* The order of the refusals is the order the evidence is needed: without the
 * GOP CRTC or the VBIOS table no reservation list can be complete, so nothing
 * after them is attempted. */
int polaris_arena_plan_build(const struct polaris_arena_inputs *inputs,
                             struct polaris_arena_plan *out)
{
    if (!out) {
        return POLARIS_ARENA_BAD_ARGUMENT;
    }
    struct polaris_arena_plan plan = {0};
    if (!inputs || !inputs->scan || !inputs->vram.bytes ||
        !inputs->aperture_bytes) {
        return finish(out, &plan, POLARIS_ARENA_BAD_ARGUMENT);
    }
    if (inputs->gop_crtc < 0 ||
        inputs->gop_crtc >= (int)POLARIS_DISPLAY_CRTC_COUNT ||
        !inputs->scan->crtc[inputs->gop_crtc].active) {
        return finish(out, &plan, POLARIS_ARENA_NO_GOP);
    }
    if (!inputs->vbios) {
        return finish(out, &plan, POLARIS_ARENA_VBIOS_UNKNOWN);
    }
    if (!inputs->gop_framebuffer.bytes) {
        return finish(out, &plan, POLARIS_ARENA_GOP_UNSIZED);
    }
    int rc = build_inventory(inputs, &plan.inventory);
    if (rc) {
        return finish(out, &plan, rc);
    }
    plan.aperture = (struct polaris_memory_range){inputs->vram.base,
                                                  inputs->aperture_bytes};
    const struct polaris_crtc_state *gop = &inputs->scan->crtc[inputs->gop_crtc];
    plan.scanout = (struct polaris_memory_range){gop->surface_mc,
                                                 gop->scanout_bytes};
#ifdef POLARIS_ARENA_NEGCTL_DROP_GOP_RANGE
    (void)range_union; /* the mutation never forms the hull */
#else
    /* The planner's non-overlap rule covers the scanout it is given, so give it
     * the whole framebuffer and not only the scanned-out part. */
    plan.scanout = range_union(plan.scanout, inputs->gop_framebuffer);
#endif
    /* The window is chosen on MC addresses but used through the CPU mapping,
     * so both address spaces must be 2 MiB aligned at the aperture start. */
    if ((plan.aperture.base | inputs->aperture_cpu_base) &
        (POLARIS_ARENA_ALIGNMENT - 1u)) {
        return finish(out, &plan, POLARIS_ARENA_MISALIGNED);
    }
    rc = runtime_requirement(inputs->firmware_bytes, &plan.required_bytes);
    if (rc) {
        return finish(out, &plan, rc);
    }
#ifdef POLARIS_ARENA_NEGCTL_NO_ALIGN
    uint64_t size = plan.required_bytes;
#else
    uint64_t size = align_up(plan.required_bytes, POLARIS_ARENA_ALIGNMENT);
#endif
    uint64_t base = 0;
    rc = find_arena_base(&plan.inventory, plan.aperture, size, &base);
    if (rc) {
        return finish(out, &plan, rc);
    }
    struct polaris_memory_request request = {0};
    request.vram = inputs->vram;
    request.aperture = plan.aperture;
    request.aperture_cpu_base = inputs->aperture_cpu_base;
    request.arena = (struct polaris_memory_range){base, size};
    request.scanout = plan.scanout;
    request.reserved = plan.inventory.range;
    request.reserved_count = plan.inventory.count;
    request.firmware_bytes = inputs->firmware_bytes;
    request.staging_bytes = POLARIS_ARENA_STAGING_BYTES;
    /* The chosen window is checked once more by the planner the runtime uses,
     * against the same reservation list and the real scanout. */
    if (polaris_memory_plan(&request, &plan.layout)) {
        plan.layout = (struct polaris_memory_layout){0};
        return finish(out, &plan, POLARIS_ARENA_PLAN_REJECTED);
    }
    plan.arena = request.arena;
    plan.arena_cpu_physical =
        inputs->aperture_cpu_base + (base - plan.aperture.base);
    return finish(out, &plan, POLARIS_ARENA_OK);
}

/* Log lines name the refusal so the HUD shows why there is no arena. */
const char *polaris_arena_result_name(int result)
{
    switch (result) {
    case POLARIS_ARENA_OK:
        return "arena-selected";
    case POLARIS_ARENA_NO_GOP:
        return "gop-crtc-unknown";
    case POLARIS_ARENA_VBIOS_UNKNOWN:
        return "vbios-reservations-unknown";
    case POLARIS_ARENA_UNKNOWN_SCANOUT:
        return "scanout-size-unknown";
    case POLARIS_ARENA_TOO_MANY:
        return "too-many-reservations";
    case POLARIS_ARENA_OUTSIDE_VRAM:
        return "reservation-outside-vram";
    case POLARIS_ARENA_MISALIGNED:
        return "aperture-not-2mib-aligned";
    case POLARIS_ARENA_REQUIREMENT:
        return "runtime-size-unknown";
    case POLARIS_ARENA_NO_FIT:
        return "no-free-window";
    case POLARIS_ARENA_PLAN_REJECTED:
        return "layout-rejected";
    case POLARIS_ARENA_GOP_UNSIZED:
        return "gop-framebuffer-size-unknown";
    default:
        return "bad-argument";
    }
}

/* The expected CPU pointer for a GPU address is a pure function of the plan,
 * so any adapter that disagrees is mis-translating, whatever it checks. */
static int mapping_agrees(const struct polaris_arena_plan *plan,
                          polaris_arena_resolve_fn resolve, void *ctx,
                          volatile uint8_t *bar0_cpu, uint64_t gpu_address,
                          uint64_t bytes)
{
    volatile uint8_t *resolved = 0;
    if (resolve(ctx, gpu_address, bytes, &resolved)) {
        return 0;
    }
#ifdef POLARIS_ARENA_NEGCTL_MAPPING_IGNORE_OFFSET
    /* Watched mutation: accept any non-null pointer. */
    (void)plan;
    (void)bar0_cpu;
    return resolved != 0;
#else
    return (uintptr_t)resolved ==
           (uintptr_t)bar0_cpu + (uintptr_t)(gpu_address - plan->aperture.base);
#endif
}

/* Stage B3 supplies the real mapping callback after it owns the device; this
 * is the check it runs, here so it is gated before any hardware exists. */
int polaris_arena_check_mapping(const struct polaris_arena_plan *plan,
                                polaris_arena_resolve_fn resolve, void *ctx,
                                volatile uint8_t *bar0_cpu)
{
    if (!plan || !resolve || !bar0_cpu || !plan->arena.bytes) {
        return -1;
    }
    if (!mapping_agrees(plan, resolve, ctx, bar0_cpu, plan->arena.base,
                        plan->arena.bytes)) {
        return -1;
    }
    for (unsigned object = 0; object < POLARIS_MEMORY_OBJECTS; ++object) {
        const struct polaris_memory_allocation *allocation =
            &plan->layout.object[object];
        if (!mapping_agrees(plan, resolve, ctx, bar0_cpu,
                            allocation->gpu_address, allocation->bytes)) {
            return -1;
        }
    }
    return 0;
}

/* Shared by the serial log and the HUD, like polaris_display_format_crtc. */
void polaris_arena_format(char *out, int capacity, int result,
                          const struct polaris_arena_plan *plan)
{
    if (result == POLARIS_ARENA_OK) {
        ksnprintf(out, capacity, "base=0x%llx size=0x%llx cpu=0x%llx reserved=%u",
                  (unsigned long long)plan->arena.base,
                  (unsigned long long)plan->arena.bytes,
                  (unsigned long long)plan->arena_cpu_physical,
                  (unsigned)plan->inventory.count);
        return;
    }
    ksnprintf(out, capacity, "refused=%s reserved=%u",
              polaris_arena_result_name(result),
              (unsigned)plan->inventory.count);
}

/* One sequence for the kernel and the gate. Refresh is measured only once the
 * GOP CRTC is known, since the wait is pointless for a CRTC nobody looks at. */
void polaris_arena_survey(const struct polaris_survey_inputs *inputs,
                          struct polaris_survey *out)
{
    struct polaris_survey survey = {0};
    survey.bar5_bytes = inputs->bar5_bytes;
    survey.gop_result = POLARIS_GOP_NO_CRTC;
    survey.refresh.result = POLARIS_REFRESH_BAD_ARGUMENT;
    survey.arena_result = POLARIS_ARENA_NO_GOP;
    survey.scan_result =
        polaris_display_scan(inputs->ops, inputs->bar5_bytes, &survey.scan);
    if (survey.scan_result == POLARIS_DISPLAY_OK) {
        survey.gop_result =
            polaris_display_find_gop(&survey.scan, &inputs->gop, &survey.gop_crtc);
    }
    if (survey.gop_result == POLARIS_GOP_MATCH) {
        survey.refresh_attempted = 1;
        (void)polaris_display_measure_refresh(inputs->ops, inputs->bar5_bytes,
                                              survey.gop_crtc, &survey.refresh);
        /* find_gop matched the surface to this exact BAR0-relative address, so
         * the subtraction cannot underflow and the sum cannot wrap. */
        struct polaris_arena_inputs arena_inputs = {
            .scan = &survey.scan,
            .gop_crtc = (int)survey.gop_crtc,
            .gop_framebuffer = {inputs->gop.mc_base +
                                    (inputs->gop.lfb - inputs->gop.bar0_physical),
                                inputs->lfb_bytes},
            .vbios = inputs->vbios,
            .vram = inputs->vram,
            .aperture_bytes = inputs->aperture_bytes,
            .aperture_cpu_base = inputs->aperture_cpu_base,
            .firmware_bytes = inputs->firmware_bytes
        };
        survey.arena_result = polaris_arena_plan_build(&arena_inputs, &survey.plan);
    }
    *out = survey;
}

/* The headline line for the HUD: the GOP CRTC when found, otherwise the reason
 * it was not, with BAR5's size since that is the other thing a mismatch of
 * register windows would show. */
void polaris_survey_format_gop(char *out, int capacity,
                               const struct polaris_survey *survey)
{
    if (survey->scan_result != POLARIS_DISPLAY_OK) {
        ksnprintf(out, capacity, "refused=%s bar5=%lluB",
                  polaris_display_result_name(survey->scan_result),
                  (unsigned long long)survey->bar5_bytes);
        return;
    }
    if (survey->gop_result != POLARIS_GOP_MATCH) {
        ksnprintf(out, capacity, "refused=%s active=%u bar5=%lluB",
                  polaris_gop_result_name(survey->gop_result),
                  survey->scan.active_count,
                  (unsigned long long)survey->bar5_bytes);
        return;
    }
    char crtc_text[96];
    polaris_display_format_crtc(crtc_text, sizeof crtc_text,
                                &survey->scan.crtc[survey->gop_crtc],
                                survey->refresh_attempted ? &survey->refresh : 0, 1);
    ksnprintf(out, capacity, "%s bar5=%lluB", crtc_text,
              (unsigned long long)survey->bar5_bytes);
}
