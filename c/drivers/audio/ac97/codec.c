/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ac97_internal.h"

static int codec_acquire(struct ac97_controller *controller)
{
    const struct ac97_bus *bus = controller->bus;
    for (unsigned attempt = 0; attempt < AC97_POLL_ATTEMPTS; attempt++) {
        if (!(bus->read8(controller->master_port + AC97_CODEC_SEMAPHORE) & 1))
            return 0;
        bus->delay_us(AC97_POLL_DELAY_US);
    }
    return -1;
}

int ac97_codec_read(struct ac97_controller *controller, unsigned reg,
                      uint16_t *value)
{
    const struct ac97_bus *bus = controller->bus;
    if (codec_acquire(controller))
        return -1;
    bus->write32(controller->master_port + AC97_GLOBAL_STATUS,
                 AC97_CODEC_READ_TIMEOUT);
    *value = bus->read16(controller->mixer_port + reg);
    if (bus->read32(controller->master_port + AC97_GLOBAL_STATUS) &
        AC97_CODEC_READ_TIMEOUT)
        return -1;
    return 0;
}

int ac97_codec_write(struct ac97_controller *controller, unsigned reg,
                       uint16_t value)
{
    if (codec_acquire(controller))
        return -1;
    controller->bus->write16(controller->mixer_port + reg, value);
    return 0;
}

int ac97_codec_configure_playback(struct ac97_controller *controller)
{
    uint16_t vendor_high, vendor_low, value, capabilities;
    if (ac97_codec_write(controller, AC97_CODEC_RESET, 0) ||
        ac97_codec_write(controller, AC97_POWER, 0))
        return -1;
    for (unsigned attempt = 0; ; attempt++) {
        if (attempt == AC97_POLL_ATTEMPTS ||
            ac97_codec_read(controller, AC97_POWER, &value))
            return -1;
        if ((value & AC97_ANALOG_READY) == AC97_ANALOG_READY)
            break;
        controller->bus->delay_us(AC97_POLL_DELAY_US);
    }
    if (ac97_codec_read(controller, AC97_VENDOR_HIGH, &vendor_high) ||
        ac97_codec_read(controller, AC97_VENDOR_LOW, &vendor_low) ||
        !vendor_high || vendor_high == UINT16_MAX || vendor_low == UINT16_MAX)
        return -1;
    controller->codec_id = ((uint32_t)vendor_high << 16) | vendor_low;
    if (ac97_codec_read(controller, AC97_EXTENDED_ID, &capabilities))
        return -1;
    /* Original fixed-rate codecs have no VRA register. On variable-rate
     * codecs, explicitly disable VRA so firmware's old rate cannot survive. */
    if (capabilities & AC97_VARIABLE_RATE) {
        if (ac97_codec_read(controller, AC97_EXTENDED_STATUS, &value) ||
            ac97_codec_write(controller, AC97_EXTENDED_STATUS,
                         value & ~AC97_VARIABLE_RATE) ||
            ac97_codec_read(controller, AC97_DAC_RATE, &value) || value != AC97_RATE)
            return -1;
    }
    if (ac97_codec_write(controller, AC97_MASTER_VOLUME, 0) ||
        ac97_codec_write(controller, AC97_PCM_VOLUME, 0) ||
        ac97_codec_read(controller, AC97_MASTER_VOLUME, &value) || value != 0 ||
        ac97_codec_read(controller, AC97_PCM_VOLUME, &value) || value != 0)
        return -1;
    return 0;
}

