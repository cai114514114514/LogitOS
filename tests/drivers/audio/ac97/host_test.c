/* Literal ICH register oracle, independent of the driver's register enum. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ac97.h"
#include "c/drivers/core/driver.h"
#include "c/drivers/core/dma.h"
#include "c/kernel/audio/snd.h"

static unsigned checks, failures;
#define CHECK(expression) do { checks++; if (!(expression)) { failures++; \
    printf("FAIL %u: %s\n", __LINE__, #expression); } } while (0)
struct engine { unsigned control, status, current, last, remaining; uint32_t base; };
static struct engine engines[3];
static uint16_t codec[128], command;
static unsigned delay_count, accesses, notifications, init_calls, allocations, frees;
static int ready, stuck_reset, stuck_stop, delayed_fetch, semaphore_busy;
static int fail_mask_once, fail_disable_once, reject_channel_write;
static int fail_irq, fail_release, fail_complete, fail_free, fail_enable, fail_alloc;
static uint32_t identity, bars[2], global_control;
static struct snd_device *sound;
static void (*interrupt_callback)(void *);
static void *interrupt_argument;
static struct dma_buffer *buffers[4];
static struct snd_capdevice *capture;
static unsigned capture_notifications, input_periods_written, input_sync;
static int verify_input, fail_capture_register, fail_input_route;
static unsigned capture_register_calls;


static struct engine *engine_port(uint16_t port, unsigned *offset)
{
    CHECK(command & 1);
    accesses++;
    CHECK(port >= 0x2000 && port < 0x2030);
    *offset = (port - 0x2000) & 15;
    return &engines[(port - 0x2000) / 16];
}
static uint8_t read8(uint16_t port)
{
    if (port == 0x2034) {
        accesses++;
        CHECK(command & 1);
        return semaphore_busy;
    }
    unsigned offset;
    struct engine *engine = engine_port(port, &offset);
    if (offset == 4) return engine->current;
    if (offset == 11) return engine->control;
    CHECK(0);
    return 0;
}
static uint16_t read16(uint16_t port)
{
    if (port >= 0x1000 && port < 0x1100) {
        CHECK(command & 1);
        accesses++;
        return codec[(port - 0x1000) / 2];
    }
    unsigned offset;
    struct engine *engine = engine_port(port, &offset);
    if (offset == 6) return engine->status;
    if (offset == 8) return engine->remaining;
    CHECK(0);
    return 0;
}
static uint32_t read32(uint16_t port)
{
    CHECK(command & 1);
    accesses++;
    if (port == 0x202c) return global_control;
    if (port == 0x2030) return ready ? 0x100 : 0;
    CHECK(0);
    return 0;
}
static void fetch_next(struct engine *engine)
{
    engine->current = (engine->current + 1) & 31;
    engine->remaining = 2048;
    engine->status &= ~3u;
}
static void write8(uint16_t port, uint8_t value)
{
    unsigned offset;
    struct engine *engine = engine_port(port, &offset);
    if (offset == 5) {
        if ((engine->control & 1) && (engine->status & 1) && !delayed_fetch)
            fetch_next(engine);
        engine->last = value & 31;
        return;
    }
    CHECK(offset == 11);
    if (value & 2) {
        CHECK(!(engine->control & 1));
        *engine = (struct engine){.status = 1, .control = stuck_reset ? 2 : 0};
    } else if (value & 1) {
        CHECK(command & 4);
        engine->control = value;
        engine->status = 0;
        engine->remaining = 2048;
    } else if (!stuck_stop) {
        engine->control = value;
        engine->status |= 1;
    }
}
static void write16(uint16_t port, uint16_t value)
{
    if (port >= 0x1000 && port < 0x1100) {
        CHECK(command & 1);
        accesses++;
        if (port == 0x1000) return;
        if (port == 0x1026) value |= 15;
        if (port == 0x102a && !(value & 1)) {
            codec[0x2c / 2] = 48000;
            codec[0x32 / 2] = 48000;
        }
        if (port == 0x101a && fail_input_route) return;
        codec[(port - 0x1000) / 2] = value;
        return;
    }
    unsigned offset;
    struct engine *engine = engine_port(port, &offset);
    CHECK(offset == 6 && !(value & ~0x1c));
    engine->status &= ~value;
}
static void write32(uint16_t port, uint32_t value)
{
    CHECK(command & 1);
    accesses++;
    if (port == 0x202c) {
        if (reject_channel_write)
            value = (value & ~0x00300000u) | (global_control & 0x00300000u);
        global_control = value & ~4u;
        return;
    }
    if (port == 0x2030) {
        CHECK(value == 0x8000);
        return;
    }
    unsigned offset;
    struct engine *engine = engine_port(port, &offset);
    CHECK(offset == 0 && !(value & 3));
    engine->base = value;
}
static void delay_us(unsigned duration) { CHECK(duration == 100); delay_count++; }
void dma_wmb(void) { }
void dma_rmb(void) { input_sync = input_periods_written; }
const struct ac97_bus ac97_native_bus = {
    read8, read16, read32, write8, write16, write32, delay_us, dma_wmb
};
uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset)
{
    (void)bus; (void)slot; (void)function;
    if (offset == 0) return identity;
    if (offset == 0x10) return bars[0];
    if (offset == 0x14) return bars[1];
    CHECK(0);
    return 0;
}
uint16_t pci_cfg_read16(uint8_t bus, uint8_t slot, uint8_t function, uint16_t offset)
{
    (void)bus; (void)slot; (void)function;
    CHECK(offset == 4);
    return command;
}
void pci_cfg_write16(uint8_t bus, uint8_t slot, uint8_t function,
                     uint16_t offset, uint16_t value)
{
    (void)bus; (void)slot; (void)function;
    CHECK(offset == 4);
    if (fail_mask_once) {
        fail_mask_once = 0;
        return;
    }
    command = value;
}
int dev_enable_checked(struct device *device, int master)
{
    (void)device;
    if (fail_enable) return -1;
    command = 1 | (master ? 4 : 0);
    return 0;
}
int dev_disable_checked(struct device *device)
{
    (void)device;
    if (fail_disable_once) {
        fail_disable_once = 0;
        return -1;
    }
    command &= ~7u;
    return 0;
}
int dev_irq_request(struct device *device, void (*callback)(void *),
                     void *argument, const char *name)
{
    CHECK(!strcmp(name, "ac97"));
    if (fail_irq) return -1;
    device->irq_mode = 1;
    interrupt_callback = callback;
    interrupt_argument = argument;
    return 48;
}
int dev_irq_release(struct device *device)
{
    (void)device;
    if (fail_release) return -1;
    interrupt_callback = NULL;
    return 0;
}
void dma_device_init(struct dma_device *device, const char *name, uint64_t mask)
{
    *device = (struct dma_device){.name = name, .mask = mask};
    CHECK(mask == 0xffffffffu);
}
struct dma_buffer *dma_alloc_coherent(struct dma_device *device, size_t size,
                                      size_t alignment, size_t boundary)
{
    CHECK(alignment == 4096 && boundary == 0);
    if (fail_alloc) return NULL;
    unsigned slot = 0;
    while (slot < 4 && buffers[slot]) slot++;
    CHECK(slot < 4);
    if (slot == 4) return NULL;
    CHECK(size == (slot % 2 ? 4096 : 32768));
    struct dma_buffer *buffer = calloc(1, sizeof(*buffer));
    buffer->cpu = calloc(1, size);
    buffer->size = size;
    buffer->dma.value = 0x10000000 + slot * 0x10000;
    buffer->dev = device;
    buffers[slot] = buffer;
    allocations++;
    return buffer;
}
int dma_free_coherent(struct dma_buffer *buffer)
{
    if (fail_free) return -1;
    CHECK(buffer->state != DMA_DEVICE_OWNED && buffer->state != DMA_QUARANTINED);
    for (unsigned slot = 0; slot < 4; slot++)
        if (buffers[slot] == buffer) buffers[slot] = NULL;
    free(buffer->cpu);
    free(buffer);
    frees++;
    return 0;
}
uint64_t dma_buffer_submit(struct dma_buffer *buffer)
{
    CHECK(!buffer->dev->blocked && buffer->state != DMA_DEVICE_OWNED);
    buffer->state = DMA_DEVICE_OWNED;
    return ++buffer->token;
}
int dma_buffer_complete(struct dma_buffer *buffer, uint64_t token)
{
    if (fail_complete) return -1;
    CHECK(token && buffer->token == token && buffer->state == DMA_DEVICE_OWNED);
    buffer->state = DMA_COMPLETED;
    return 0;
}
void dma_device_quarantine(struct dma_device *device)
{
    device->blocked = 1;
    for (unsigned index = 0; index < 4; index++)
        if (buffers[index]) buffers[index]->state = DMA_QUARANTINED;
}
void dma_device_quiesced(struct dma_device *device)
{
    (void)device;
    CHECK(!(command & 4));
    for (unsigned index = 0; index < 4; index++)
        if (buffers[index]) buffers[index]->state = DMA_QUIESCED;
}
int snd_register_device(struct snd_device *device)
{
    if (sound) return -1;
    sound = device;
    return 0;
}
void snd_unregister_device(struct snd_device *device)
{
    CHECK(sound == device);
    sound = NULL;
    device->stop(device);
}
int snd_register_capture_device(struct snd_capdevice *device)
{
    capture_register_calls++;
    if (fail_capture_register || capture) return -1;
    capture = device;
    return 0;
}
void snd_unregister_capture_device(struct snd_capdevice *device)
{
    CHECK(capture == device);
    capture = NULL;
    device->stop(device);
}
static int16_t input_sample(unsigned frame, unsigned channel)
{
    /* The external ADC pattern has distinct channels and changes each period;
     * notification-only or playback-buffer aliases cannot satisfy it. */
    unsigned mixed = frame * 109u + channel * 173u + 23u;
    return (int16_t)((int)(mixed % 7001u) - 3500);
}
void snd_capture_period_elapsed(struct snd_capdevice *device)
{
    CHECK(capture == device);
    CHECK(input_sync == input_periods_written);
    if (verify_input) {
        const int16_t *samples = (const int16_t *)(device->ring +
                                  (capture_notifications % 8) * 4096);
        for (unsigned frame = 0; frame < 1024; frame++) {
            CHECK(samples[frame * 2] == input_sample(capture_notifications * 1024 + frame, 0));
            CHECK(samples[frame * 2 + 1] == input_sample(capture_notifications * 1024 + frame, 1));
        }
    }
    capture_notifications++;
}
void snd_init(void) { init_calls++; }
void snd_period_elapsed(struct snd_device *device) { CHECK(sound == device); notifications++; }
void kprintf(const char *format, ...) { (void)format; }

