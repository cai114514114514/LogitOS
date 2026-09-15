#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "es1370.h"
#include "driver.h"
#include "dma.h"
#include "snd.h"
#include "pci.h"
#include "kprintf.h"
#include "kheap.h"
#include "ktime.h"

static unsigned checks, failures;
#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        printf("FAIL line %u: %s\n", (unsigned)__LINE__, #condition); \
    } \
} while (0)

struct allocation { void *memory; unsigned freed; };
static struct allocation allocations[32];
static unsigned allocation_count, free_count;
static struct device device;
static struct snd_device *sound;
static struct snd_capdevice *capture_device;
static irq_handler_t irq_handler;
static void *irq_argument;
static struct dma_device *dma_device;
static unsigned callbacks, init_calls, port_writes, publish_count;
static unsigned capture_callbacks, input_barriers;
static unsigned dma_allocations, dma_frees, barriers, quarantines;
static uint64_t clock_ns, next_token;
static uint32_t control, serial, status, page, frame_address[2], frame_size[2];
static uint32_t sample_count, pci_command, pci_bar, pci_identity;
static uint32_t adc_address, adc_size, adc_count;
static uint8_t codec[32];
static unsigned irq_releases, enable_calls;
static unsigned fail_dma, fail_irq, fail_registration, fail_enable;
static unsigned stuck_stop, fail_irq_release, busy_codec, wrong_ring_readback;
static unsigned high_dma;
static unsigned fail_capture_registration;
static uint64_t input_frames;
static unsigned next_capture_period;
static uint8_t delivered_input[32768 * 8];
static size_t delivered_input_bytes;
static unsigned stuck_adc_stop;
static unsigned fail_submit, ignored_adc_enable;
static const uint8_t input_pattern[16] = {
    0x00, 0x80, 0xff, 0x7f, 0x34, 0x12, 0xba, 0xdc,
    0x01, 0x00, 0xff, 0xff, 0x68, 0x24, 0x57, 0x13
};
static uint8_t captured[32768 * 4];
static size_t captured_bytes;
static unsigned next_slot;

static void *allocate(size_t bytes)
{
    CHECK(allocation_count < 32);
    void *memory = calloc(1, bytes);
    CHECK(memory != NULL);
    allocations[allocation_count++] = (struct allocation){memory, 0};
    return memory;
}

static void release(void *memory)
{
    if (!memory) {
        return;
    }
    for (unsigned i = 0; i < allocation_count; ++i) {
        if (allocations[i].memory == memory && !allocations[i].freed) {
            allocations[i].freed = 1;
            ++free_count;
            free(memory);
            return;
        }
    }
    CHECK(0); /* Double-free or an allocation absent from the model. */
}

static void destroy_fixture(void)
{
    /* A test fixture may destroy its entire fake machine, including buffers
     * production deliberately quarantines. This is not a driver reset API. */
    irq_handler = NULL;
    irq_argument = NULL;
    sound = NULL;
    capture_device = NULL;
    for (unsigned i = 0; i < allocation_count; ++i) {
        if (!allocations[i].freed) {
            free(allocations[i].memory);
        }
    }
    allocation_count = 0;
    free_count = 0;
}

static void reset(void)
{
    destroy_fixture();
    device = (struct device){.bus_type = DEV_BUS_PCI, .vendor = 0x1274,
        .device = 0x5000, .class_code = 4, .subclass = 1,
        .slot = (uint8_t)next_slot++, .irq_vec = -1};
    device.res[0] = (struct dev_resource){.start = 0xc000, .size = 256,
        .flags = DEV_RES_IO};
    callbacks = init_calls = port_writes = publish_count = 0;
    capture_callbacks = input_barriers = 0;
    dma_allocations = dma_frees = barriers = quarantines = 0;
    irq_releases = enable_calls = 0;
    fail_dma = fail_irq = fail_registration = fail_enable = 0;
    fail_capture_registration = 0;
    stuck_stop = fail_irq_release = busy_codec = wrong_ring_readback = high_dma = 0;
    control = serial = status = page = sample_count = 0;
    adc_address = adc_size = adc_count = 0;
    input_frames = 0;
    next_capture_period = 0;
    delivered_input_bytes = 0;
    stuck_adc_stop = 0;
    fail_submit = ignored_adc_enable = 0;
    frame_address[0] = frame_address[1] = frame_size[0] = frame_size[1] = 0;
    pci_command = 0;
    pci_identity = 0x50001274;
    pci_bar = 0xc001;
    memset(codec, 0xff, sizeof codec);
    clock_ns = 1000000000;
    next_token = 0;
    dma_device = NULL;
    captured_bytes = 0;
}

