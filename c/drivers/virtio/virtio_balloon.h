#ifndef LOGIT_VIRTIO_BALLOON_H
#define LOGIT_VIRTIO_BALLOON_H

#include <stdint.h>

/* virtio-balloon (virtio device ID 5, "Memory Balloon Device", virtio-v1.1
 * spec sec 5.5) over the virtio-pci transport (virtio.c). Self-registers with
 * the device model (driver.h) -- adding this file is the whole of adding the
 * driver. See virtio_balloon.c for the device contract, why it exists (this
 * tree's reclaim/swap subsystem needs pressure applied from OUTSIDE the
 * guest, not manufactured by booting with less -m), and the boot self-test.
 *
 * WHAT THIS FILE DOES NOT DO: get itself polled once the desktop is running.
 * The host sets a target in the device's config space (QMP `balloon N`, or a
 * command-line target already latched before the guest's PCI enumeration
 * runs) and virtio-balloon has no mandated interrupt for "the target
 * changed" -- a real driver either handles a config-change interrupt or is
 * polled periodically. This one is polled exactly once, at probe() (see
 * virtio_balloon_poll() below for the one-step primitive a periodic caller
 * would use). Wiring `if (virtio_balloon_present()) virtio_balloon_poll();`
 * into the WM main loop next to `net_poll()` (c/kernel/gui/wm.c, the existing
 * "poll a device from the compositor's loop" pattern) is the natural next
 * step and is reported as a REQUEST rather than done here, because wm.c is
 * owned by another line of work. */

/* 1 once a virtio-balloon device has been found, both queues are up, and
 * DRIVER_OK has been sent. 0 on a machine with no such device. */
int virtio_balloon_present(void);

/* The host's current target, straight out of config.num_pages (4 KiB pages).
 * 0 if the device is not present. */
uint32_t virtio_balloon_target_pages(void);

/* How many frames this driver is currently holding out of circulation (what
 * it will next write to config.actual). */
uint32_t virtio_balloon_held_pages(void);

/* One bounded step toward the host's target: inflate if held < target,
 * deflate if held > target, do nothing if equal, then write the new held
 * count to config.actual. Returns the number of frames moved this call
 * (positive = inflated, negative = deflated, 0 = converged, not present, or a
 * single step could move no more -- e.g. pmm has no free frames left to
 * donate). A step can be partial (see virtio_balloon.c's MAX_BALLOON_FRAMES
 * and the "pmm ran dry" case); the caller decides whether to call again. */
int virtio_balloon_poll(void);

#endif /* LOGIT_VIRTIO_BALLOON_H */
