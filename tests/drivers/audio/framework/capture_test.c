/* Execute the production capture worker, with deterministic scheduler wakes.
 * These are service-layer tests. DMA provenance is tested separately against
 * the controller models and QEMU's external audio input backend. */
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include "c/kernel/audio/capture.c"
#include <string.h>

static unsigned checks, failures, starts, stops, allocations, init_count;
static struct thread current;
static void (*worker)(void);
static jmp_buf worker_idle;
#define CHECK(test) do { ++checks; if (!(test)) { ++failures; \
    printf("CAPTURE_FRAMEWORK_FAIL %d: %s\n", __LINE__, #test); } } while (0)

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
struct thread *sched_current_thread(void) { return &current; }
struct thread *thread_create(void (*entry)(void), const char *name)
{
    CHECK(!strcmp(name, "kcapture") && !worker);
    worker = entry;
    return &current;
}
void *kmalloc(size_t bytes) { ++allocations; return malloc(bytes); }
void kfree(void *memory) { if (memory) --allocations; free(memory); }
void kprintf(const char *format, ...) { (void)format; }
void waitq_init(struct waitq *queue) { queue->wakes = 0; }
void waitq_wake_all(struct waitq *queue) { ++queue->wakes; }
void semaphore_init(struct semaphore *sem, int value)
{
    ++init_count;
    sem->tokens = (unsigned)value;
}
void sem_post(struct semaphore *sem) { ++sem->tokens; }
void sem_wait(struct semaphore *sem)
{
    if (!sem->tokens) longjmp(worker_idle, 1);
    --sem->tokens;
}
static void pump(void)
{
    CHECK(worker != NULL);
    if (!setjmp(worker_idle)) worker();
    CHECK(!g_cap_lock.held);
}
static int start(struct snd_capdevice *device) { (void)device; ++starts; return 0; }
static void stop(struct snd_capdevice *device) { (void)device; ++stops; }

enum { PERIOD_BYTES = 32, PERIODS = 4 };
static uint8_t hardware_ring[PERIOD_BYTES * PERIODS];
static struct snd_capdevice input = {
    .name = "fixture-input", .rate = 48000, .channels = 2,
    .format = SND_FMT_S16, .period_bytes = PERIOD_BYTES, .periods = PERIODS,
    .ring = hardware_ring, .start = start, .stop = stop
};
static int owner;

static void period(unsigned index, uint8_t value)
{
    memset(hardware_ring + index * PERIOD_BYTES, value, PERIOD_BYTES);
    snd_capture_period_elapsed(&input);
}
static void expect_bytes(const uint8_t *data, size_t count, uint8_t value)
{
    for (size_t i = 0; i < count; ++i) CHECK(data[i] == value);
}
static int open_native(void)
{
    struct logit_sndfmt format = {0};
    int handle = snd_cap_open(&owner, &format);
    CHECK(handle > 0 && format.rate == 48000 && format.channels == 2);
    return handle;
}
static void registration_and_regular_input(void)
{
    CHECK(snd_register_capture_device(NULL) < 0);
    struct snd_capdevice invalid = input;
    invalid.channels = 0;
    CHECK(snd_register_capture_device(&invalid) < 0);
    invalid = input;
    invalid.period_bytes = 31;
    CHECK(snd_register_capture_device(&invalid) < 0);
    CHECK(snd_register_capture_device(&input) == 0);
    CHECK(init_count == 1);
    int handle = open_native();
    CHECK(starts == 1);
    struct snd_capdevice other = input;
    CHECK(snd_register_capture_device(&other) < 0);
    snd_capture_period_elapsed(&other);
    CHECK(g_cap_periods_done == 0);
    period(0, 0x12);
    snd_cap_init();
    CHECK(init_count == 1 && g_cap_period.tokens == 1);
    pump();
    uint8_t bytes[PERIOD_BYTES];
    CHECK(snd_cap_read(&owner, handle, bytes, sizeof bytes) == sizeof bytes);
    expect_bytes(bytes, sizeof bytes, 0x12);
    CHECK(snd_cap_close(&owner, handle) == 0);
    pump();
    CHECK(stops == 1 && allocations == 0);
}
static void overrun_excludes_active_slot(void)
{
    int handle = open_native();
    for (unsigned i = 0; i < PERIODS; ++i) period(i, (uint8_t)(0x20 + i));
    /* Four completed periods wrap the writer to slot zero. Half its bytes
     * already belong to the next period; it cannot be handed to a reader. */
    memset(hardware_ring, 0xe7, PERIOD_BYTES / 2);
    pump();
    CHECK(snd_cap_avail(&owner, handle) == 3 * PERIOD_BYTES);
    uint8_t bytes[PERIOD_BYTES * PERIODS];
    int received = snd_cap_read(&owner, handle, bytes, sizeof bytes);
    CHECK(received == 3 * PERIOD_BYTES);
    for (unsigned i = 0; i < 3; ++i)
        expect_bytes(bytes + i * PERIOD_BYTES, PERIOD_BYTES, (uint8_t)(0x21 + i));
    struct logit_sndstate state = {0};
    CHECK(snd_cap_state(&owner, handle, &state) == 0);
    CHECK(state.underruns == 1 && state.frames_played == 24);
    snd_cap_close(&owner, handle);
    pump();
}
static void detach_and_reopen(void)
{
    int handle = open_native();
    snd_unregister_capture_device(&input);
    CHECK(snd_cap_avail(&owner, handle) == SND_E_NODEV);
    uint64_t completed = g_cap_periods_done;
    snd_capture_period_elapsed(&input);
    CHECK(g_cap_periods_done == completed);
    pump();
    CHECK(allocations == 0);
    CHECK(snd_register_capture_device(&input) == 0);
    handle = open_native();
    period(0, 0x47);
    pump();
    uint8_t bytes[PERIOD_BYTES];
    CHECK(snd_cap_read(&owner, handle, bytes, sizeof bytes) == sizeof bytes);
    expect_bytes(bytes, sizeof bytes, 0x47);
    snd_cap_close(&owner, handle);
    pump();
    snd_unregister_capture_device(&input);
    CHECK(allocations == 0 && !snd_capture_present());
}
int main(void)
{
    registration_and_regular_input();
    overrun_excludes_active_slot();
    detach_and_reopen();
    printf("CAPTURE_FRAMEWORK: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
