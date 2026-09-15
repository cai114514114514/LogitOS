#include "amd/polaris/desktop.h"
#include "fb.h"
#include "kprintf.h"

static struct polaris_native hardware;
static struct polaris_runtime runtime;
static int hook(const uint32_t *pixels, uint32_t stride,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    /* fb's callback stride is in pixels; the hardware controller takes bytes.
     * fb already holds the graphics mutex and has drained AP front writers. */
    if (stride > UINT32_MAX / 4) return -1;
    int rc = polaris_runtime_present(&runtime, pixels,
                                      (uint64_t)stride * 4 * fb_height(),
                                      stride * 4, x, y, w, h);
    if (!rc) {
        uint64_t n = runtime.present.frames;
        if (!(n & (n - 1)))
            kprintf("[amd-polaris-present] completed=%llu pixels=%llu upload=%llu sdma=%llu\n",
                    (unsigned long long)n,
                    (unsigned long long)runtime.present.pixels,
                    (unsigned long long)runtime.present.uploaded_bytes,
                    (unsigned long long)runtime.queue.completed);
    } else if (rc == -2) {
        kprintf("[amd-polaris-present] completion=unknown CPU-front-writes=stopped\n");
    }
    return rc;
}
int polaris_desktop_start(const struct polaris_native_resources *resources,
                           const struct polaris_runtime_request *request)
{
    int rc = -1;
    uint64_t lfb, bytes;
    struct polaris_platform platform;
    if (!resources || !request) return -1;
    fb_graphics_lock();
    if (hardware.bound || runtime.attempted || runtime.quarantined) goto done;
    if (!fb_boot_lfb_range(&lfb, &bytes) ||
        request->width != fb_width() || request->height != fb_height() ||
        bytes != (uint64_t)request->pitch * request->height ||
        resources->arena.base != request->memory.arena.base ||
        resources->arena.bytes != request->memory.arena.bytes ||
        resources->scanout.base != request->memory.scanout.base ||
        resources->scanout.bytes != request->memory.scanout.bytes ||
        resources->scanout.bytes != bytes ||
        polaris_native_bind(&hardware, resources, &platform)) goto done;
    /* Matching MC and HDP readbacks establish the address translation. A PCI
     * BAR address is never passed to the SDMA encoder as a GPU address. */
    if (request->memory.vram.base != hardware.vram_base ||
        request->memory.vram.bytes != hardware.vram_bytes ||
        request->memory.aperture.base != hardware.vram_base ||
        request->memory.aperture.bytes != resources->bar0_bytes ||
        request->memory.aperture_cpu_base != resources->bar0_physical ||
        resources->scanout.base < hardware.vram_base ||
        resources->scanout.base - hardware.vram_base >= resources->bar0_bytes ||
        lfb != resources->bar0_physical + resources->scanout.base - hardware.vram_base) {
        runtime.stage = POLARIS_RUNTIME_FAILED;
        runtime.failed_stage = POLARIS_RUNTIME_MEMORY;
        runtime.error = -1;
        kprintf("[amd-polaris-runtime] stage=failed reason=boot-surface-mapping\n");
        goto done;
    }
    rc = polaris_runtime_start(&runtime, &platform, request);
    if (!rc) fb_set_native_present(hook);
    kprintf("[amd-polaris-runtime] stage=%u failed_stage=%u error=%d "
            "smu=%x sdma=%llu quarantine=%u presenter=%s\n",
            (unsigned)runtime.stage, (unsigned)runtime.failed_stage, runtime.error,
            runtime.smu.load_status, (unsigned long long)runtime.queue.completed,
            runtime.quarantined, !rc ? "installed" : "unavailable");
done:
    fb_graphics_unlock();
    return rc;
}
int polaris_desktop_query(struct polaris_desktop_info *out)
{
    if (!out) return -1;
    fb_graphics_lock();
    *out = (struct polaris_desktop_info) {
        runtime.stage, runtime.failed_stage, runtime.error, runtime.quarantined,
        runtime.queue.submitted, runtime.queue.completed,
        runtime.present.frames, runtime.present.pixels, runtime.present.uploaded_bytes
    };
    fb_graphics_unlock();
    return 0;
}
