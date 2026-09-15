#include "es1370_internal.h"
#include "io.h"
#include "ktime.h"

#define ES_POLL_LIMIT 100000u
#define ES_CODEC_SETTLE_NS UINT64_C(100000)

static int codec_settle(void)
{
    uint64_t started = time_mono_ns();
    for (unsigned attempt = 0; attempt < ES_POLL_LIMIT; ++attempt) {
        uint64_t now = time_mono_ns();
        if (now < started) {
            return -1;
        }
        if (now - started >= ES_CODEC_SETTLE_NS) {
            return 0;
        }
        io_relax();
    }
    /* A clock that did not advance cannot prove the codec's reset delay. */
    return -1;
}

static int write_codec(struct es1370_card *card, unsigned reg, unsigned value)
{
    if (es1370_wait_clear(card, ES_STATUS, ES_STATUS_CODEC_BUSY)) {
        return -1;
    }
    outw((uint16_t)(card->port + ES_CODEC), (uint16_t)(reg << 8 | value));
    return es1370_wait_clear(card, ES_STATUS, ES_STATUS_CODEC_BUSY);
}

int es1370_codec_configure(struct es1370_card *card)
{
    /* AK4531 reset, clock and mixer routing follow ens1370.c/ak4531_codec.c.
     * DAC2 uses LRCLK2. Mute unrelated analogue sources instead of allowing
     * microphone/line-in noise into playback. PCM remains under mixer gain. */
    if (write_codec(card, AK_RESET, AK_POWERED_RESET) || codec_settle() ||
        write_codec(card, AK_RESET, AK_POWERED_RUNNING) || codec_settle() ||
        write_codec(card, AK_CLOCK, 0)) {
        return -1;
    }
    for (unsigned reg = AK_MASTER_LEFT; reg < AK_MONO_OUTPUT; ++reg) {
        if (write_codec(card, reg, AK_MUTE_VOLUME)) {
            return -1;
        }
    }
    if (write_codec(card, AK_MONO_OUTPUT, AK_MUTE_MONO) ||
        write_codec(card, AK_OUTPUT_SWITCH1, 0) ||
        write_codec(card, AK_OUTPUT_SWITCH2, 0)) {
        return -1;
    }
    for (unsigned reg = AK_INPUT_SWITCH_FIRST; reg <= AK_INPUT_SWITCH_LAST; ++reg) {
        if (write_codec(card, reg, 0)) {
            return -1;
        }
    }
    if (write_codec(card, AK_ADC_INPUT, 0) || write_codec(card, AK_MIC_GAIN, 0) ||
        write_codec(card, AK_VOICE_LEFT, AK_VOICE_UNITY_GAIN) ||
        write_codec(card, AK_VOICE_RIGHT, AK_VOICE_UNITY_GAIN) ||
        write_codec(card, AK_OUTPUT_SWITCH2, AK_VOICE_OUTPUT_BOTH) ||
        write_codec(card, AK_MASTER_LEFT, 0) ||
        write_codec(card, AK_MASTER_RIGHT, 0)) {
        return -1;
    }
    /* Mic feeds both ADC channels at 0 dB without the optional +30 dB boost.
     * Output-switch bits remain off, so recording cannot enable sidetone or
     * acoustic feedback. QEMU's host input bypasses this analogue mixer. */
    if (write_codec(card, AK_MIC_VOLUME, AK_VOICE_UNITY_GAIN) ||
        write_codec(card, AK_INPUT_LEFT_SWITCH1, AK_INPUT_MIC_ROUTE) ||
        write_codec(card, AK_INPUT_RIGHT_SWITCH1, AK_INPUT_MIC_ROUTE)) {
        return -1;
    }
    return 0;
}

