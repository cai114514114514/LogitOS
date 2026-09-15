#ifndef LOGIT_AMD_BOOTFB_H
#define LOGIT_AMD_BOOTFB_H

struct device;
struct dev_match;

/* Passive ownership probe for an AMD/ATI firmware framebuffer.  Success means
 * only that the Multiboot LFB belongs to this PCI function; it is not a native
 * modesetting or command-submission interface. */
int amd_bootfb_probe(struct device *dev);

/* Single-threaded boot, after the root filesystem mounts. Only the Polaris
 * device that passed framebuffer ownership checks is considered. This reads
 * VBIOS and installed firmware; it does not grant a GPU execution lease. */
void amd_bootfb_probe_resources(void);

#ifdef LOGIT_HOST_TEST
/* Lets the host gate pin the declarative match table as well as the probe. */
const struct dev_match *amd_bootfb_match_table(void);
#endif

#endif
