#ifndef LOGIT_NVIDIA_PASCAL_H
#define LOGIT_NVIDIA_PASCAL_H

#include <stdint.h>

struct device;
struct dev_match;

/* Native acceleration staging lives in nvidia_pascal_accel.h.  It is kept out
 * of this passive-observer header so a caller cannot mistake a successful
 * bootfb probe for a working GPU channel. */

/* These names describe NVIDIA's marketed PCI IDs, not a chip register probe.
 * NULL means the ID is outside the deliberately narrow GTX 1050 family. */
const char *nvidia_pascal_model_name(uint16_t device_id);

/* Exposed so the host fixture exercises the exact production probe.  Normal
 * kernels reach it only through DRIVER_DECLARE in nvidia_pascal.c. */
int nvidia_pascal_bootfb_probe(struct device *dev);

#ifdef LOGIT_HOST_TEST
/* Lets the host gate pin the declarative table itself, rather than proving a
 * helper while a stale or broad DRIVER_DECLARE table quietly binds more. */
const struct dev_match *nvidia_pascal_bootfb_match_table(void);
#endif

#endif
