#ifndef LOGIT_INTEL_BOOTFB_H
#define LOGIT_INTEL_BOOTFB_H

#include <stdint.h>

struct device;
struct dev_match;

/* Observe a firmware-initialised Intel framebuffer after proving that the
 * complete Multiboot LFB belongs to this exact PCI function. */
int intel_bootfb_probe(struct device *dev);

#ifdef LOGIT_HOST_TEST
/* Keep the declarative table observable in the host gate.  The production
 * driver reaches it only through DRIVER_DECLARE in intel_bootfb.c. */
const struct dev_match *intel_bootfb_match_table(void);
#endif

#endif
