#ifndef LOGIT_VIRTIO_RNG_H
#define LOGIT_VIRTIO_RNG_H

/* virtio-rng: a hardware/host entropy source over the virtio-pci transport
 * (virtio.c). Self-registers with the device model (driver.h) -- adding this
 * file is the whole of adding the driver, nothing else needs editing for it
 * to probe and come up. See virtio_rng.c for the device contract, why it
 * exists (rng.c's RDSEED/RDRAND-less fallback), and the boot self-test. */

/* 1 once a virtio-rng device has been found and brought up (DRIVER_OK sent).
 * 0 on a machine with no such device -- callers must have another source or
 * accept the weak fallback; this driver invents no entropy of its own. */
int virtio_rng_present(void);

/* Fill buf[0..len) with device-supplied random bytes. Loops internally in
 * bounded chunks so any len is accepted; a virtio-rng completion may fill a
 * request only PARTIALLY (virtio-v1.1 sec 5.4.5 places no lower bound on
 * how much of a buffer one completion fills), so the return value can be
 * less than len even on success -- callers that need exactly len bytes must
 * check the return and retry for the remainder, exactly like the SYS_GETRANDOM
 * loop in rng.c already does around kernel_random_bytes.
 * Returns bytes filled (>= 0), or -1 if the device is not present, len <= 0,
 * or the very first chunk failed outright. */
int virtio_rng_get(void *buf, int len);

#endif /* LOGIT_VIRTIO_RNG_H */
