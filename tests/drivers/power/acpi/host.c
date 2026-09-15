/* Host service backend for a synthetic hardware-reduced ACPI board. The real
 * uACPI interpreter and production devices.c are linked unchanged. Unsupported
 * IO/PCI/IRQ accesses return errors; they never execute host hardware. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uacpi/kernel_api.h>

#ifdef POWER_EC_BOARD
#include "ec/board.h"
#endif

unsigned char power_test_memory[16384];

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out)
{
    *out = 0x100;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr address, uacpi_size length)
{
    if (address > sizeof power_test_memory ||
        length > sizeof power_test_memory - address)
        return UACPI_MAP_FAILED;
    return power_test_memory + address;
}

void uacpi_kernel_unmap(void *address, uacpi_size length)
{
    (void)address;
    (void)length;
}

void uacpi_kernel_log(uacpi_log_level level, const char *message)
{
    (void)level;
    fputs(message, stderr);
}

void *uacpi_kernel_alloc(uacpi_size size)
{
    return malloc(size);
}

void uacpi_kernel_free(void *allocation)
{
    free(allocation);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
#ifdef POWER_EC_BOARD
    static uint64_t last_model_nanoseconds;
    uint64_t model_nanoseconds = power_ec_model.now * 1000;
    if (model_nanoseconds <= last_model_nanoseconds)
        model_nanoseconds = last_model_nanoseconds + 1;
    last_model_nanoseconds = model_nanoseconds;
    return model_nanoseconds;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
#endif
}

void uacpi_kernel_stall(uacpi_u8 microseconds)
{
#ifdef POWER_EC_BOARD
    ec_model_delay(&power_ec_model, microseconds);
#else
    uint64_t start = uacpi_kernel_get_nanoseconds_since_boot();
    while (uacpi_kernel_get_nanoseconds_since_boot() - start < microseconds * 1000ull) {
    }
#endif
}

void uacpi_kernel_sleep(uacpi_u64 milliseconds)
{
#ifdef POWER_EC_BOARD
    power_ec_model.now += milliseconds * 1000;
#else
    struct timespec duration = {(time_t)(milliseconds / 1000),
                                (long)(milliseconds % 1000) * 1000000};
    while (nanosleep(&duration, &duration) && errno == EINTR) {
    }
#endif
}

uacpi_handle uacpi_kernel_create_mutex(void)
{
    pthread_mutex_t *mutex = malloc(sizeof *mutex);
    if (mutex && pthread_mutex_init(mutex, NULL)) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle handle)
{
    if (handle) {
        pthread_mutex_destroy(handle);
        free(handle);
    }
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout)
{
    if (timeout == UINT16_MAX)
        return pthread_mutex_lock(handle) ? UACPI_STATUS_INTERNAL_ERROR
                                          : UACPI_STATUS_OK;
    uint64_t start = uacpi_kernel_get_nanoseconds_since_boot();
    do {
        if (!pthread_mutex_trylock(handle))
            return UACPI_STATUS_OK;
        if (!timeout)
            break;
        uacpi_kernel_sleep(1);
    } while (uacpi_kernel_get_nanoseconds_since_boot() - start < timeout * 1000000ull);
    return UACPI_STATUS_TIMEOUT;
}

void uacpi_kernel_release_mutex(uacpi_handle handle)
{
    pthread_mutex_unlock(handle);
}

struct host_event {
    pthread_mutex_t lock;
    unsigned count;
};

uacpi_handle uacpi_kernel_create_event(void)
{
    struct host_event *event = calloc(1, sizeof *event);
    if (event)
        pthread_mutex_init(&event->lock, NULL);
    return event;
}

void uacpi_kernel_free_event(uacpi_handle handle)
{
    struct host_event *event = handle;
    if (event) {
        pthread_mutex_destroy(&event->lock);
        free(event);
    }
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout)
{
    struct host_event *event = handle;
    uint64_t start = uacpi_kernel_get_nanoseconds_since_boot();
    do {
        pthread_mutex_lock(&event->lock);
        if (event->count) {
            event->count--;
            pthread_mutex_unlock(&event->lock);
            return 1;
        }
        pthread_mutex_unlock(&event->lock);
        if (!timeout)
            break;
        uacpi_kernel_sleep(1);
    } while (timeout == UINT16_MAX ||
             uacpi_kernel_get_nanoseconds_since_boot() - start < timeout * 1000000ull);
    return 0;
}

void uacpi_kernel_signal_event(uacpi_handle handle)
{
    struct host_event *event = handle;
    pthread_mutex_lock(&event->lock);
    event->count++;
    pthread_mutex_unlock(&event->lock);
}

void uacpi_kernel_reset_event(uacpi_handle handle)
{
    struct host_event *event = handle;
    pthread_mutex_lock(&event->lock);
    event->count = 0;
    pthread_mutex_unlock(&event->lock);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    return (uacpi_thread_id)(uintptr_t)pthread_self();
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    return 0; /* This test board has no asynchronous IRQ producer. */
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state)
{
    (void)state;
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    return uacpi_kernel_create_mutex();
}

void uacpi_kernel_free_spinlock(uacpi_handle handle)
{
    uacpi_kernel_free_mutex(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle)
{
    pthread_mutex_lock(handle);
    return 0;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags)
{
    (void)flags;
    pthread_mutex_unlock(handle);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request)
{
    (void)request;
    return UACPI_STATUS_DENIED;
}

#ifndef POWER_EC_BOARD
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length,
                                 uacpi_handle *out)
{
    (void)base;
    (void)length;
    (void)out;
    return UACPI_STATUS_UNIMPLEMENTED;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    (void)handle;
}

#endif

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out)
{
    (void)address;
    (void)out;
    return UACPI_STATUS_UNIMPLEMENTED;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle)
{
    (void)handle;
}

#ifndef POWER_EC_BOARD
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,
                                                    uacpi_interrupt_handler handler,
                                                    uacpi_handle context,
                                                    uacpi_handle *out)
{
    (void)irq;
    (void)handler;
    (void)context;
    (void)out;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                                      uacpi_handle handle)
{
    (void)handler;
    (void)handle;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                        uacpi_work_handler handler,
                                        uacpi_handle context)
{
    (void)type;
    (void)handler;
    (void)context;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    return UACPI_STATUS_OK; /* No work can have been accepted by this backend. */
}

#endif

#ifndef POWER_EC_BOARD
uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset,
                                   uacpi_u8 *value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u8 value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u16 *value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u16 value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u32 *value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u32 value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

#endif

uacpi_status uacpi_kernel_pci_read8(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u8 *value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u8 value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u16 *value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle handle, uacpi_size offset,
                                      uacpi_u16 value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u32 *value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle handle, uacpi_size offset,
                                      uacpi_u32 value)
{
    (void)handle;
    (void)offset;
    (void)value;
    return UACPI_STATUS_UNIMPLEMENTED;
}
