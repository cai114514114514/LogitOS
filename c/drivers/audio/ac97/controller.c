/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ac97_internal.h"

static int wait_halted(struct ac97_controller *controller, unsigned engine)
{
    const struct ac97_bus *bus = controller->bus;
    unsigned base = controller->master_port + engine;
    for (unsigned attempt = 0; attempt < AC97_POLL_ATTEMPTS; attempt++) {
        unsigned control = bus->read8(base + AC97_CONTROL);
        unsigned status = bus->read16(base + AC97_STATUS);
        if (!(control & AC97_RUN) && (status & AC97_HALTED))
            return 0;
        bus->delay_us(AC97_POLL_DELAY_US);
    }
    controller->poisoned = 1;
    return -1;
}

static int reset_engine(struct ac97_controller *controller)
{
    const struct ac97_bus *bus = controller->bus;
    unsigned base = controller->master_port + controller->engine_offset;
    bus->write8(base + AC97_CONTROL, AC97_RESET_REGISTERS);
    for (unsigned attempt = 0; attempt < AC97_POLL_ATTEMPTS; attempt++) {
        unsigned control = bus->read8(base + AC97_CONTROL);
        if (!(control & (AC97_RESET_REGISTERS | AC97_RUN)) &&
            (bus->read16(base + AC97_STATUS) & AC97_HALTED)) {
            bus->write16(base + AC97_STATUS, AC97_STATUS_ACK);
            return 0;
        }
        bus->delay_us(AC97_POLL_DELAY_US);
    }
    controller->poisoned = 1;
    return -1;
}

static int reset_link(struct ac97_controller *controller)
{
    const struct ac97_bus *bus = controller->bus;
    unsigned port = controller->master_port + AC97_GLOBAL_CONTROL;
    uint32_t control = bus->read32(port);
    /* ICH2/3 may inherit a 4/6-channel stream format from firmware. Their
     * bit 22 is reserved, unlike ICH4's sample-width bit; preserve it. The
     * original ICH/ICH0 only has stereo and no channel selector to modify. */
    uint32_t channel_mask = controller->model->output_channel_mask;
    control &= ~(AC97_LINK_OFF | channel_mask);
    control |= (control & AC97_COLD_RELEASE) ?
               AC97_WARM_RESET : AC97_COLD_RELEASE;
    bus->write32(port, control);
    /* Cold release is a level, warm reset is self-clearing. Readiness comes
     * from the link, not from assuming a successful control-register write. */
    for (unsigned attempt = 0; attempt < AC97_POLL_ATTEMPTS; attempt++) {
        if (!(bus->read32(port) & (AC97_WARM_RESET | channel_mask)) &&
            (bus->read32(controller->master_port + AC97_GLOBAL_STATUS) &
             AC97_PRIMARY_READY))
            return 0;
        bus->delay_us(AC97_POLL_DELAY_US);
    }
    return -1;
}

int ac97_controller_init(struct ac97_controller *controller,
                         const struct ac97_bus *bus,
                         uint16_t mixer_port, uint16_t master_port,
                         const struct ac97_model *model)
{
    if (!controller || !model || !bus || !bus->read8 || !bus->read16 || !bus->read32 ||
        !bus->write8 || !bus->write16 || !bus->write32 || !bus->delay_us ||
        !bus->publish || !mixer_port || mixer_port > UINT16_MAX - 255 ||
        !master_port || master_port > UINT16_MAX - 63)
        return -1;
    *controller = (struct ac97_controller){
        .bus = bus, .model = model, .mixer_port = mixer_port, .master_port = master_port,
        .engine_offset = AC97_OUTPUT
    };
    /* Firmware may have left capture running too. BME must be disabled by
     * the PCI owner before this call, and may only be granted after it passes. */
    for (unsigned engine = AC97_INPUT; engine <= AC97_MIC; engine += 0x10) {
        bus->write8(master_port + engine + AC97_CONTROL, 0);
        if (wait_halted(controller, engine))
            return -1;
    }
    if (reset_link(controller) || reset_engine(controller) ||
        ac97_codec_configure_playback(controller))
        return -1;
    controller->initialized = 1;
    return 0;
}

int ac97_build_descriptors(struct ac97_descriptor *descriptors,
                           size_t descriptor_bytes, uint64_t ring_address)
{
    if (!descriptors || descriptor_bytes <
        sizeof(*descriptors) * AC97_DESCRIPTOR_COUNT ||
        ((uintptr_t)descriptors & 3) || (ring_address & 3) ||
        ring_address > UINT32_MAX - (AC97_RING_BYTES - 1))
        return -1;
    for (unsigned index = 0; index < AC97_DESCRIPTOR_COUNT; index++) {
        descriptors[index].address = (uint32_t)ring_address +
            (index % AC97_RING_PERIODS) * AC97_PERIOD_BYTES;
        /* Length is individual 16-bit samples, NOT stereo frames or bytes. */
        descriptors[index].samples_flags = AC97_DESCRIPTOR_IRQ |
                                           (AC97_PERIOD_BYTES / 2);
    }
    return 0;
}