static struct device setup(void)
{
    memset(engines, 0, sizeof(engines));
    for (unsigned index = 0; index < 3; index++) engines[index].status = 1;
    memset(codec, 0, sizeof(codec));
    codec[0x26 / 2] = 15;
    codec[0x7c / 2] = 0x8384;
    codec[0x7e / 2] = 0x7600;
    codec[0x28 / 2] = 9;
    codec[0x2a / 2] = 9;
    codec[0x2c / 2] = 44100;
    codec[0x32 / 2] = 44100;
    codec[0x1a / 2] = 0x0404;
    codec[0x1c / 2] = 0x8808;
    identity = 0x24158086;
    bars[0] = 0x1001;
    bars[1] = 0x2001;
    command = 0;
    global_control = 0;
    ready = 1;
    delay_count = accesses = notifications = 0;
    capture_notifications = input_periods_written = input_sync = 0;
    verify_input = 0;
    struct device device = {.bus_type = 1, .vendor = 0x8086, .device = 0x2415,
                             .class_code = 4, .subclass = 1};
    device.res[0] = (struct dev_resource){0x1000, 256, 2};
    device.res[1] = (struct dev_resource){0x2000, 64, 2};
    return device;
}
static void finish_periods(unsigned count)
{
    struct engine *engine = &engines[1];
    for (unsigned period = 0; period < count; period++) {
        CHECK(engine->control == 0x1d && !(engine->status & 1));
        CHECK(buffers[0]->state == DMA_DEVICE_OWNED);
        CHECK(buffers[1]->state == DMA_DEVICE_OWNED);
        uint32_t *entry = (uint32_t *)buffers[1]->cpu + engine->current * 2;
        CHECK(entry[0] == 0x10000000u + (engine->current % 8) * 4096);
        CHECK(entry[1] == 0x80000800u);
        engine->remaining = 0;
        engine->status |= 8;
        if (engine->current == engine->last) {
            engine->status |= 7;
        } else {
            fetch_next(engine);
        }
    }
}
static void deliver_input_periods(unsigned count)
{
    struct engine *engine = &engines[0];
    for (unsigned period = 0; period < count; period++) {
        CHECK(engine->control == 0x1d && !(engine->status & 1));
        if (!(engine->control & 1) || (engine->status & 1)) return;
        CHECK(codec[0x1a / 2] == 0 && codec[0x1c / 2] == 0x0808);
        CHECK(codec[0x32 / 2] == 48000);
        CHECK(buffers[2]->state == DMA_DEVICE_OWNED);
        CHECK(buffers[3]->state == DMA_DEVICE_OWNED);
        CHECK(engine->base == buffers[3]->dma.value);
        uint32_t *entry = (uint32_t *)buffers[3]->cpu + engine->current * 2;
        CHECK(entry[0] == 0x10020000u + (engine->current % 8) * 4096);
        CHECK(entry[1] == 0x80000800u);
        unsigned offset = entry[0] - (unsigned)buffers[2]->dma.value;
        CHECK(offset <= buffers[2]->size - 4096);
        if (offset > buffers[2]->size - 4096) return;
        int16_t *destination = (int16_t *)((uint8_t *)buffers[2]->cpu + offset);
        for (unsigned frame = 0; frame < 1024; frame++) {
            destination[frame * 2] = input_sample(input_periods_written * 1024 + frame, 0);
            destination[frame * 2 + 1] = input_sample(input_periods_written * 1024 + frame, 1);
        }
        input_periods_written++;
        engine->remaining = 0;
        engine->status |= 8;
        if (engine->current == engine->last) engine->status |= 7;
        else fetch_next(engine);
    }
}
static void test_descriptor_bounds(void)
{
    struct ac97_descriptor descriptors[32];
    struct ac97_descriptor previous[32];
    memset(descriptors, 0xa5, sizeof(descriptors));
    memcpy(previous, descriptors, sizeof(previous));
    CHECK(ac97_build_descriptors(descriptors, sizeof(descriptors), 0xfffff000) < 0);
    CHECK(!memcmp(descriptors, previous, sizeof(previous)));
    CHECK(ac97_build_descriptors(descriptors, sizeof(descriptors) - 1, 0x10000000) < 0);
    CHECK(ac97_build_descriptors(descriptors, sizeof(descriptors), 0x10000001) < 0);
    CHECK(!memcmp(descriptors, previous, sizeof(previous)));
}
static void test_playback(void)
{
    struct device device = setup();
    CHECK(ac97_pci_probe(&device) == 0 && sound && init_calls == 1);
    CHECK(sound->rate == 48000 && sound->channels == 2 && sound->format == SND_FMT_S16);
    CHECK(sound->periods == 8 && sound->period_bytes == 4096 && sound->irq_mode == 1);
    CHECK(codec[0x2c / 2] == 48000 && codec[1] == 0 && codec[12] == 0);
    CHECK(sound->start(sound) == 0);
    CHECK(engines[1].base == 0x10010000 && engines[1].last == 31);
    for (unsigned period = 0; period < 70; period++) {
        finish_periods(1);
        interrupt_callback(interrupt_argument);
        CHECK(notifications == period + 1);
        CHECK(sound->position(sound) == (uint64_t)(period + 1) * 1024);
    }
    unsigned old = notifications;
    interrupt_callback(interrupt_argument);
    CHECK(notifications == old);
    sound->stop(sound);
    CHECK(buffers[0]->state == DMA_COMPLETED && buffers[1]->state == DMA_COMPLETED);
    CHECK(sound->start(sound) == 0);
    delayed_fetch = 1;
    finish_periods(32);
    interrupt_callback(interrupt_argument);
    CHECK(notifications == old + 32);
    CHECK(sound->position(sound) == 32768);
    fetch_next(&engines[1]);
    CHECK(sound->position(sound) == 32768);
    delayed_fetch = 0;
    ac97_pci_remove(&device);
    CHECK(!sound && !device.drvdata && !buffers[0] && !buffers[1]);
}
static void test_capture_duplex(void)
{
    struct device device = setup();
    CHECK(ac97_pci_probe(&device) == 0 && sound && capture);
    CHECK(capture->rate == 48000 && capture->channels == 2 && capture->format == SND_FMT_S16);
    CHECK(capture->ring != sound->ring && capture->ring == buffers[2]->cpu);
    CHECK(codec[0x0e / 2] == 0x8000);
    CHECK(capture->start(capture) == 0);
    CHECK(!(engines[1].control & 1));
    CHECK(sound->start(sound) == 0);
    verify_input = 1;
    for (unsigned period = 0; period < 40; period++) {
        deliver_input_periods(1);
        finish_periods(1);
        interrupt_callback(interrupt_argument);
        CHECK(capture_notifications == period + 1);
        CHECK(notifications == period + 1);
    }
    capture->stop(capture);
    CHECK((engines[1].control & 1) && !(engines[0].control & 1));
    CHECK(buffers[2]->state == DMA_COMPLETED && buffers[3]->state == DMA_COMPLETED);
    CHECK(buffers[0]->state == DMA_DEVICE_OWNED && buffers[1]->state == DMA_DEVICE_OWNED);
    finish_periods(1);
    interrupt_callback(interrupt_argument);
    CHECK(notifications == 41 && capture_notifications == 40);
    CHECK(capture->start(capture) == 0);
    capture_notifications = input_periods_written = 0;
    sound->stop(sound);
    CHECK((engines[0].control & 1) && !(engines[1].control & 1));
    deliver_input_periods(3);
    interrupt_callback(interrupt_argument);
    CHECK(capture_notifications == 3 && notifications == 41);
    ac97_pci_remove(&device);
    CHECK(!sound && !capture && !device.drvdata);
    for (unsigned index = 0; index < 4; index++) CHECK(!buffers[index]);
}

