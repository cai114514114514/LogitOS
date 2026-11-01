/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AMD_GPU_LEASE_H
#define LOGIT_AMD_GPU_LEASE_H

/* Exclusive ownership of one AMD GPU, as a small state machine.
 *
 * Until stage B3 the AMD boot-framebuffer driver only observed its device
 * (bootfb.c "passive claim"). Any register write, firmware load or engine
 * start needs more than that: nobody else may touch the card while the driver
 * reprograms it, and nothing may touch it again once the driver has left it in
 * a state only the driver understands. The lease records that, in three states:
 *
 *   FREE     nobody has taken the device; the driver is passive.
 *   HELD     the driver owns the device; only the holder may write registers.
 *   RETIRED  the holder is gone or has stopped for good; the device is not
 *            handed out again in this boot. A GPU that failed half way (or one
 *            whose driver was unbound while the card was running) has a state
 *            nobody can describe, so recovery is a reboot, never a re-lease.
 *
 * "No other driver may bind" is enforced by the device model, which binds a
 * device to at most one driver (c/drivers/core/driver.h, dev_probe_all); the
 * lease therefore also requires that the device is bound to the requesting
 * driver, so that a lease cannot outlive its binding unnoticed.
 *
 * Pure bookkeeping: no hardware access, no allocation, boot-time single
 * threaded use only. Model evidence only; not verified on X79-VG4 hardware. */

enum amd_gpu_lease_state {
    AMD_GPU_LEASE_FREE = 0,
    AMD_GPU_LEASE_HELD,
    AMD_GPU_LEASE_RETIRED
};

struct amd_gpu_lease {
    enum amd_gpu_lease_state state;
    const void *holder; /* the device, compared by address only */
};

enum amd_gpu_lease_result {
    AMD_GPU_LEASE_OK = 0,
    AMD_GPU_LEASE_BAD_ARGUMENT = -1,
    AMD_GPU_LEASE_NOT_BOUND = -2,
    AMD_GPU_LEASE_ALREADY_TAKEN = -3,
    AMD_GPU_LEASE_ALREADY_RETIRED = -4
};

/* Take the lease for DEVICE. DEVICE_IS_BOUND says the device is currently
 * bound to the driver asking. Only a FREE lease can be taken. */
int amd_gpu_lease_acquire(struct amd_gpu_lease *lease, const void *device,
                          int device_is_bound);

/* True while DEVICE is the holder of a HELD lease. */
int amd_gpu_lease_holds(const struct amd_gpu_lease *lease, const void *device);

/* End the lease for good if DEVICE holds it (quiesce, unbind). A lease nobody
 * took stays FREE: retiring a device that was never touched would only block
 * a later, legitimate attempt after rebinding. */
void amd_gpu_lease_retire(struct amd_gpu_lease *lease, const void *device);

const char *amd_gpu_lease_result_name(int result);

#endif
