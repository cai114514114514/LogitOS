/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_AC97_INTERNAL_H
#define LOGIT_AC97_INTERNAL_H
#include "ac97.h"
#include "../../core/driver.h"
#include "../../core/dma.h"
#include "../../core/io_lock.h"
#include "../../../kernel/audio/snd.h"

/* The register engine is direction-neutral. DMA handles remain paired with
 * their matching engine so stopping input never completes output ownership. */
struct ac97_stream {
    struct ac97_controller controller;
    struct dma_buffer *ring, *descriptors;
    uint64_t ring_token, descriptor_token;
};
struct ac97_card {
    struct device *device;
    struct dma_device dma;
    struct ac97_stream playback, capture;
    struct snd_device sound;
    struct snd_capdevice input;
    io_lock_t gate;
    int irq_registered, sound_registered, capture_registered, io_enabled;
    unsigned removing;
};

int ac97_codec_read(struct ac97_controller *, unsigned, uint16_t *);
int ac97_codec_write(struct ac97_controller *, unsigned, uint16_t);
int ac97_codec_configure_playback(struct ac97_controller *);

/* Locked helpers: callbacks acquire owner->gate. PCI teardown drops it before
 * unregistering sound devices or draining IRQ callbacks to avoid inversion. */
void ac97_contain_pci(struct ac97_card *owner);
int ac97_stream_stop_locked(struct ac97_card *, struct ac97_stream *);
int ac97_streams_allocate(struct ac97_card *);
int ac97_streams_free(struct ac97_card *);
void ac97_streams_configure(struct ac97_card *);
void ac97_streams_interrupt(void *);
#endif
