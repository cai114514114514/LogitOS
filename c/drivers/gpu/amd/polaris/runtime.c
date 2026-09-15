#include "amd/polaris/runtime.h"

static int overlaps(uintptr_t a, uint64_t n, uintptr_t b, uint64_t m)
{ return a <= b ? b - a < n : a - b < m; }
static int cpu_span(const void *p, uint64_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }

static int identity(void *p, uint32_t *v)
{ struct polaris_runtime *r = p; return r->platform.read_identity(r->platform.opaque, v); }
static int read_reg(void *p, uint32_t a, uint32_t *v)
{ struct polaris_runtime *r = p; return r->platform.read_reg(r->platform.opaque, a, v); }
static int write_reg(void *p, uint32_t a, uint32_t v)
{ struct polaris_runtime *r = p; return r->platform.write_reg(r->platform.opaque, a, v); }
static int resolve_byte(void *p, uint64_t a, uint64_t n, volatile uint8_t **v)
{ struct polaris_runtime *r = p; return r->platform.resolve_mapping(r->platform.opaque, a, n, v); }
static int resolve_word(void *p, uint64_t a, uint64_t n, volatile uint32_t **v)
{
    volatile uint8_t *b = 0;
    if (resolve_byte(p, a, n, &b) || !b || ((uintptr_t)b & 3)) return -1;
    *v = (volatile uint32_t *)b;
    return 0;
}
static int sync_device(void *p, uint64_t a, uint64_t n)
{ struct polaris_runtime *r = p; return r->platform.sync(r->platform.opaque, POLARIS_SDMA_TO_DEVICE, a, n); }
static int sync_map(void *p, enum polaris_sdma_sync direction,
                     const struct polaris_sdma_mapping *m)
{ struct polaris_runtime *r = p; return r->platform.sync(r->platform.opaque, direction, m->range.gpu_base, m->range.bytes); }
static uint64_t now_us(void *p)
{ struct polaris_runtime *r = p; return r->platform.now_us(r->platform.opaque); }
static int write_wptr(void *p, uint32_t v)
{ return write_reg(p, 0xd210, v); }
static int read_smc(void *p, uint32_t a, uint32_t *v)
{
    uint32_t control, posted;
    if ((a & 3u) || a > 0x3fffcu || read_reg(p, 0x248, &control) ||
        control == UINT32_MAX || (control & 0x800u)) return -1;
    /* Port 11 is shared with the loader, under the same device lease. The
     * index read flushes the posted write before the SRAM DATA read. */
    return write_reg(p, 0x6b0, a) || read_reg(p, 0x6b0, &posted) ||
           posted != a || read_reg(p, 0x6b4, v) ? -1 : 0;
}
static int map_allocation(struct polaris_runtime *r, unsigned object,
                           struct polaris_sdma_mapping *m)
{
    const struct polaris_memory_allocation *a = &r->memory.object[object];
    m->range.gpu_base = a->gpu_address; m->range.bytes = a->bytes;
    return resolve_word(r, a->gpu_address, a->bytes, &m->cpu);
}
int polaris_runtime_start(struct polaris_runtime *r,
                           const struct polaris_platform *platform,
                           const struct polaris_runtime_request *request)
{
    int rc = -1;
    uint32_t id;
    struct polaris_fw_bundle_info plan;
    struct polaris_fw_view smc;
    struct polaris_sdma_mapping mapped[POLARIS_MEMORY_OBJECTS], scanout;
    if (!r) return -1;
    if (__atomic_exchange_n(&r->lock, 1u, __ATOMIC_ACQUIRE)) return -1;
    if (r->quarantined) { rc = -2; goto done; }
    if (r->attempted || !platform || !request || !platform->read_identity ||
        !platform->read_reg || !platform->write_reg || !platform->resolve_mapping ||
        !platform->sync || !platform->now_us || !request->workspace ||
        request->memory.staging_bytes < 8192 || !request->width ||
        request->width > UINT32_MAX / 4 || !request->height ||
        (request->pitch & 3u) || request->pitch < request->width * 4 ||
        (uint64_t)request->pitch * request->height > request->memory.scanout.bytes ||
        request->smc_security_key > 1 ||
        !cpu_span(request->workspace, request->workspace_bytes) ||
        overlaps((uintptr_t)request->workspace, request->workspace_bytes,
                 (uintptr_t)r, sizeof *r) ||
        overlaps((uintptr_t)request->workspace, request->workspace_bytes,
                 (uintptr_t)request, sizeof *request) ||
        overlaps((uintptr_t)request->workspace, request->workspace_bytes,
                 (uintptr_t)platform, sizeof *platform) ||
        overlaps((uintptr_t)request->workspace, request->workspace_bytes,
                 (uintptr_t)request->smc.data, request->smc.bytes)) goto done;
    if (polaris_fw_parse(POLARIS_FW_SMC, request->smc.data, request->smc.bytes, &smc) ||
        polaris_fw_bundle_plan(&request->firmware, 0, &plan) ||
        request->workspace_bytes < plan.bytes_used) goto done;
    struct polaris_memory_request memory = request->memory;
    memory.firmware_bytes = plan.bytes_used;
    if (polaris_memory_plan(&memory, &r->memory)) goto done;
    r->platform = *platform;
    r->stage = POLARIS_RUNTIME_MEMORY;
    if (identity(r, &id) || id != 0x67df1002u) goto failed;
    for (unsigned i = 0; i < POLARIS_MEMORY_OBJECTS; i++)
        if (map_allocation(r, i, &mapped[i])) goto failed;
    scanout.range.gpu_base = memory.scanout.base;
    scanout.range.bytes = memory.scanout.bytes;
    if (resolve_word(r, scanout.range.gpu_base, scanout.range.bytes, &scanout.cpu)) goto failed;
    /* A broken mapping adapter can return two CPU aliases for distinct MC
     * allocations. Reject that before staging bytes clobber the ring, input
     * firmware or a controller object; packet range checks alone cannot see it. */
    for (unsigned i = 0; i <= POLARIS_MEMORY_OBJECTS; i++) {
        const struct polaris_sdma_mapping *m = i == POLARIS_MEMORY_OBJECTS ? &scanout : &mapped[i];
        uintptr_t a = (uintptr_t)m->cpu;
        uint64_t n = m->range.bytes;
        if (!cpu_span((const void *)m->cpu, n) ||
            overlaps(a, n, (uintptr_t)r, sizeof *r) ||
            overlaps(a, n, (uintptr_t)request, sizeof *request) ||
            overlaps(a, n, (uintptr_t)platform, sizeof *platform) ||
            overlaps(a, n, (uintptr_t)request->workspace, request->workspace_bytes) ||
            overlaps(a, n, (uintptr_t)request->smc.data, request->smc.bytes)) goto failed;
        for (unsigned f = 0; f < POLARIS_FW_FILE_COUNT; f++)
            if (overlaps(a, n, (uintptr_t)request->firmware.file[f].data,
                         request->firmware.file[f].bytes)) goto failed;
        for (unsigned j = 0; j < i; j++)
            if (overlaps(a, n, (uintptr_t)mapped[j].cpu, mapped[j].range.bytes)) goto failed;
    }
    r->stage = POLARIS_RUNTIME_FIRMWARE;
    struct polaris_sdma_mapping *fw = &mapped[POLARIS_MEMORY_FIRMWARE];
    if (polaris_fw_bundle_stage(request->workspace, request->workspace_bytes,
                                fw->range.gpu_base, &request->firmware,
                                &r->firmware)) goto failed;
    /* The bundle is assembled in RAM first. Device memory gets explicit
     * DWORD accesses; memcpy may vectorize or issue unsuitable MMIO stores. */
    r->attempted = 1;
    for (size_t i = 0; i < r->firmware.bytes_used / 4; i++) {
        const uint8_t *b = request->workspace + i * 4;
        fw->cpu[i] = b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }
    if (sync_device(r, fw->range.gpu_base, r->firmware.bytes_used)) goto failed;
    r->stage = POLARIS_RUNTIME_SMU;
    struct polaris_smu_loader_ops smu_ops = {
        r, identity, read_reg, write_reg, resolve_byte, sync_device, now_us
    };
    struct polaris_smu_load_request load = {
        &smc, request->smc_security_key,
        mapped[POLARIS_MEMORY_TOC].range.gpu_base,
        mapped[POLARIS_MEMORY_SMU_SCRATCH].range.gpu_base,
        r->firmware.image, POLARIS_FW_BUNDLE_IMAGES
    };
    rc = polaris_smu_load(&r->smu, &smu_ops, &load);
    if (rc) goto failed;
    r->stage = POLARIS_RUNTIME_RING;
    struct polaris_sdma_engine_ops engine_ops = {
        {r, identity, read_reg, write_wptr, resolve_word, sync_map, now_us},
        write_reg, read_smc
    };
    mapped[POLARIS_MEMORY_FENCE].range.bytes = 4;
    rc = polaris_sdma_engine_start(&r->engine, &engine_ops, &r->smu,
                                   &mapped[POLARIS_MEMORY_RING],
                                   &mapped[POLARIS_MEMORY_FENCE], &r->queue);
    if (rc) goto failed;
    rc = polaris_present_init(&r->present, &r->queue,
                               &mapped[POLARIS_MEMORY_STAGING], &scanout,
                               request->width, request->height, request->pitch);
    if (rc) goto failed;
    r->stage = POLARIS_RUNTIME_ACTIVE;
    r->error = 0;
    goto done;
failed:
    r->failed_stage = r->stage;
    r->stage = POLARIS_RUNTIME_FAILED;
    r->error = rc ? rc : -1;
    if (r->attempted) r->quarantined = 1;
    rc = r->quarantined ? -2 : -1;
done:
    __atomic_store_n(&r->lock, 0u, __ATOMIC_RELEASE);
    return rc;
}
int polaris_runtime_present(struct polaris_runtime *r, const uint32_t *pixels,
                             uint64_t bytes, uint32_t stride,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    int rc;
    if (!r) return -1;
    if (__atomic_exchange_n(&r->lock, 1u, __ATOMIC_ACQUIRE)) return -2;
    if (r->quarantined) rc = -2;
    else if (r->stage != POLARIS_RUNTIME_ACTIVE) rc = -1;
    else {
        rc = polaris_present_rect(&r->present, pixels, bytes, stride, x, y, w, h);
        if (rc == -2) {
            r->quarantined = 1;
            r->failed_stage = r->stage;
            r->stage = POLARIS_RUNTIME_FAILED;
            r->error = rc;
        }
    }
    __atomic_store_n(&r->lock, 0u, __ATOMIC_RELEASE);
    return rc;
}
