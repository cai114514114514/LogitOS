/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AC97_MODELS_H
#define LOGIT_AC97_MODELS_H
#include <stdint.h>

/* One inventory feeds both the PCI match table and initialization profiles.
 * ICH4+ is NOT the same profile: extra DMA engines and SDIN steering need a
 * different resource/stop-all path. 440MX also needs its uncached-DMA erratum. */
#define AC97_ICH23_CHANNEL_SELECT UINT32_C(0x00300000)
#define AC97_INTEL_MODELS(ENTRY) \
    ENTRY(0x2415, "82801AA ICH", 0) \
    ENTRY(0x2425, "82801AB ICH0", 0) \
    ENTRY(0x2445, "82801BA/BAM ICH2", AC97_ICH23_CHANNEL_SELECT) \
    ENTRY(0x2485, "82801CA ICH3", AC97_ICH23_CHANNEL_SELECT)

struct ac97_model {
    uint16_t device_id;
    const char *name;
    uint32_t output_channel_mask;
};
const struct ac97_model *ac97_model_find(uint16_t vendor, uint16_t device);
#endif