static void test_ich_models(void)
{
    /* Independent Intel identities; do not derive expectations from the
     * production X-macro inventory or the channel-mask profile under test. */
    const uint16_t identities[] = {0x2415, 0x2425, 0x2445, 0x2485};
    for (unsigned index = 0; index < 4; index++) {
        struct device device = setup();
        device.device = identities[index];
        identity = ((uint32_t)device.device << 16) | 0x8086;
        global_control = 0x00400012u;
        if (index >= 2) global_control |= 0x00200000u;
        CHECK(ac97_pci_probe(&device) == 0 && sound && capture);
        CHECK(global_control == 0x00400012u);
        if (!sound || !capture) continue;
        CHECK(sound->start(sound) == 0 && capture->start(capture) == 0);
        verify_input = 1;
        deliver_input_periods(2);
        finish_periods(2);
        interrupt_callback(interrupt_argument);
        CHECK(capture_notifications == 2 && notifications == 2);
        CHECK(sound->position(sound) == 2048);
        ac97_pci_remove(&device);
        CHECK(!device.drvdata && !sound && !capture && allocations == frees);
    }
    struct device device = setup();
    device.device = 0x2445;
    identity = 0x24458086;
    global_control = 0x00200002;
    reject_channel_write = 1;
    CHECK(ac97_pci_probe(&device) < 0);
    CHECK(delay_count == 1000 && !sound && !capture);
    reject_channel_write = 0;
    ac97_pci_remove(&device);
    CHECK(!device.drvdata && allocations == frees);
    const uint16_t unsupported[] = {0x24c5, 0x24d5, 0x266e, 0x27de, 0x7195, 0xffff};
    for (unsigned index = 0; index < sizeof(unsupported) / sizeof(unsupported[0]); index++) {
        device = setup();
        device.device = unsupported[index];
        identity = ((uint32_t)device.device << 16) | 0x8086;
        CHECK(ac97_pci_probe(&device) < 0 && accesses == 0);
    }
}

