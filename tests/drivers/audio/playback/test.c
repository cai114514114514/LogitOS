/* Execute the production mixer and PCM conversion, observing bytes in the DMA
 * ring. Kernel wait/scheduler services are controlled by this fixture; PCI and
 * real-time deadlines belong to the independent guest gates. */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "c/kernel/audio/mixer.c"
#include <string.h>

static unsigned checks, failures, live_allocations, allocation_calls, fail_allocation;
static unsigned queue_inits, semaphore_inits, worker_creations, starts, stops;
static unsigned scheduler_ready, fail_start, synchronous_irq;
static struct thread current;
static void (*worker)(void);
static void (*wait_interleave)(void);
static jmp_buf worker_idle;
static struct snd_device *last_started;
static int owner;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; \
    printf("PLAYBACK_FRAMEWORK_FAIL %d: %s\n", __LINE__, #test); } } while (0)

uint64_t spin_lock_irqsave(spinlock_t *lock)
{
    CHECK(!lock->held);
    lock->held = 1;
    return 0;
}
void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    (void)flags;
    CHECK(lock->held);
    lock->held = 0;
}
struct thread *sched_current_thread(void)
{
    return scheduler_ready ? &current : NULL;
}
void thread_create(void (*entry)(void), const char *name)
{
    CHECK(!strcmp(name, "kaudio") && !worker && !g_snd_lock.held);
    ++worker_creations;
    worker = entry;
}
void *kmalloc(size_t bytes)
{
    ++allocation_calls;
    if (allocation_calls == fail_allocation) {
        return NULL;
    }
    void *memory = malloc(bytes);
    CHECK(memory != NULL);
    ++live_allocations;
    memset(memory, 0xad, bytes);
    return memory;
}
void kfree(void *memory)
{
    if (memory) {
        CHECK(live_allocations > 0);
        --live_allocations;
        free(memory);
    }
}
void kprintf(const char *format, ...) { (void)format; }
void snd_report_once(void) { }
void snd_cap_init(void) { }
void snd_cap_report(void) { }
void waitq_init(struct waitq *queue)
{
    ++queue_inits;
    queue->wakes = 0;
}
void waitq_wake_all(struct waitq *queue) { ++queue->wakes; }
void semaphore_init(struct semaphore *sem, int value)
{
    ++semaphore_inits;
    sem->tokens = (unsigned)value;
}
void sem_post(struct semaphore *sem) { ++sem->tokens; }
void sem_wait(struct semaphore *sem)
{
    CHECK(!g_snd_lock.held && !g_event_lock.held);
    if (!sem->tokens) {
        longjmp(worker_idle, 1);
    }
    --sem->tokens;
}
void playback_test_wait(void)
{
    CHECK(!g_snd_lock.held);
    if (wait_interleave) {
        void (*action)(void) = wait_interleave;
        wait_interleave = NULL;
        action();
    }
}
static void pump(void)
{
    CHECK(worker != NULL);
    if (!setjmp(worker_idle)) {
        worker();
    }
    CHECK(!g_snd_lock.held && !g_event_lock.held);
}
static int start(struct snd_device *device)
{
    ++starts;
    last_started = device;
    size_t size = (size_t)device->period_bytes * device->periods;
    for (size_t index = 0; index < size; ++index) {
        CHECK(device->ring[index] == 0);
    }
    if (synchronous_irq) {
        /* The driver owns its gate during an ISR. Event publication must not
         * recursively take the mixer lock held across this start callback. */
        snd_period_elapsed(device);
    }
    return fail_start ? -1 : 0;
}
static void stop(struct snd_device *device) { (void)device; ++stops; }

