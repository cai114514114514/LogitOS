#ifndef LOGIT_AMD_BOOTFB_H
#define LOGIT_AMD_BOOTFB_H

struct device;
struct dev_match;
struct driver;

/* Passive ownership probe for an AMD/ATI firmware framebuffer.  Success means
 * only that the Multiboot LFB belongs to this PCI function; it is not a native
 * modesetting or command-submission interface. */
int amd_bootfb_probe(struct device *dev);

/* Single-threaded boot, after the root filesystem mounts. Only the Polaris
 * device that passed framebuffer ownership checks is considered. This reads
 * VBIOS and installed firmware; it does not grant a GPU execution lease. */
void amd_bootfb_probe_resources(void);

/* "module", "vfct" or "none" for the Polaris device whose resources were
 * probed, so the HUD can show where its VBIOS came from; NULL for any other
 * device and before the probe ran. The string is static. */
const char *amd_bootfb_vbios_source(const struct device *dev);

/* HUD lines for the read-only display scan and the VRAM arena choice made by
 * amd_bootfb_probe_resources (index 0: GOP CRTC, or "skipped: <reason>" when
 * the scan did not run; 1: arena). Returns NULL when the line does not exist,
 * so a caller can loop until NULL. The text is the serial log line without its
 * "[amd-dce] " / "[amd-arena] " prefix. The scan runs only in HWHUD builds
 * (LOGIT_HWHUD): a build without it always returns NULL. */
const char *amd_bootfb_display_hud_line(unsigned index);

/* The HUD line of the GPU self-test (stage B3), without its "gpu-rt " label:
 * "stage=... result=... key=... smc=... sdma=...". NULL until the self-test
 * has run or been skipped, and always NULL in a build without HWHUD. The text
 * is the serial log's last stage line, shortened, never separately worded. */
const char *amd_bootfb_gpu_runtime_hud_line(void);

#ifdef LOGIT_HOST_TEST
/* Lets the host gate pin the declarative match table as well as the probe. */
const struct dev_match *amd_bootfb_match_table(void);

/* The driver record itself, so the gate can call its remove and quiesce hooks
 * the way the device model does. */
const struct driver *amd_bootfb_driver_for_test(void);
#endif

#endif