static void test_capture_without_playback(void)
{
    struct device device = setup();
    struct snd_device other_output = {.name = "other-playback-owner"};
    sound = &other_output;
    unsigned previous_init = init_calls;
    int result = ac97_pci_probe(&device);
    CHECK(result == 0 && capture && sound == &other_output);
    CHECK(init_calls == previous_init);
    if (result == 0 && capture) {
        CHECK(capture->start(capture) == 0 && !(engines[1].control & 1));
        verify_input = 1;
        deliver_input_periods(2);
        interrupt_callback(interrupt_argument);
        CHECK(capture_notifications == 2 && notifications == 0);
        CHECK(buffers[0]->state == DMA_READY && buffers[1]->state == DMA_READY);
        ac97_pci_remove(&device);
    }
    CHECK(sound == &other_output && !capture && !device.drvdata);
    CHECK(init_calls == previous_init && allocations == frees);
    /* A second case rejects BOTH directions. Cleanup must not unregister
     * somebody else's devices, retain a useless slot or reset their mixer. */
    device = setup();
    fail_capture_register = 1;
    CHECK(ac97_pci_probe(&device) < 0);
    CHECK(sound == &other_output && !capture && !device.drvdata);
    CHECK(init_calls == previous_init && allocations == frees);
    fail_capture_register = 0;
    sound = NULL;
}