void *kmalloc(size_t bytes) { return allocate(bytes); }
void kfree(void *memory) { release(memory); }
void kprintf(const char *format, ...) { (void)format; }
uint64_t time_mono_ns(void) { clock_ns += 1000; return clock_ns; }

uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset)
{
    CHECK(bus == device.bus && slot == device.slot && function == device.func);
    if (offset == 0) {
        return pci_identity;
    }
    if (offset == 0x10) {
        return pci_bar;
    }
    CHECK(0);
    return UINT32_MAX;
}

uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset)
{
    CHECK(bus == device.bus && slot == device.slot && function == device.func && offset == 4);
    return (uint16_t)pci_command;
}

void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset,
                    uint16_t value)
{
    CHECK(bus == device.bus && slot == device.slot && function == device.func && offset == 4);
    pci_command = value;
}

int dev_enable_checked(struct device *target, int bus_master)
{
    CHECK(target == &device);
    ++enable_calls;
    pci_command |= 1;
    if (bus_master) {
        ++publish_count;
        CHECK(barriers > 0 && dma_device && dma_device->mask == 0xffffffff);
        pci_command |= 4;
        return fail_enable ? -1 : 0;
    }
    pci_command &= ~4u;
    return 0;
}

int dev_disable_checked(struct device *target)
{
    CHECK(target == &device);
    pci_command &= ~7u;
    return 0;
}

int dev_irq_request(struct device *target, irq_handler_t handler, void *argument,
                    const char *name)
{
    CHECK(target == &device && !strcmp(name, "es1370"));
    if (fail_irq) {
        return -1;
    }
    CHECK(!(serial & 0x200) && !(control & 0x70));
    irq_handler = handler;
    irq_argument = argument;
    target->irq_mode = DEV_IRQ_INTX;
    target->irq_vec = 0x60;
    return 0x60;
}

int dev_irq_release(struct device *target)
{
    CHECK(target == &device);
    ++irq_releases;
    if (fail_irq_release) {
        return -1;
    }
    CHECK(!(serial & 0x200));
    irq_handler = NULL;
    irq_argument = NULL;
    target->irq_mode = DEV_IRQ_NONE;
    target->irq_vec = -1;
    return 0;
}

int snd_present(void) { return sound != NULL; }
int snd_register_device(struct snd_device *candidate)
{
    if (fail_registration || sound) {
        return -1;
    }
    sound = candidate;
    return 0;
}
void snd_unregister_device(struct snd_device *candidate)
{
    CHECK(candidate == sound);
    sound = NULL;
}
void snd_init(void) { ++init_calls; }
int snd_capture_present(void) { return capture_device != NULL; }
int snd_register_capture_device(struct snd_capdevice *candidate)
{
    if (capture_device || fail_capture_registration) {
        return -1;
    }
    capture_device = candidate;
    return 0;
}
void snd_unregister_capture_device(struct snd_capdevice *candidate)
{
    CHECK(candidate == capture_device);
    capture_device = NULL;
}
void snd_capture_period_elapsed(struct snd_capdevice *candidate)
{
    CHECK(candidate == capture_device && input_barriers > 0);
    if (delivered_input_bytes + 4096 <= sizeof delivered_input) {
        memcpy(delivered_input + delivered_input_bytes,
               candidate->ring + next_capture_period * 4096, 4096);
        delivered_input_bytes += 4096;
    }
    next_capture_period = (next_capture_period + 1u) % 8u;
    ++capture_callbacks;
}
void snd_period_elapsed(struct snd_device *candidate)
{
    CHECK(candidate == sound && sound != NULL);
    ++callbacks;
}