static _Alignas(16) uint8_t first_ring[4096 * 8];
static _Alignas(16) uint8_t second_ring[64 * 2];
static struct snd_device first = {
    .name = "fixture-first", .rate = 48000, .channels = 2, .format = SND_FMT_S16,
    .period_bytes = 4096, .periods = 8, .ring = first_ring, .start = start, .stop = stop
};
static struct snd_device second = {
    .name = "fixture-second", .rate = 22050, .channels = 1, .format = SND_FMT_S16,
    .period_bytes = 64, .periods = 2, .ring = second_ring, .start = start, .stop = stop
};
static int open_native(struct snd_device *device)
{
    struct logit_sndfmt format = {.rate = device->rate, .channels = device->channels,
        .format = SND_FMT_S16, .flags = SND_F_NONBLOCK};
    int handle = snd_stream_open(&owner, &format);
    CHECK(handle > 0);
    return handle;
}
static void expect_sample_pairs(const uint8_t *data, size_t frames,
                                int16_t left, int16_t right)
{
    const int16_t *samples = (const int16_t *)data;
    for (size_t frame = 0; frame < frames; ++frame) {
        CHECK(samples[frame * 2] == left && samples[frame * 2 + 1] == right);
    }
}
static void geometry_and_rollback(void)
{
    snd_init();
    snd_init();
    CHECK(semaphore_inits == 1 && queue_inits == 8 && live_allocations == 0);
    CHECK(snd_register_device(NULL) < 0);
    for (unsigned which = 0; which < 14; ++which) {
        struct snd_device invalid = first;
        switch (which) {
        case 0: invalid.channels = 0; break;
        case 1: invalid.channels = 3; break;
        case 2: invalid.rate = 0; break;
        case 3: invalid.format = SND_FMT_F32; break;
        case 4: invalid.period_bytes = 4095; break;
        case 5: invalid.periods = 1; break;
        case 6: invalid.periods = UINT32_MAX; break;
        case 7: invalid.period_bytes = UINT32_MAX - 3u; break;
        case 8: invalid.ring = first_ring + 1; break;
        case 9: invalid.ring = (uint8_t *)(UINTPTR_MAX - 1u); break;
        case 10: invalid.start = NULL; break;
        case 11: invalid.stop = NULL; break;
        case 12: invalid.name = NULL; break;
        default: invalid.channels = 1; invalid.period_bytes = 65536; break;
        }
        CHECK(snd_register_device(&invalid) < 0 && !snd_present());
        CHECK(live_allocations == 0);
    }
    for (unsigned failure = 1; failure <= 3; ++failure) {
        fail_allocation = allocation_calls + failure;
        CHECK(snd_register_device(&first) < 0 && !snd_present());
        CHECK(live_allocations == 0);
    }
    fail_allocation = 0;
}
static void repeated_init_and_real_mix(void)
{
    CHECK(snd_register_device(&first) == 0 && live_allocations == 3);
    snd_init();
    CHECK(starts == 0 && worker_creations == 0);
    CHECK(!snd_engine_ensure());
    unsigned allocated = allocation_calls;
    g_str[0].wq.wakes = 77;
    snd_init();
    CHECK(allocation_calls == allocated && g_str[0].wq.wakes == 77);
    CHECK(semaphore_inits == 1 && queue_inits == 8);
    CHECK(snd_register_device(&second) < 0 && allocation_calls == allocated);
    scheduler_ready = 1;
    synchronous_irq = 1;
    int handle = open_native(&first);
    synchronous_irq = 0;
    CHECK(starts == 1 && last_started == &first && worker_creations == 1);
    CHECK(g_periods_done == 1);
    unsigned tokens = g_period.tokens;
    snd_period_elapsed(&second);
    CHECK(g_periods_done == 1 && g_period.tokens == tokens);
    snd_init();
    CHECK(starts == 1 && worker_creations == 1 && g_period.tokens == tokens);
    static int16_t samples[1024 * 2 * 3];
    for (unsigned index = 0; index < sizeof samples / sizeof samples[0]; index += 2) {
        samples[index] = 1234;
        samples[index + 1] = -4321;
    }
    CHECK(snd_stream_write(&owner, handle, samples, sizeof samples) == sizeof samples);
    pump();
    expect_sample_pairs(first_ring + 2 * 4096, 1024, 1234, -4321);
    expect_sample_pairs(first_ring + 3 * 4096, 1024, 1234, -4321);
    /* Re-init must preserve an open stream's wait queue, resampler and scratch. */
    allocated = allocation_calls;
    snd_init();
    CHECK(allocation_calls == allocated && snd_stream_avail(&owner, handle) > 0);
    snd_unregister_device(&second);
    CHECK(snd_present());
    snd_unregister_device(&first);
    CHECK(!snd_present() && live_allocations == 0 && stops == 0);
    CHECK(snd_stream_avail(&owner, handle) == SND_E_NODEV);
    tokens = g_period.tokens;
    snd_period_elapsed(&first);
    CHECK(g_period.tokens == tokens && g_periods_done == 0);
    /* The driver owns final hardware stop. The upper layer only removes CPU
     * access; no test equates unregister with confirmed hardware quiescence. */
}
static void new_geometry_and_small_ring(void)
{
    memset(first_ring, 0x95, sizeof first_ring);
    memset(second_ring, 0xa7, sizeof second_ring);
    unsigned before_starts = starts;
    CHECK(snd_register_device(&second) == 0);
    int handle = open_native(&second);
    CHECK(starts == before_starts + 1 && last_started == &second);
    CHECK(worker_creations == 1 && g_period_frames == 32);
    int16_t samples[32];
    for (unsigned index = 0; index < 32; ++index) {
        samples[index] = (int16_t)(1000 + index * 7);
    }
    CHECK(snd_stream_write(&owner, handle, samples, sizeof samples) == sizeof samples);
    /* One completed period: slot one is under the device's read pointer.
     * Protect a recognizable in-flight payload while slot zero is refilled. */
    memset(second_ring + 64, 0x6d, 64);
    snd_period_elapsed(&second);
    pump();
    CHECK(memcmp(second_ring, samples, sizeof samples) == 0);
    for (unsigned index = 64; index < 128; ++index) {
        CHECK(second_ring[index] == 0x6d);
    }
    for (unsigned index = 0; index < sizeof first_ring; ++index) {
        CHECK(first_ring[index] == 0x95);
    }
    snd_unregister_device(&second);
    CHECK(live_allocations == 0);
}
/* Keep this frame in ASan's trace: the mandatory scratch mutation must fail
 * inside this exact input case, not merely terminate somewhere in the suite. */
