/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ac97_internal.h"

static void quarantine_card(struct ac97_card *owner)
{
    /* PCI bus mastering is shared. A failed input halt invalidates output's
     * ability to run too, but cannot be used as proof either buffer is idle. */
    owner->playback.controller.poisoned = 1;
    owner->capture.controller.poisoned = 1;
    owner->playback.controller.running = 0;
    owner->capture.controller.running = 0;
    dma_device_quarantine(&owner->dma);
    ac97_contain_pci(owner);
}

int ac97_stream_stop_locked(struct ac97_card *owner, struct ac97_stream *stream)
{
    if (!owner->io_enabled)
        return stream->controller.poisoned ? -1 : 0;
    if (!stream->controller.bus)
        return 0;
    if (ac97_controller_stop(&stream->controller)) {
        quarantine_card(owner);
        return -1;
    }
    if (stream->ring_token) {
        if (dma_buffer_complete(stream->ring, stream->ring_token)) {
            quarantine_card(owner);
            return -1;
        }
        stream->ring_token = 0;
    }
    if (stream->descriptor_token) {
        if (dma_buffer_complete(stream->descriptors, stream->descriptor_token)) {
            quarantine_card(owner);
            return -1;
        }
        stream->descriptor_token = 0;
    }
    return 0;
}

static int start_stream_locked(struct ac97_card *owner, struct ac97_stream *stream)
{
    if (__atomic_load_n(&owner->removing, __ATOMIC_ACQUIRE) ||
        !owner->io_enabled || owner->dma.blocked || stream->controller.running ||
        !stream->controller.initialized || !stream->ring || !stream->descriptors)
        return -1;
    stream->ring_token = dma_buffer_submit(stream->ring);
    stream->descriptor_token = dma_buffer_submit(stream->descriptors);
    if (!stream->ring_token || !stream->descriptor_token ||
        ac97_controller_start(&stream->controller,
                              dma_addr_value(stream->descriptors->dma))) {
        ac97_stream_stop_locked(owner, stream);
        return -1;
    }
    return 0;
}

static int start_playback(struct snd_device *sound)
{
    struct ac97_card *owner = sound->priv;
    IO_GUARD(&owner->gate);
    return start_stream_locked(owner, &owner->playback);
}

static void stop_playback(struct snd_device *sound)
{
    struct ac97_card *owner = sound->priv;
    IO_GUARD(&owner->gate);
    ac97_stream_stop_locked(owner, &owner->playback);
}

static uint64_t playback_position(struct snd_device *sound)
{
    struct ac97_card *owner = sound->priv;
    IO_GUARD(&owner->gate);
    return ac97_controller_position(&owner->playback.controller);
}

static int start_capture(struct snd_capdevice *input)
{
    struct ac97_card *owner = input->priv;
    IO_GUARD(&owner->gate);
    return start_stream_locked(owner, &owner->capture);
}

static void stop_capture(struct snd_capdevice *input)
{
    struct ac97_card *owner = input->priv;
    IO_GUARD(&owner->gate);
    ac97_stream_stop_locked(owner, &owner->capture);
}

static int allocate_stream(struct ac97_card *owner, struct ac97_stream *stream)
{
    stream->ring = dma_alloc_coherent(&owner->dma, AC97_RING_BYTES, 4096, 0);
    stream->descriptors = dma_alloc_coherent(&owner->dma, 4096, 4096, 0);
    if (!stream->ring || !stream->descriptors)
        return -1;
    return ac97_build_descriptors(stream->descriptors->cpu,
        stream->descriptors->size, dma_addr_value(stream->ring->dma));
}

int ac97_streams_allocate(struct ac97_card *owner)
{
    if (allocate_stream(owner, &owner->playback))
        return -1;
    if (owner->capture.controller.initialized)
        return allocate_stream(owner, &owner->capture);
    return 0;
}

static int free_stream(struct ac97_stream *stream)
{
    if (stream->descriptors) {
        if (dma_free_coherent(stream->descriptors))
            return -1;
        stream->descriptors = NULL;
    }
    if (stream->ring) {
        if (dma_free_coherent(stream->ring))
            return -1;
        stream->ring = NULL;
    }
    return 0;
}

int ac97_streams_free(struct ac97_card *owner)
{
    /* The PCI owner has already stopped BOTH engines, drained the shared IRQ,
     * unregistered both consumers and confirmed BME disabled before this. */
    if (free_stream(&owner->capture))
        return -1;
    return free_stream(&owner->playback);
}

static void codec_label(char *destination, uint32_t codec)
{
    static const char prefix[] = "AC97 codec ";
    static const char hex[] = "0123456789abcdef";
    unsigned index = 0;
    while (prefix[index]) {
        destination[index] = prefix[index];
        index++;
    }
    for (unsigned digit = 0; digit < 8; digit++)
        destination[index++] = hex[(codec >> (28 - digit * 4)) & 15];
    destination[index] = 0;
}

void ac97_streams_configure(struct ac97_card *owner)
{
    owner->sound = (struct snd_device){
        .name = "ac97", .rate = AC97_RATE, .channels = 2, .format = SND_FMT_S16,
        .period_bytes = AC97_PERIOD_BYTES, .periods = AC97_RING_PERIODS,
        .ring = owner->playback.ring->cpu, .start = start_playback,
        .stop = stop_playback, .position = playback_position,
        .priv = owner, .irq_mode = owner->device->irq_mode
    };
    codec_label(owner->sound.codec, owner->playback.controller.codec_id);
    if (!owner->capture.controller.initialized)
        return;
    owner->input = (struct snd_capdevice){
        .name = "ac97-in", .rate = AC97_RATE, .channels = 2, .format = SND_FMT_S16,
        .period_bytes = AC97_PERIOD_BYTES, .periods = AC97_RING_PERIODS,
        .ring = owner->capture.ring->cpu, .start = start_capture,
        .stop = stop_capture, .priv = owner, .irq_mode = owner->device->irq_mode
    };
    codec_label(owner->input.codec, owner->capture.controller.codec_id);
}

void ac97_streams_interrupt(void *argument)
{
    struct ac97_card *owner = argument;
    IO_GUARD(&owner->gate);
    if (!owner->io_enabled) {
        ac97_contain_pci(owner);
        return;
    }
    if (__atomic_load_n(&owner->removing, __ATOMIC_ACQUIRE))
        return;
    int output_periods = ac97_controller_interrupt(&owner->playback.controller);
    if (output_periods < 0)
        ac97_stream_stop_locked(owner, &owner->playback);
    /* A failed output stop may disable shared PCI decode. Never touch input
     * registers after that transition, even from this same IRQ callback. */
    if (!owner->io_enabled)
        return;
    int input_periods = ac97_controller_interrupt(&owner->capture.controller);
    if (input_periods < 0)
        ac97_stream_stop_locked(owner, &owner->capture);
    if (!owner->io_enabled)
        return;
    for (int period = 0; owner->sound_registered && period < output_periods; period++)
        snd_period_elapsed(&owner->sound);
    /* Input bytes must be visible before the kernel is notified to copy them.
     * The coherent ring remains device-owned across interrupts; only a proven
     * stop completes its DMA cookie. capture.c handles consumer overruns. */
    if (input_periods > 0)
        dma_rmb();
    for (int period = 0; owner->capture_registered && period < input_periods; period++)
        snd_capture_period_elapsed(&owner->input);
}