void dma_device_init(struct dma_device *owner, const char *name, uint64_t mask)
{
    memset(owner, 0, sizeof *owner);
    owner->name = name;
    owner->mask = mask;
    dma_device = owner;
}

struct dma_buffer *dma_alloc_coherent(struct dma_device *owner, size_t bytes,
                                      size_t alignment, size_t boundary)
{
    ++dma_allocations;
    CHECK(owner->mask == 0xffffffff && alignment == 4096 && boundary == 0);
    if (fail_dma == dma_allocations) {
        return NULL;
    }
    struct dma_buffer *buffer = allocate(sizeof *buffer);
    buffer->cpu = allocate(bytes);
    buffer->size = bytes;
    buffer->dev = owner;
    buffer->dma.value = UINT64_C(0x11000000) + dma_allocations * 0x10000;
    if (high_dma) {
        buffer->dma.value += UINT64_C(0x100000000);
    }
    buffer->next = owner->buffers;
    owner->buffers = buffer;
    return buffer;
}

int dma_free_coherent(struct dma_buffer *buffer)
{
    CHECK(buffer->state != DMA_DEVICE_OWNED && buffer->state != DMA_QUARANTINED);
    CHECK(!(pci_command & 4) && !irq_handler);
    struct dma_buffer **link = &buffer->dev->buffers;
    while (*link != buffer) {
        CHECK(*link != NULL);
        link = &(*link)->next;
    }
    *link = buffer->next;
    release(buffer->cpu);
    release(buffer);
    ++dma_frees;
    return 0;
}

uint64_t dma_buffer_submit(struct dma_buffer *buffer)
{
    CHECK(buffer->state != DMA_DEVICE_OWNED && !buffer->dev->blocked);
    if (fail_submit) {
        return 0;
    }
    buffer->state = DMA_DEVICE_OWNED;
    buffer->token = ++next_token;
    return buffer->token;
}

int dma_buffer_complete(struct dma_buffer *buffer, uint64_t token)
{
    CHECK(buffer->state == DMA_DEVICE_OWNED && token && token == buffer->token);
    if (buffer->dma.value == 0x11010000) {
        CHECK(!(control & 0x20));
    } else if (buffer->dma.value == 0x11030000) {
        CHECK(!(control & 0x10));
    } else {
        CHECK(!(control & 0x70) && !(pci_command & 4));
    }
    buffer->state = DMA_COMPLETED;
    return 0;
}

void dma_wmb(void) { ++barriers; }
void dma_rmb(void) { ++input_barriers; }
void dma_device_quarantine(struct dma_device *owner)
{
    ++quarantines;
    owner->blocked = 1;
    for (struct dma_buffer *buffer = owner->buffers; buffer; buffer = buffer->next) {
        if (buffer->state == DMA_DEVICE_OWNED) {
            buffer->state = DMA_QUARANTINED;
        }
    }
}

void dma_device_quiesced(struct dma_device *owner)
{
    CHECK(!(control & 0x70) && !(pci_command & 4) && !irq_handler);
    owner->blocked = 1;
    for (struct dma_buffer *buffer = owner->buffers; buffer; buffer = buffer->next) {
        buffer->state = DMA_QUIESCED;
    }
}

uint32_t inl(uint16_t port)
{
    CHECK((pci_command & 1) && port >= 0xc000 && port < 0xc040);
    switch (port - 0xc000) {
    case 0x00: return control;
    case 0x04: return status | (busy_codec ? 0x400u : 0);
    case 0x0c: return page;
    case 0x20: return serial;
    case 0x28: return sample_count;
    case 0x2c: return adc_count;
    case 0x30: CHECK(page == 0xd); return adc_address;
    case 0x34: CHECK(page == 0xd); return adc_size;
    case 0x38: return frame_address[page == 0xd];
    case 0x3c: return frame_size[page == 0xd] ^ (wrong_ring_readback ? 1u : 0);
    default: CHECK(0); return UINT32_MAX;
    }
}

