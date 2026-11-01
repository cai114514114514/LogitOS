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

/* Why one function decides: the present hook replaces the CPU copy of every
 * frame, so installing it where the caller asked only for a self-test would
 * hand the screen to an unproven GPU path. Keeping the decision in one place
 * lets the host gate break exactly this line and watch the self-test checks go
 * red. */
static int hook_may_be_installed(enum polaris_desktop_mode mode)
{
#ifdef POLARIS_DESKTOP_NEGCTL_SELF_TEST_INSTALLS_HOOK
    (void)mode;
    return 1;
#else
    return mode == POLARIS_DESKTOP_MODE_PRESENT;
#endif
}

/* Why a label: the log line is the only thing a machine without a debugger
 * shows, and "presenter=installed" on a self-test run would be a lie. */
static const char *presenter_label(enum polaris_desktop_mode mode, int started)
{
    if (!started) {
        return "unavailable";
    }
    return hook_may_be_installed(mode) ? "installed" : "self-test-only";
}

/* Why separate from run: stage B3 must read the SMU security key through the
 * bound platform before it knows which firmware file the runtime request
 * carries, so binding (read-only) and starting (writes) are two steps. The
 * checks are the ones polaris_desktop_start always made, in the same order. */
int polaris_desktop_bind(const struct polaris_native_resources *resources,
                         const struct polaris_runtime_request *request,
                         struct polaris_platform *platform)
{
    int result = POLARIS_DESKTOP_REFUSED;
    uint64_t lfb;
    uint64_t bytes;
    if (!resources || !request || !platform) {
        return result;
    }
    fb_graphics_lock();
    if (hardware.bound || runtime.attempted || runtime.quarantined) {
        goto done;
    }
    if (!fb_boot_lfb_range(&lfb, &bytes) ||
        request->width != fb_width() || request->height != fb_height() ||
        bytes != (uint64_t)request->pitch * request->height ||
        resources->arena.base != request->memory.arena.base ||
        resources->arena.bytes != request->memory.arena.bytes ||
        resources->scanout.base != request->memory.scanout.base ||
        resources->scanout.bytes != request->memory.scanout.bytes ||
        resources->scanout.bytes != bytes ||
        polaris_native_bind(&hardware, resources, platform)) {
        goto done;
    }
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
        runtime.error = POLARIS_DESKTOP_REFUSED;
        kprintf("[amd-polaris-runtime] stage=failed reason=boot-surface-mapping\n");
        goto done;
    }
    result = POLARIS_DESKTOP_OK;
done:
    fb_graphics_unlock();
    return result;
}

/* Why the mode is a parameter and not a global: the decision to hand the screen
 * to the GPU belongs to the caller of one specific start, and a test can then
 * show that a self-test start never reaches fb_set_native_present. A bind that
 * failed its consistency check leaves the runtime in FAILED; a runtime in that
 * state is never started. */
int polaris_desktop_run(const struct polaris_platform *platform,
                        const struct polaris_runtime_request *request,
                        enum polaris_desktop_mode mode)
{
    int result = POLARIS_DESKTOP_REFUSED;
    if (!platform || !request) {
        return result;
    }
    fb_graphics_lock();
    if (!hardware.bound || runtime.attempted || runtime.quarantined ||
        runtime.stage == POLARIS_RUNTIME_FAILED) {
        goto done;
    }
    result = polaris_runtime_start(&runtime, platform, request);
    if (!result && hook_may_be_installed(mode)) {
        fb_set_native_present(hook);
    }
    kprintf("[amd-polaris-runtime] stage=%u failed_stage=%u error=%d "
            "smu=%x sdma=%llu quarantine=%u presenter=%s\n",
            (unsigned)runtime.stage, (unsigned)runtime.failed_stage, runtime.error,
            runtime.smu.load_status, (unsigned long long)runtime.queue.completed,
            runtime.quarantined, presenter_label(mode, !result));
done:
    fb_graphics_unlock();
    return result;
}

/* Why it stays: every caller before stage B3 asks for bind and PRESENT-mode run
 * in one call, and the existing model gates pin that behaviour. */
int polaris_desktop_start(const struct polaris_native_resources *resources,
                           const struct polaris_runtime_request *request)
{
    struct polaris_platform platform;
    if (polaris_desktop_bind(resources, request, &platform)) {
        return POLARIS_DESKTOP_REFUSED;
    }
    return polaris_desktop_run(&platform, request, POLARIS_DESKTOP_MODE_PRESENT);
}

/* Why under the lock: the counters are written by the present hook. */
int polaris_desktop_query(struct polaris_desktop_info *out)
{
    if (!out) return POLARIS_DESKTOP_REFUSED;
    fb_graphics_lock();
    *out = (struct polaris_desktop_info) {
        .stage = runtime.stage,
        .failed_stage = runtime.failed_stage,
        .error = runtime.error,
        .quarantined = runtime.quarantined,
        .submitted = runtime.queue.submitted,
        .completed = runtime.queue.completed,
        .frames = runtime.present.frames,
        .pixels = runtime.present.pixels,
        .uploaded_bytes = runtime.present.uploaded_bytes,
        .smu_load_status = runtime.smu.load_status,
        .smu_upload_microseconds = runtime.smu.upload_microseconds,
        .smu_previous_response_anomalies = runtime.smu.previous_response_anomalies,
        .smu_previous_response_first = runtime.smu.previous_response_first
    };
    fb_graphics_unlock();
    return 0;
}
