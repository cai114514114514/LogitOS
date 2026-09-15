/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include "ac97_models.h"

/* Intel ICH/ICH0 PRM 298028-001 section 2.2 specifies stereo PCM engines.
 * ICH2 datasheet 290687-002 and ICH3-S datasheet 290733-002 section 13.2.8
 * add GLOB_CNT[21:20] = {2,4,6} channels. Bit 22 remains reserved in both.
 * Linux v6.12 intel8x0.c groups these four IDs as DEVICE_INTEL, distinct
 * from DEVICE_INTEL_ICH4. DMA offsets and sample-count semantics are shared. */
#define AC97_MODEL_ENTRY(id, label, channels) {id, label, channels},
static const struct ac97_model models[] = {
    AC97_INTEL_MODELS(AC97_MODEL_ENTRY)
};
#undef AC97_MODEL_ENTRY

const struct ac97_model *ac97_model_find(uint16_t vendor, uint16_t device)
{
    if (vendor != 0x8086)
        return NULL;
    for (size_t index = 0; index < sizeof(models) / sizeof(models[0]); index++) {
        if (models[index].device_id == device)
            return &models[index];
    }
    return NULL;
}