void outl(uint16_t port, uint32_t value)
{
    ++port_writes;
    CHECK((pci_command & 1) && port >= 0xc000 && port < 0xc040);
    switch (port - 0xc000) {
    case 0x00:
        if (ignored_adc_enable) {
            value &= ~0x10u;
        }
        if (!stuck_stop || (value & 0x20)) {
            if (stuck_adc_stop && (control & 0x10)) {
                value |= 0x10;
            }
            control = value;
        }
        break;
    case 0x0c:
        page = value;
        CHECK(page == 0xc || page == 0xd);
        break;
    case 0x20:
        serial = value;
        if (!(serial & 0x200)) {
            status &= ~2u;
        }
        if (!(serial & 0x400)) {
            status &= ~1u;
        }
        if (!(status & 7u)) {
            status &= ~0x80000000u;
        }
        break;
    case 0x28:
        sample_count = (value & 0xffff) | (value << 16);
        break;
    case 0x2c:
        adc_count = (value & 0xffff) | (value << 16);
        break;
    case 0x30:
        CHECK(page == 0xd && !(control & 0x10));
        adc_address = value;
        break;
    case 0x34:
        CHECK(page == 0xd && !(control & 0x10));
        adc_size = value;
        break;
    case 0x38:
        CHECK(!(control & (page == 0xd ? 0x70 : 0x20)));
        frame_address[page == 0xd] = value;
        break;
    case 0x3c:
        CHECK(!(control & (page == 0xd ? 0x70 : 0x20)));
        frame_size[page == 0xd] = value;
        break;
    default:
        CHECK(0);
        break;
    }
}

void outw(uint16_t port, uint16_t value)
{
    ++port_writes;
    CHECK((pci_command & 1) && port == 0xc010 && !(status & 0x400));
    CHECK((value >> 8) < 32);
    codec[value >> 8] = (uint8_t)value;
}

static void advance_frames(unsigned frames, int interrupt)
{
    CHECK((pci_command & 5) == 5 && (control & 0x20));
    CHECK((serial & 0x0010020c) == 0x0010020c && (frame_size[0] & 0xffff) == 8191);
    CHECK((sample_count & 0xffff) == 1023 && frame_address[1] == 0x11020000);
    struct dma_buffer *ring = dma_device->buffers;
    while (ring && ring->dma.value != frame_address[0]) {
        ring = ring->next;
    }
    CHECK(ring && ring->state == DMA_DEVICE_OWNED);
    unsigned current_frame = frame_size[0] >> 16;
    for (unsigned frame = 0; frame < frames; ++frame) {
        if (captured_bytes + 4 <= sizeof captured) {
            memcpy(captured + captured_bytes, (uint8_t *)ring->cpu + current_frame * 4, 4);
            captured_bytes += 4;
        }
        current_frame = (current_frame + 1u) % 8192u;
    }
    frame_size[0] = 8191u | current_frame << 16;
    clock_ns += (uint64_t)frames * 1000000000u / 48662u;
    if (interrupt) {
        status = 0x80000002;
        irq_handler(irq_argument);
    }
}

static void playback_case(void)
{
    reset();
    CHECK(es1370_probe(&device) == 0 && sound && init_calls == 1);
    CHECK(sound->rate == 48662 && sound->channels == 2 && sound->format == SND_FMT_S16);
    CHECK(sound->period_bytes == 4096 && sound->periods == 8 && sound->irq_mode == DEV_IRQ_INTX);
    CHECK(frame_address[0] == 0x11010000 && frame_address[1] == 0x11020000);
    CHECK(codec[0] == 0 && codec[1] == 0 && codec[2] == 6 && codec[3] == 6);
    CHECK(codec[0x10] == 0 && codec[0x11] == 12 && codec[0x16] == 3 && codec[0x17] == 0);
    CHECK(!(pci_command & 4) && !(control & 0x70));
    for (unsigned i = 0; i < 32768; ++i) {
        sound->ring[i] = (uint8_t)(i * 73u + 29u);
    }
    CHECK(sound->start(sound) == 0);
    CHECK(control == 0x001b0022 && serial == 0x0010023c);
    advance_frames(1024, 1);
    CHECK(callbacks == 1 && sound->position(sound) == 1024);
    advance_frames(2048, 1);
    CHECK(callbacks == 3 && sound->position(sound) == 3072);
    for (unsigned period = 0; period < 21; ++period) {
        advance_frames(1024, 1);
    }
    CHECK(callbacks == 24 && sound->position(sound) == 24576);
    for (size_t i = 0; i < captured_bytes; ++i) {
        CHECK(captured[i] == (uint8_t)((i % 32768u) * 73u + 29u));
    }
    unsigned before = callbacks;
    status = 0x80000004; /* A shared vector, not DAC2 completion. */
    irq_handler(irq_argument);
    CHECK(callbacks == before);
    sound->stop(sound);
    CHECK(!(control & 0x70) && !(pci_command & 4));
    status = 0x80000002;
    irq_handler(irq_argument);
    CHECK(callbacks == before && !(serial & 0x200));
    CHECK(sound->start(sound) == 0 && sound->position(sound) == 0);
    advance_frames(1024, 1);
    CHECK(callbacks == before + 1);
    es1370_remove(&device);
    CHECK(!sound && !capture_device && !irq_handler && !device.drvdata && dma_frees == 3);
    CHECK(free_count == allocation_count);
}

