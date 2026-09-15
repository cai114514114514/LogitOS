#include "es1370_internal.h"
#include "pci.h"
#include "ktime.h"
#include "kprintf.h"

struct engine_registers {
    uint16_t page, address, size, count;
    uint32_t enable, interrupt, status, format;
};

static const struct engine_registers engines[ES_DIRECTIONS] = {
    {ES_PAGE_DAC, ES_DAC2_FRAME, ES_DAC2_SIZE, ES_DAC2_COUNT,
     ES_CONTROL_DAC2_ENABLE, ES_SERIAL_DAC2_IRQ, ES_STATUS_DAC2, ES_SERIAL_FORMAT},
    {ES_PAGE_ADC, ES_ADC_FRAME, ES_ADC_SIZE, ES_ADC_COUNT,
     ES_CONTROL_ADC_ENABLE, ES_SERIAL_ADC_IRQ, ES_STATUS_ADC, ES_SERIAL_ADC_S16_STEREO}
};

static int any_running(const struct es1370_card *card)
{
    return card->stream[ES_PLAYBACK].running || card->stream[ES_CAPTURE].running;
}

static int complete_buffer(struct es1370_card *card, struct dma_buffer *buffer)
{
    if (buffer && buffer->state == DMA_DEVICE_OWNED &&
        dma_buffer_complete(buffer, buffer->token)) {
        es1370_quarantine(card);
        return -1;
    }
    return 0;
}

int es1370_stop_all_locked(struct es1370_card *card)
{
    card->control = ES_CONTROL_IDLE;
    card->serial = 0;
    for (unsigned direction = 0; direction < ES_DIRECTIONS; ++direction) {
        card->stream[direction].running = 0;
    }
    es1370_write(card, ES_SERIAL, card->serial);
    es1370_write(card, ES_CONTROL, card->control);
    int stopped = es1370_wait_clear(card, ES_CONTROL, ES_CONTROL_DMA_ENABLES) == 0;
    int master_disabled = dev_enable_checked(card->device, 0) == 0;
    if (!stopped || !master_disabled || es1370_read(card, ES_SERIAL) != 0) {
        es1370_quarantine(card);
        return -1;
    }
    for (unsigned direction = 0; direction < ES_DIRECTIONS; ++direction) {
        if (complete_buffer(card, card->stream[direction].ring)) {
            return -1;
        }
    }
    return complete_buffer(card, card->phantom);
}

static int program_stream(struct es1370_card *card, enum es1370_direction direction)
{
    struct es1370_stream *stream = &card->stream[direction];
    const struct engine_registers *registers = &engines[direction];
    if (!es1370_buffer_valid(stream->ring, ES_RING_BYTES) || stream->running) {
        return -1;
    }
    es1370_write(card, ES_MEM_PAGE, registers->page);
    es1370_write(card, registers->address, (uint32_t)dma_addr_value(stream->ring->dma));
    es1370_write(card, registers->size, ES_RING_FRAMES - 1u);
    es1370_write(card, registers->count, ES_PERIOD_FRAMES - 1u);
    card->serial |= registers->format;
    es1370_write(card, ES_SERIAL, card->serial);
    if (es1370_read(card, ES_MEM_PAGE) != registers->page ||
        es1370_read(card, registers->address) != dma_addr_value(stream->ring->dma) ||
        es1370_read(card, registers->size) != ES_RING_FRAMES - 1u ||
        (es1370_read(card, registers->count) & 0xffffu) != ES_PERIOD_FRAMES - 1u ||
        es1370_read(card, ES_SERIAL) != card->serial) {
        return -1;
    }
    return 0;
}

int es1370_engines_initialize(struct es1370_card *card)
{
    if (!es1370_buffer_valid(card->phantom, ES_PHANTOM_BYTES)) {
        return -1;
    }
    /* Phantom DMA remains separate from BOTH rings and is programmed only
     * while global BME is off. A per-direction restart never rewrites it. */
    es1370_write(card, ES_MEM_PAGE, ES_PAGE_ADC);
    es1370_write(card, ES_PHANTOM_FRAME, (uint32_t)dma_addr_value(card->phantom->dma));
    es1370_write(card, ES_PHANTOM_SIZE, 0);
    for (unsigned direction = 0; direction < ES_DIRECTIONS; ++direction) {
        if (card->stream[direction].ring && program_stream(card,
                (enum es1370_direction)direction)) {
            return -1;
        }
    }
    return 0;
}

int es1370_engine_stop_locked(struct es1370_card *card, enum es1370_direction direction)
{
    const struct engine_registers *registers = &engines[direction];
    struct es1370_stream *stream = &card->stream[direction];
    stream->running = 0;
    card->serial &= ~registers->interrupt;
    card->control &= ~registers->enable;
    es1370_write(card, ES_SERIAL, card->serial);
    es1370_write(card, ES_CONTROL, card->control);
    if (es1370_wait_clear(card, ES_CONTROL, registers->enable) ||
        es1370_read(card, ES_CONTROL) != card->control ||
        es1370_read(card, ES_SERIAL) != card->serial) {
        es1370_quarantine(card);
        return -1;
    }
    /* CONTROL/SERIAL and the divider are shared. Clearing global BME while
     * the other direction runs would silently stop its DMA and phantom target.
     * A confirmed per-engine disable is the DMA completion proof in duplex. */
    if (!any_running(card) && dev_enable_checked(card->device, 0)) {
        es1370_quarantine(card);
        return -1;
    }
    if (complete_buffer(card, stream->ring)) {
        return -1;
    }
    if (!any_running(card)) {
        return complete_buffer(card, card->phantom);
    }
    return 0;
}

