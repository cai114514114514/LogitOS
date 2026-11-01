/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include "amd/gpu_lease.h"

/* Why one function: that the device is bound to this driver is the whole
 * "no other driver may touch it" argument of the lease, so the watched mutation
 * waives exactly this condition and the gate must watch an unbound device
 * obtain the lease. */
static int device_binding_is_acceptable(int device_is_bound)
{
#ifdef AMD_GPU_LEASE_NEGCTL_ACCEPT_UNBOUND
    (void)device_is_bound;
    return 1;
#else
    return device_is_bound;
#endif
}

/* Why the checks come in this order: a retired lease must say "retired" even
 * for the old holder, so the log of a second attempt names the real reason,
 * not "already taken". */
int amd_gpu_lease_acquire(struct amd_gpu_lease *lease, const void *device,
                          int device_is_bound)
{
    if (!lease || !device) {
        return AMD_GPU_LEASE_BAD_ARGUMENT;
    }
    if (lease->state == AMD_GPU_LEASE_RETIRED) {
        return AMD_GPU_LEASE_ALREADY_RETIRED;
    }
    if (lease->state == AMD_GPU_LEASE_HELD) {
        return AMD_GPU_LEASE_ALREADY_TAKEN;
    }
    if (!device_binding_is_acceptable(device_is_bound)) {
        return AMD_GPU_LEASE_NOT_BOUND;
    }
    lease->state = AMD_GPU_LEASE_HELD;
    lease->holder = device;
    return AMD_GPU_LEASE_OK;
}

/* Why compare the holder: the GPU self-test hands this question to code that
 * runs register writes, and a stale pointer to another device must not pass. */
int amd_gpu_lease_holds(const struct amd_gpu_lease *lease, const void *device)
{
    return lease && device && lease->state == AMD_GPU_LEASE_HELD &&
           lease->holder == device;
}

/* Why sticky: see the state descriptions in gpu_lease.h. */
void amd_gpu_lease_retire(struct amd_gpu_lease *lease, const void *device)
{
    if (!amd_gpu_lease_holds(lease, device)) {
        return;
    }
    lease->state = AMD_GPU_LEASE_RETIRED;
}

/* Why words: these appear on the serial line and the HUD. */
const char *amd_gpu_lease_result_name(int result)
{
    switch (result) {
    case AMD_GPU_LEASE_OK:
        return "ok";
    case AMD_GPU_LEASE_BAD_ARGUMENT:
        return "bad-argument";
    case AMD_GPU_LEASE_NOT_BOUND:
        return "device-not-bound-to-amd-bootfb";
    case AMD_GPU_LEASE_ALREADY_TAKEN:
        return "already-taken";
    case AMD_GPU_LEASE_ALREADY_RETIRED:
        return "already-retired";
    default:
        return "unknown";
    }
}