static void preflight_cases(void)
{
    reset();
    device.device = 0x1371;
    CHECK(es1370_probe(&device) < 0 && allocation_count == 0 && port_writes == 0);
    reset();
    pci_identity = 0x13711274;
    CHECK(es1370_probe(&device) < 0 && allocation_count == 0 && port_writes == 0);
    reset();
    pci_bar = 0xd001;
    CHECK(es1370_probe(&device) < 0 && allocation_count == 0 && port_writes == 0);
    reset();
    device.res[0].size = 32;
    CHECK(es1370_probe(&device) < 0 && allocation_count == 0 && port_writes == 0);
    reset();
    device.res[0].start = 0xff80;
    pci_bar = 0xff81;
    CHECK(es1370_probe(&device) < 0 && allocation_count == 0 && port_writes == 0);
}

static void advance_input(unsigned frames, int interrupt)
{
    CHECK((pci_command & 5) == 5 && (control & 0x10));
    CHECK((serial & 0x430) == 0x430 && (adc_count & 0xffff) == 1023);
    CHECK((adc_size & 0xffff) == 8191 && adc_address == 0x11030000);
    struct dma_buffer *ring = dma_device->buffers;
    while (ring && ring->dma.value != adc_address) {
        ring = ring->next;
    }
    CHECK(ring && ring->state == DMA_DEVICE_OWNED);
    CHECK(ring->cpu == capture_device->ring);
    if (sound) {
        CHECK(ring->cpu != sound->ring);
    }
    unsigned current_frame = adc_size >> 16;
    for (unsigned frame = 0; frame < frames; ++frame) {
        memcpy((uint8_t *)ring->cpu + current_frame * 4,
               input_pattern + (input_frames % 4u) * 4u, 4);
        ++input_frames;
        current_frame = (current_frame + 1u) % 8192u;
    }
    adc_size = 8191u | current_frame << 16;
    clock_ns += (uint64_t)frames * 1000000000u / 48662u;
    if (interrupt) {
        status |= 0x80000001;
        irq_handler(irq_argument);
    }
}

static void capture_case(void)
{
    reset();
    CHECK(es1370_probe(&device) == 0 && capture_device != NULL);
    CHECK(capture_device->rate == 48662 && capture_device->channels == 2 &&
          capture_device->format == SND_FMT_S16 && capture_device->periods == 8);
    CHECK(capture_device->ring != sound->ring && adc_address == 0x11030000);
    CHECK(codec[0x0e] == 6 && codec[0x12] == 1 && codec[0x13] == 1 && codec[0x19] == 0);
    CHECK(codec[0x10] == 0 && codec[0x11] == 12); /* No microphone sidetone. */
    CHECK(capture_device->start(capture_device) == 0);
    CHECK(control == 0x001b0012 && (serial & 0x430) == 0x430);
    advance_input(1024, 1);
    advance_input(2048, 1);
    CHECK(capture_callbacks == 3);
    for (unsigned period = 0; period < 21; ++period) {
        advance_input(1024, 1);
    }
    CHECK(capture_callbacks == 24 && delivered_input_bytes == 24u * 4096u);
    for (size_t i = 0; i < delivered_input_bytes; ++i) {
        CHECK(delivered_input[i] == input_pattern[i % sizeof input_pattern]);
    }
    capture_device->stop(capture_device);
    CHECK(!(control & 0x70) && !(pci_command & 4));
    unsigned before = capture_callbacks;
    status = 0x80000001;
    irq_handler(irq_argument);
    CHECK(capture_callbacks == before && !(serial & 0x400));
    memset(capture_device->ring, 0xcc, 32768);
    next_capture_period = 0;
    CHECK(capture_device->start(capture_device) == 0 && (adc_size >> 16) == 0);
    advance_input(1024, 1);
    CHECK(capture_callbacks == before + 1);
    es1370_remove(&device);
    CHECK(!sound && !capture_device && dma_frees == 3 && free_count == allocation_count);
}