static int reject_start(struct es1370_card *card, enum es1370_direction direction)
{
    /* A fault disables the shared ISR, so another live engine cannot remain
     * running until its next interrupt. Stop publication and DMA immediately.
     * With no other engine running, a confirmed stop can release its tokens. */
    if (any_running(card)) {
        es1370_quarantine(card);
    } else {
        (void)es1370_engine_stop_locked(card, direction);
        card->faulted = 1;
    }
    return -1;
}

int es1370_engine_start_locked(struct es1370_card *card, enum es1370_direction direction)
{
    struct es1370_stream *stream = &card->stream[direction];
    const struct engine_registers *registers = &engines[direction];
    if (card->faulted || !stream->registered || !card->irq_registered) {
        return -1;
    }
    if (stream->running) {
        return 0;
    }
    if (es1370_engine_stop_locked(card, direction) || program_stream(card, direction)) {
        es1370_quarantine(card);
        return -1;
    }
    int other_running = any_running(card);
    if (!dma_buffer_submit(stream->ring) ||
        (!other_running && !dma_buffer_submit(card->phantom))) {
        return reject_start(card, direction);
    }
    dma_wmb();
    if (!other_running && dev_enable_checked(card->device, 1)) {
        return reject_start(card, direction);
    }
    uint16_t command = pci_cfg_read16(card->device->bus, card->device->slot,
                                      card->device->func, PCI_CFG_COMMAND);
    if (command == UINT16_MAX || (command & (PCI_CMD_IO | PCI_CMD_MASTER)) !=
        (PCI_CMD_IO | PCI_CMD_MASTER)) {
        es1370_quarantine(card);
        return -1;
    }
    stream->completed_frames = 0;
    stream->observed_frames = 0;
    stream->last_hardware_frame = 0;
    stream->last_progress_ns = time_mono_ns();
    card->serial |= registers->interrupt;
    card->control |= registers->enable;
    es1370_write(card, ES_SERIAL, card->serial);
    es1370_write(card, ES_CONTROL, card->control);
    if (es1370_read(card, ES_CONTROL) != card->control ||
        es1370_read(card, ES_SERIAL) != card->serial) {
        return reject_start(card, direction);
    }
    stream->running = 1;
    kprintf("[es1370] %s started rate=%u stereo=s16 period=%u periods=%u\n",
            direction == ES_CAPTURE ? "ADC" : "DAC2", ES_RATE,
            ES_PERIOD_BYTES, ES_PERIODS);
    return 0;
}

static int completed_periods(struct es1370_card *card, enum es1370_direction direction)
{
    struct es1370_stream *stream = &card->stream[direction];
    const struct engine_registers *registers = &engines[direction];
    es1370_write(card, ES_MEM_PAGE, registers->page);
    uint32_t size_position = es1370_read(card, registers->size);
    uint64_t now = time_mono_ns();
    unsigned frame = size_position >> 16;
    if (size_position == UINT32_MAX || frame >= ES_RING_FRAMES ||
        (size_position & 0xffffu) != ES_RING_FRAMES - 1u ||
        now < stream->last_progress_ns ||
        now - stream->last_progress_ns >= ES_RING_DURATION_NS) {
        return -1;
    }
    unsigned advanced = (frame + ES_RING_FRAMES - stream->last_hardware_frame) %
                          ES_RING_FRAMES;
    uint64_t observed = stream->observed_frames + advanced;
    unsigned periods = (unsigned)((observed - stream->completed_frames) / ES_PERIOD_FRAMES);
    if (!advanced || !periods || periods >= ES_PERIODS) {
        return -1;
    }
    stream->last_hardware_frame = frame;
    stream->last_progress_ns = now;
    stream->observed_frames = observed;
    return (int)periods;
}

void es1370_interrupt(void *opaque)
{
    struct es1370_card *card = opaque;
    IO_GUARD(&card->gate);
    if (!card->io_enabled) {
        return;
    }
    if (card->faulted) {
        es1370_quarantine(card);
        return;
    }
    uint32_t status = es1370_read(card, ES_STATUS);
    if (status == UINT32_MAX) {
        es1370_quarantine(card);
        return;
    }
    if (!(status & ES_STATUS_INTERRUPT)) {
        return;
    }
    uint32_t acknowledged_serial = card->serial;
    for (unsigned direction = 0; direction < ES_DIRECTIONS; ++direction) {
        if (status & engines[direction].status) {
            acknowledged_serial &= ~engines[direction].interrupt;
        }
    }
    /* Clear only pending sources. A DAC2 interrupt must not erase ADC INT_EN,
     * and an ADC interrupt must not pause the playback format or clock. */
    es1370_write(card, ES_SERIAL, acknowledged_serial);
    for (unsigned direction = 0; direction < ES_DIRECTIONS; ++direction) {
        struct es1370_stream *stream = &card->stream[direction];
        if (!(status & engines[direction].status) || !stream->running || !stream->registered) {
            continue;
        }
        int periods = completed_periods(card, (enum es1370_direction)direction);
        if (periods < 0) {
            es1370_quarantine(card);
            return;
        }
        if (direction == ES_CAPTURE) {
            /* Coherent DMA still requires ordering before publishing device
             * writes to the capture worker. Never complete the live ring's
             * DMA token merely because one period became readable. */
            dma_rmb();
        }
        for (int period = 0; period < periods; ++period) {
            stream->completed_frames += ES_PERIOD_FRAMES;
            if (direction == ES_CAPTURE) {
#ifndef ES1370_NEGCTL_DROP_CAPTURE
                snd_capture_period_elapsed(&card->capture);
#endif
            } else {
#ifndef ES1370_NEGCTL_DROP_ELAPSED
                snd_period_elapsed(&card->sound);
#endif
            }
        }
    }
    es1370_write(card, ES_SERIAL, card->serial);
}
