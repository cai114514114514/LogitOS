#ifndef LOGIT_ES1370_INTERNAL_H
#define LOGIT_ES1370_INTERNAL_H

#include "registers.h"
#include "../../core/driver.h"
#include "../../core/dma.h"
#include "../../core/io_lock.h"
#include "snd.h"

#define ES_PERIOD_BYTES 4096u
#define ES_PERIODS 8u
#define ES_RING_BYTES (ES_PERIOD_BYTES * ES_PERIODS)
#define ES_FRAME_BYTES 4u
#define ES_RING_FRAMES (ES_RING_BYTES / ES_FRAME_BYTES)
#define ES_PERIOD_FRAMES (ES_PERIOD_BYTES / ES_FRAME_BYTES)
#define ES_RING_DURATION_NS ((uint64_t)ES_RING_FRAMES * UINT64_C(1000000000) / ES_RATE)
#define ES_PHANTOM_BYTES 4096u

enum es1370_direction { ES_PLAYBACK, ES_CAPTURE, ES_DIRECTIONS };

struct es1370_stream {
    struct dma_buffer *ring;
    unsigned registered;
    unsigned running;
    unsigned last_hardware_frame;
    uint64_t last_progress_ns;
    uint64_t observed_frames;
    uint64_t completed_frames;
};

struct es1370_card {
    struct device *device;
    struct snd_device sound;
    struct snd_capdevice capture;
    struct dma_device dma;
    struct dma_buffer *phantom;
    struct es1370_stream stream[ES_DIRECTIONS];
    io_lock_t gate;
    uint16_t port;
    unsigned io_enabled;
    unsigned irq_registered;
    unsigned faulted;
    unsigned tombstone_recorded;
    uint32_t control;
    uint32_t serial;
};

/* All register access and engine transitions use the card gate. It protects
 * shared CONTROL/SERIAL, the banked memory window and the common BME lifetime.
 * Only probe before IRQ publication may call these without acquiring it. */
uint32_t es1370_read(struct es1370_card *, uint16_t offset);
void es1370_write(struct es1370_card *, uint16_t offset, uint32_t value);
int es1370_wait_clear(struct es1370_card *, uint16_t offset, uint32_t mask);
int es1370_function_quarantined(const struct device *);
void es1370_record_tombstone(struct es1370_card *);
void es1370_quarantine(struct es1370_card *);
int es1370_codec_configure(struct es1370_card *);
int es1370_buffer_valid(const struct dma_buffer *, size_t bytes);
int es1370_engines_initialize(struct es1370_card *);
int es1370_engine_start_locked(struct es1370_card *, enum es1370_direction);
int es1370_engine_stop_locked(struct es1370_card *, enum es1370_direction);
int es1370_stop_all_locked(struct es1370_card *);
void es1370_interrupt(void *);
void es1370_pcm_initialize(struct es1370_card *);

#endif
