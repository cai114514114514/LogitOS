/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AC97_H
#define LOGIT_AC97_H
#include <stdint.h>
#include <stddef.h>
#include "ac97_regs.h"
#include "ac97_models.h"

/* Bus callbacks use absolute I/O ports, matching the native implementation.
 * All controller calls require the owner's IRQ-safe gate. Callbacks neither
 * allocate nor reenter the controller; the core contains no global state. */
struct ac97_bus {
    uint8_t (*read8)(uint16_t port);
    uint16_t (*read16)(uint16_t port);
    uint32_t (*read32)(uint16_t port);
    void (*write8)(uint16_t port, uint8_t value);
    void (*write16)(uint16_t port, uint16_t value);
    void (*write32)(uint16_t port, uint32_t value);
    void (*delay_us)(unsigned microseconds);
    void (*publish)(void);
};
struct ac97_descriptor {
    uint32_t address;
    uint32_t samples_flags;
};
struct ac97_controller {
    const struct ac97_bus *bus;
    const struct ac97_model *model;
    uint16_t mixer_port, master_port;
    unsigned engine_offset; /* separate state for PCM input or output */
    uint32_t codec_id;
    unsigned next_completed, last_valid;
    uint64_t completed_frames, last_position;
    int initialized, running, poisoned;
    int terminal_awaiting_fetch;
};

int ac97_controller_init(struct ac97_controller *controller,
                         const struct ac97_bus *bus,
                         uint16_t mixer_port, uint16_t master_port,
                         const struct ac97_model *model);
int ac97_build_descriptors(struct ac97_descriptor *descriptors,
                           size_t descriptor_bytes, uint64_t ring_address);
int ac97_controller_start(struct ac97_controller *controller,
                          uint64_t descriptor_address);
/* Stop failure is sticky: DMA addresses must remain allocated until a higher
 * level proves a hardware reset. This driver deliberately offers no recovery
 * API that could turn an unacknowledged stop into permission to free RAM. */
int ac97_controller_stop(struct ac97_controller *controller);
int ac97_controller_interrupt(struct ac97_controller *controller);
uint64_t ac97_controller_position(struct ac97_controller *controller);

/* Configure the primary codec ADC without resetting an active playback link.
 * The returned input controller owns independent progress/start/stop state. */
int ac97_capture_prepare(struct ac97_controller *capture,
                         const struct ac97_controller *playback);

extern const struct ac97_bus ac97_native_bus;
struct device;
int ac97_pci_probe(struct device *device);
void ac97_pci_remove(struct device *device);
#endif