int ac97_controller_start(struct ac97_controller *controller,
                          uint64_t descriptor_address)
{
    if (!controller || !controller->initialized || controller->poisoned ||
        controller->running || (descriptor_address & 3) ||
        descriptor_address > UINT32_MAX -
                             (AC97_DESCRIPTOR_COUNT * 8 - 1))
        return -1;
    if (reset_engine(controller))
        return -1;
    unsigned base = controller->master_port + controller->engine_offset;
    controller->next_completed = 0;
    controller->last_valid = AC97_DESCRIPTOR_COUNT - 1;
    controller->completed_frames = 0;
    controller->last_position = 0;
    controller->terminal_awaiting_fetch = 0;
    controller->bus->publish();
    controller->bus->write32(base + AC97_DESCRIPTOR_BASE,
                             (uint32_t)descriptor_address);
    controller->bus->write8(base + AC97_LAST_INDEX, controller->last_valid);
    controller->running = 1;
    controller->bus->write8(base + AC97_CONTROL,
        AC97_RUN | AC97_LAST_IRQ | AC97_FIFO_IRQ | AC97_PERIOD_IRQ);
    if (!(controller->bus->read8(base + AC97_CONTROL) & AC97_RUN)) {
        ac97_controller_stop(controller);
        return -1;
    }
    return 0;
}

int ac97_controller_stop(struct ac97_controller *controller)
{
    if (!controller || !controller->bus)
        return -1;
    unsigned base = controller->master_port + controller->engine_offset;
    if (controller->running)
        ac97_controller_position(controller);
    controller->bus->write8(base + AC97_CONTROL, 0);
    controller->running = 0;
    if (wait_halted(controller, controller->engine_offset))
        return -1;
    controller->bus->write16(base + AC97_STATUS, AC97_STATUS_ACK);
    return controller->poisoned ? -1 : 0;
}

int ac97_controller_interrupt(struct ac97_controller *controller)
{
    if (!controller || !controller->running || controller->poisoned)
        return 0;
    const struct ac97_bus *bus = controller->bus;
    unsigned base = controller->master_port + controller->engine_offset;
    unsigned status = bus->read16(base + AC97_STATUS);
    if (!(status & AC97_STATUS_ACK))
        return 0;
    if (status & AC97_FIFO_ERROR) {
        ac97_controller_stop(controller);
        return -1;
    }
    /* Acknowledge before sampling CIV. A completion racing this read is
     * counted by CIV and can leave a later harmless, zero-progress IRQ. */
    bus->write16(base + AC97_STATUS, status & AC97_STATUS_ACK);
    unsigned current = bus->read8(base + AC97_CURRENT_INDEX) & 31;
    unsigned completed = (current - controller->next_completed) & 31;
    int terminal = (status & (AC97_HALTED | AC97_LAST_COMPLETE)) ==
                   (AC97_HALTED | AC97_LAST_COMPLETE);
    if (terminal)
        completed++;
    controller->completed_frames += (uint64_t)completed * AC97_PERIOD_FRAMES;
    controller->next_completed = (controller->next_completed + completed) & 31;
    controller->last_valid = (controller->last_valid + completed) & 31;
    if (completed) {
        controller->terminal_awaiting_fetch = terminal;
        bus->publish();
        /* Extending LVI resumes a halted RUN engine. Rewriting RUN instead
         * advances QEMU's prefetch index and skips a descriptor. */
        bus->write8(base + AC97_LAST_INDEX, controller->last_valid);
    }
    return (int)completed;
}

uint64_t ac97_controller_position(struct ac97_controller *controller)
{
    if (!controller || !controller->running || controller->poisoned)
        return controller ? controller->last_position : 0;
    const struct ac97_bus *bus = controller->bus;
    unsigned base = controller->master_port + controller->engine_offset;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        unsigned current = bus->read8(base + AC97_CURRENT_INDEX) & 31;
        unsigned remaining = bus->read16(base + AC97_REMAINING_SAMPLES);
        if (current != (bus->read8(base + AC97_CURRENT_INDEX) & 31) ||
            remaining > AC97_PERIOD_BYTES / 2)
            continue;
        /* LVI restart can be posted: CIV may still name the terminal entry
         * already included in completed_frames. Counting its modular distance
         * again would add an entire ring and permanently inflate position. */
        if (controller->terminal_awaiting_fetch &&
            current == ((controller->next_completed - 1) & 31)) {
            if (controller->completed_frames > controller->last_position)
                controller->last_position = controller->completed_frames;
            break;
        }
        controller->terminal_awaiting_fetch = 0;
        uint64_t position = controller->completed_frames +
            (uint64_t)((current - controller->next_completed) & 31) *
            AC97_PERIOD_FRAMES + (AC97_PERIOD_BYTES / 2 - remaining) / 2;
        if (position > controller->last_position)
            controller->last_position = position;
        break;
    }
    return controller->last_position;
}