__attribute__((noinline)) static void eight_channel_float_input(void)
{
    CHECK(snd_register_device(&first) == 0);
    struct logit_sndfmt format = {.rate = 192000, .channels = 8,
        .format = SND_FMT_F32, .flags = SND_F_NONBLOCK};
    int handle = snd_stream_open(&owner, &format);
    CHECK(handle > 0);
    static float samples[4096 * 8];
    for (unsigned frame = 0; frame < 4096; ++frame) {
        for (unsigned channel = 0; channel < 8; ++channel) {
            samples[frame * 8 + channel] = channel == 0 ? 0.25f :
                                          channel == 1 ? -0.5f : 0.75f;
        }
    }
    CHECK(snd_stream_write(&owner, handle, samples, sizeof samples) == sizeof samples);
    snd_period_elapsed(&first);
    pump();
    /* The resampler begins with zero history, yielding one initial silent
     * frame at phase zero. The remaining 1023 frames use the constant input. */
    expect_sample_pairs(first_ring + 2 * 4096, 1, 0, 0);
    expect_sample_pairs(first_ring + 2 * 4096 + 4, 1023, 8191, -16383);
    snd_unregister_device(&first);
    CHECK(live_allocations == 0);
}
static void failed_start_is_not_retried(void)
{
    CHECK(snd_register_device(&first) == 0);
    fail_start = 1;
    unsigned before = starts;
    CHECK(!snd_engine_ensure() && starts == before + 1);
    memset(first_ring, 0xef, sizeof first_ring);
    snd_init();
    CHECK(!snd_engine_ensure() && starts == before + 1);
    for (unsigned index = 0; index < sizeof first_ring; ++index) {
        CHECK(first_ring[index] == 0xef);
    }
    snd_period_elapsed(&first);
    pump();
    CHECK(snd_register_device(&second) < 0);
    snd_unregister_device(&first);
    CHECK(live_allocations == 0);
    fail_start = 0;
    CHECK(snd_register_device(&second) == 0);
    CHECK(snd_engine_ensure() && starts == before + 2);
    snd_unregister_device(&second);
    CHECK(live_allocations == 0 && worker_creations == 1);
}
static int replacement_handle;
static void replace_while_writer_waits(void)
{
    snd_unregister_device(&first);
    CHECK(snd_register_device(&second) == 0);
    replacement_handle = open_native(&second);
}
static void waiting_writer_generation(void)
{
    CHECK(snd_register_device(&first) == 0);
    struct logit_sndfmt format = {.rate = 48000, .channels = 2, .format = SND_FMT_S16};
    int handle = snd_stream_open(&owner, &format);
    CHECK(handle > 0);
    int available = snd_stream_avail(&owner, handle);
    uint8_t *data = malloc((size_t)available);
    memset(data, 0x71, (size_t)available);
    CHECK(snd_stream_write(&owner, handle, data, available) == available);
    free(data);
    wait_interleave = replace_while_writer_waits;
    const uint32_t stale_sample = 0x77777777;
    CHECK(snd_stream_write(&owner, handle, &stale_sample, 4) == 0);
    CHECK(replacement_handle != handle);
    struct logit_sndstate state;
    CHECK(snd_stream_state(&owner, replacement_handle, &state) == 0);
    CHECK(state.frames_written == 0);
    snd_unregister_device(&second);
    CHECK(live_allocations == 0);
}
int main(void)
{
    geometry_and_rollback();
    repeated_init_and_real_mix();
    new_geometry_and_small_ring();
    eight_channel_float_input();
    failed_start_is_not_retried();
    waiting_writer_generation();
    CHECK(semaphore_inits == 1 && queue_inits == 8 && worker_creations == 1);
    printf("PLAYBACK_FRAMEWORK: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