static void duplex_case(void)
{
    reset();
    CHECK(es1370_probe(&device) == 0);
    CHECK(sound->start(sound) == 0);
    advance_frames(1024, 1);
    unsigned old_playback_size = frame_size[0];
    unsigned old_publish_count = publish_count;
    CHECK(capture_device->start(capture_device) == 0);
    CHECK(frame_size[0] == old_playback_size && publish_count == old_publish_count);
    CHECK(control == 0x001b0032 && serial == 0x0010063c);
    advance_frames(1024, 0);
    advance_input(1024, 0);
    status = 0x80000003;
    irq_handler(irq_argument);
    CHECK(callbacks == 2);
    unsigned old_capture_callbacks = capture_callbacks;
    sound->stop(sound);
    CHECK((control & 0x70) == 0x10 && (pci_command & 4) && (serial & 0x400));
    advance_input(1024, 1);
    CHECK(capture_callbacks == old_capture_callbacks + 1);
    unsigned old_adc_size = adc_size;
    CHECK(sound->start(sound) == 0 && adc_size == old_adc_size);
    capture_device->stop(capture_device);
    CHECK((control & 0x70) == 0x20 && (pci_command & 4) && (serial & 0x200));
    advance_frames(1024, 1);
    CHECK(callbacks == 3);
    es1370_remove(&device);
    CHECK(!sound && !capture_device && dma_frees == 3 && free_count == allocation_count);
}

static void capture_failure_cases(void)
{
    reset();
    fail_capture_registration = 1;
    CHECK(es1370_probe(&device) == 0 && sound && !capture_device);
    CHECK(sound->start(sound) == 0);
    es1370_remove(&device);
    CHECK(free_count == allocation_count);

    reset();
    fail_dma = 3;
    CHECK(es1370_probe(&device) == 0 && sound && !capture_device);
    CHECK(sound->start(sound) == 0);
    es1370_remove(&device);
    CHECK(dma_frees == 2 && free_count == allocation_count);

    reset();
    struct snd_device other_sound = {.name = "other-playback"};
    sound = &other_sound;
    CHECK(es1370_probe(&device) == 0 && capture_device != NULL && init_calls == 0);
    CHECK(capture_device->start(capture_device) == 0);
    advance_input(1024, 1);
    es1370_remove(&device);
    CHECK(sound == &other_sound && !capture_device && free_count == allocation_count);

    for (unsigned which = 0; which < 3; ++which) {
        reset();
        CHECK(es1370_probe(&device) == 0);
        CHECK(sound->start(sound) == 0 && capture_device->start(capture_device) == 0);
        if (which == 0) {
            stuck_adc_stop = 1;
            capture_device->stop(capture_device);
        } else if (which == 1) {
            advance_input(8192, 1);
        } else {
            adc_size = 8191u | 9000u << 16;
            status = 0x80000001;
            irq_handler(irq_argument);
        }
        CHECK(quarantines > 0 && !(pci_command & 4) && (pci_command & 0x400));
        CHECK(sound->start(sound) < 0 && capture_device->start(capture_device) < 0);
        es1370_remove(&device);
        CHECK(!sound && !capture_device && dma_frees == 0);
        unsigned before = allocation_count;
        CHECK(es1370_probe(&device) < 0 && allocation_count == before);
    }
}

