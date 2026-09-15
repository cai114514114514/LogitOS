#include "es1370_internal.h"

static int playback_start(struct snd_device *sound)
{
    struct es1370_card *card = sound->priv;
    IO_GUARD(&card->gate);
    return es1370_engine_start_locked(card, ES_PLAYBACK);
}

static void playback_stop(struct snd_device *sound)
{
    struct es1370_card *card = sound->priv;
    IO_GUARD(&card->gate);
    (void)es1370_engine_stop_locked(card, ES_PLAYBACK);
}

static uint64_t playback_position(struct snd_device *sound)
{
    struct es1370_card *card = sound->priv;
    IO_GUARD(&card->gate);
    return card->stream[ES_PLAYBACK].completed_frames;
}

static int capture_start(struct snd_capdevice *capture)
{
    struct es1370_card *card = capture->priv;
    IO_GUARD(&card->gate);
    return es1370_engine_start_locked(card, ES_CAPTURE);
}

static void capture_stop(struct snd_capdevice *capture)
{
    struct es1370_card *card = capture->priv;
    IO_GUARD(&card->gate);
    (void)es1370_engine_stop_locked(card, ES_CAPTURE);
}

void es1370_pcm_initialize(struct es1370_card *card)
{
    card->sound = (struct snd_device){.name = "es1370", .codec = "AK4531 DAC2",
        .rate = ES_RATE, .channels = 2, .format = SND_FMT_S16,
        .period_bytes = ES_PERIOD_BYTES, .periods = ES_PERIODS,
        .ring = card->stream[ES_PLAYBACK].ring->cpu,
        .start = playback_start, .stop = playback_stop,
        .position = playback_position, .priv = card};
    if (card->stream[ES_CAPTURE].ring) {
        card->capture = (struct snd_capdevice){.name = "es1370-in", .codec = "AK4531 mic ADC",
            .rate = ES_RATE, .channels = 2, .format = SND_FMT_S16,
            .period_bytes = ES_PERIOD_BYTES, .periods = ES_PERIODS,
            .ring = card->stream[ES_CAPTURE].ring->cpu,
            .start = capture_start, .stop = capture_stop, .priv = card};
    }
}