static void test_capture_rejection(void)
{
    struct device device = setup();
    fail_input_route = 1;
    unsigned previous_registrations = capture_register_calls;
    CHECK(ac97_pci_probe(&device) == 0 && sound && !capture);
    CHECK(capture_register_calls == previous_registrations && !buffers[2] && !buffers[3]);
    fail_input_route = 0;
    ac97_pci_remove(&device);
    device = setup();
    fail_capture_register = 1;
    CHECK(ac97_pci_probe(&device) == 0 && sound && !capture);
    CHECK(sound->start(sound) == 0);
    finish_periods(1);
    interrupt_callback(interrupt_argument);
    CHECK(notifications == 1 && capture_notifications == 0);
    ac97_pci_remove(&device);
    CHECK(!device.drvdata);
    fail_capture_register = 0;
}

static void test_probe_rejection(void)
{
    struct device device = setup();
    device.seg = 1;
    CHECK(ac97_pci_probe(&device) < 0 && accesses == 0);
    device.seg = 0;
    bars[1] = 0x3001;
    CHECK(ac97_pci_probe(&device) < 0 && accesses == 0);
    bars[1] = 0x2001;
    identity = 0xffffffff;
    CHECK(ac97_pci_probe(&device) < 0 && accesses == 0);
    identity = 0x24158086;
    fail_enable = 1;
    CHECK(ac97_pci_probe(&device) < 0 && accesses == 0);
    fail_enable = 0;
    ready = 0;
    CHECK(ac97_pci_probe(&device) < 0 && !device.drvdata && delay_count == 1000);
    ready = 1;
    fail_alloc = 1;
    CHECK(ac97_pci_probe(&device) < 0 && !device.drvdata);
    fail_alloc = 0;
    fail_irq = 1;
    CHECK(ac97_pci_probe(&device) < 0 && !device.drvdata && !sound);
    fail_irq = 0;
    CHECK(allocations == frees);
}
static void test_release_failure(void)
{
    struct device device = setup();
    CHECK(ac97_pci_probe(&device) == 0);
    fail_release = 1;
    ac97_pci_remove(&device);
    CHECK(device.drvdata && buffers[0] && buffers[1] && !sound);
    unsigned previous = accesses;
    interrupt_callback(interrupt_argument);
    CHECK(accesses == previous);
    fail_release = 0;
    ac97_pci_remove(&device);
    CHECK(!device.drvdata && !buffers[0] && !buffers[1]);
    device = setup();
    CHECK(ac97_pci_probe(&device) == 0);
    fail_free = 1;
    ac97_pci_remove(&device);
    CHECK(device.drvdata && buffers[0] && buffers[1]);
    fail_free = 0;
    ac97_pci_remove(&device);
    CHECK(!device.drvdata && !buffers[0] && !buffers[1]);
}
static void test_retained_failure(int completion_failure)
{
    struct device device = setup();
    CHECK(ac97_pci_probe(&device) == 0 && sound->start(sound) == 0);
    CHECK(capture && capture->start(capture) == 0);
    unsigned previous_frees = frees;
    fail_complete = completion_failure;
    stuck_stop = !completion_failure;
    fail_mask_once = !completion_failure;
    fail_disable_once = !completion_failure;
    capture->stop(capture);
    if (!completion_failure) {
        CHECK(delay_count == 1000 && (command & 4));
        unsigned previous = accesses;
        interrupt_callback(interrupt_argument);
        CHECK(accesses == previous);
        CHECK((command & 0x407) == 0x400);
    }
    ac97_pci_remove(&device);
    CHECK(device.drvdata && buffers[0] && buffers[1]);
    CHECK(frees == previous_frees);
    if (!buffers[0] || !buffers[1]) return;
    CHECK(buffers[0]->state == DMA_QUARANTINED && buffers[1]->state == DMA_QUARANTINED);
    CHECK(buffers[2]->state == DMA_QUARANTINED && buffers[3]->state == DMA_QUARANTINED);
    unsigned previous = accesses;
    CHECK(ac97_pci_probe(&device) < 0 && accesses == previous);
    /* Quarantined allocations intentionally survive for process lifetime. */
}
int main(int argc, char **argv)
{
    test_descriptor_bounds();
    test_playback();
    test_capture_duplex();
    test_ich_models();
    test_capture_without_playback();
    test_capture_rejection();
    test_probe_rejection();
    test_release_failure();
    test_retained_failure(argc > 1 && !strcmp(argv[1], "complete"));
    printf("AC97_HOST: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