static void duplex_start_failure_cases(void)
{
    for (unsigned which = 0; which < 2; ++which) {
        reset();
        CHECK(es1370_probe(&device) == 0 && sound->start(sound) == 0);
        if (which == 0) {
            fail_submit = 1;
        } else {
            ignored_adc_enable = 1;
        }
        CHECK(capture_device->start(capture_device) < 0);
        /* No IRQ is delivered here: the failed second start must immediately
         * stop the first engine and retain its unresolved DMA allocations. */
        CHECK(quarantines > 0 && !(control & 0x70) && !(pci_command & 4));
        CHECK((pci_command & 0x400) && !(serial & 0x600));
        CHECK(sound->start(sound) < 0 && capture_device->start(capture_device) < 0);
        es1370_remove(&device);
        CHECK(!sound && !capture_device && dma_frees == 0);
        unsigned before = allocation_count;
        CHECK(es1370_probe(&device) < 0 && allocation_count == before);
    }
}

static void ordinary_failure_cases(void)
{
    for (unsigned which = 0; which < 8; ++which) {
        reset();
        switch (which) {
        case 0: fail_dma = 1; break;
        case 1: fail_dma = 2; break;
        case 2: fail_irq = 1; break;
        case 3: fail_registration = fail_capture_registration = 1; break;
        case 4: busy_codec = 1; break;
        case 5: wrong_ring_readback = 1; break;
        case 6: high_dma = 1; break;
        default: break;
        }
        if (which == 7) {
            CHECK(es1370_probe(&device) == 0);
            fail_enable = 1;
            CHECK(sound->start(sound) < 0);
            es1370_remove(&device);
        } else {
            CHECK(es1370_probe(&device) < 0);
        }
        CHECK(!sound && !irq_handler && !(pci_command & 4));
        CHECK(free_count == allocation_count);
    }
}

static void quarantine_cases(void)
{
    for (unsigned which = 0; which < 5; ++which) {
        reset();
        CHECK(es1370_probe(&device) == 0 && sound->start(sound) == 0);
        if (which == 0) {
            stuck_stop = 1;
            sound->stop(sound);
        } else if (which == 1) {
            advance_frames(8192, 1); /* Exact lap: ambiguous current position. */
        } else if (which == 2) {
            clock_ns += 200000000;
            advance_frames(1024, 1); /* More than one lap may have passed. */
        } else if (which == 3) {
            fail_irq_release = 1;
        } else {
            status = 0x80000002; /* Same position without a whole-lap delay. */
            irq_handler(irq_argument);
        }
        es1370_remove(&device);
        CHECK(quarantines > 0 && dma_frees == 0 && free_count < allocation_count);
        CHECK(!sound && (pci_command & 0x400) && !(pci_command & 4));
        unsigned previous_allocations = allocation_count;
        unsigned previous_enable_calls = enable_calls;
        device.drvdata = NULL; /* Mirrors core's failed probe/unbind clearing. */
        CHECK(es1370_probe(&device) < 0);
        struct device replacement = device;
        CHECK(es1370_probe(&replacement) < 0);
        CHECK(allocation_count == previous_allocations && enable_calls == previous_enable_calls);
        if (irq_handler) {
            unsigned before = callbacks;
            status = 0x80000002;
            irq_handler(irq_argument);
            CHECK(callbacks == before && !(serial & 0x200));
        }
    }
    reset();
    fail_registration = 1;
    fail_capture_registration = 1;
    fail_irq_release = 1;
    CHECK(es1370_probe(&device) < 0 && irq_handler != NULL);
    device.drvdata = NULL;
    unsigned before = allocation_count;
    unsigned enabled_before = enable_calls;
    CHECK(es1370_probe(&device) < 0);
    CHECK(allocation_count == before && enable_calls == enabled_before);
    CHECK((pci_command & 0x400) && !(pci_command & 4) && dma_frees == 0);
}

int main(void)
{
    playback_case();
    preflight_cases();
    ordinary_failure_cases();
    quarantine_cases();
    capture_case();
    duplex_case();
    capture_failure_cases();
    duplex_start_failure_cases();
    destroy_fixture();
    printf("ES1370_HOST: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
