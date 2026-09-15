/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ac97_internal.h"

static int wait_adc_ready(struct ac97_controller *controller)
{
    for (unsigned attempt = 0; attempt < AC97_POLL_ATTEMPTS; attempt++) {
        uint16_t power;
        if (ac97_codec_read(controller, AC97_POWER, &power))
            return -1;
        if ((power & (AC97_ANALOG_READY | AC97_ADC_READY)) ==
            (AC97_ANALOG_READY | AC97_ADC_READY))
            return 0;
        controller->bus->delay_us(AC97_POLL_DELAY_US);
    }
    return -1;
}

static int configure_input_rate(struct ac97_controller *controller)
{
    uint16_t capabilities, value;
    if (ac97_codec_read(controller, AC97_EXTENDED_ID, &capabilities))
        return -1;
    if (!(capabilities & AC97_VARIABLE_RATE))
        return 0;
    if (ac97_codec_read(controller, AC97_EXTENDED_STATUS, &value) ||
        (value & AC97_VARIABLE_RATE) ||
        ac97_codec_read(controller, AC97_ADC_RATE, &value) || value != AC97_RATE)
        return -1;
    return 0;
}

static int configure_input_route(struct ac97_controller *controller)
{
    uint16_t selected, gain;
    /* Record Select routes the microphone into both PCM ADC channels. The
     * separate mono MIC DMA engine is deliberately unused: snd consumes a
     * fixed stereo PCM stream. Keep microphone analogue monitoring muted to
     * avoid feeding the recorded signal back into the loudspeakers. */
    if (ac97_codec_write(controller, AC97_MIC_VOLUME, AC97_VOLUME_MUTE) ||
        ac97_codec_write(controller, AC97_RECORD_SELECT, AC97_RECORD_MICROPHONE) ||
        ac97_codec_write(controller, AC97_RECORD_GAIN, AC97_RECORD_GAIN_12DB) ||
        ac97_codec_read(controller, AC97_RECORD_SELECT, &selected) ||
        ac97_codec_read(controller, AC97_RECORD_GAIN, &gain))
        return -1;
    /* AC'97 gain code 8 is +12 dB. QEMU interprets it as 8/15 linear volume;
     * code zero would silently discard all input in that primary emulator. */
    return selected == AC97_RECORD_MICROPHONE &&
           gain == AC97_RECORD_GAIN_12DB ? 0 : -1;
}

int ac97_capture_prepare(struct ac97_controller *capture,
                         const struct ac97_controller *playback)
{
    if (!capture || !playback || capture == playback || !playback->initialized ||
        playback->poisoned || !playback->bus)
        return -1;
    *capture = (struct ac97_controller){
        .bus = playback->bus, .model = playback->model,
        .mixer_port = playback->mixer_port,
        .master_port = playback->master_port, .codec_id = playback->codec_id,
        .engine_offset = AC97_INPUT
    };
    if (wait_adc_ready(capture) || configure_input_rate(capture) ||
        configure_input_route(capture))
        return -1;
    capture->initialized = 1;
    return 0;
}
