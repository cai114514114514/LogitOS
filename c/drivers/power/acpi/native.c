#include "native.h"
#include "../../../kernel/cpu/acpi/acpi.h"
#include "../../../kernel/cpu/irq/ioapic.h"
#include "../../../kernel/cpu/smp/percpu.h"
#include "../../../kernel/cpu/spinlock.h"
#include "../../../kernel/mm/mm.h"
#include "../../../kernel/mm/mmhost.h"
#include "../../../kernel/mm/phys/kheap.h"
#include "../../../kernel/mm/phys/pmm.h"
#include "../../../kernel/mm/virt/vmm.h"
#include "../../../kernel/pci/pci.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/sync/wait.h"
#include "../../../kernel/sync/work.h"
#include "../../core/irq.h"
#include "io.h"
#include "kprintf.h"
#include "ktime.h"
#include "prot.h"
#include <string.h>
#include <uacpi/kernel_api.h>
/* Deferred AML runs on the existing sleepable kworker. Upstream recommends
 * CPU0 to avoid some SMI firmware bugs; this kernel's worker can migrate, so
 * CPU0 affinity remains an explicit real-hardware compatibility limitation. */
#define NATIVE_MAP_MAX_BYTES (1u << 20)
#define ACPI_ALIAS_BASE (PHYSMAP_BASE + PHYSMAP_SIZE)
#define PAGE_BYTES 4096ull
#define PAGE_OFFSET_MASK (PAGE_BYTES - 1)
#define PTE_PRESENT 1ull
#define PTE_WRITE_THROUGH (1ull << 3)
#define WAIT_FOREVER UINT16_MAX
#define NANOSECONDS_PER_MICROSECOND 1000ull
#define NANOSECONDS_PER_MILLISECOND 1000000ull
#define MAX_SLEEP_CHUNK_MS 60000u
#define IO_PORT_COUNT 65536u
#define PCI_CONFIG_BYTES 4096u
#define PCI_MAX_DEVICE 31u
#define PCI_MAX_FUNCTION 7u

/* A stable copy separates the bootloader's RSDP CPU pointer from the physical
 * address passed to uACPI. Its publication belongs to native initialization;
 * zero size means get_rsdp must refuse access. */
static unsigned char rsdp_copy[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));
static unsigned rsdp_size;
static uint64_t rsdp_phys;
static struct mutex map_lock = MUTEX_INIT;

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_address)
{
    if (!out_address || !rsdp_size)
        return UACPI_STATUS_NOT_FOUND;
    *out_address = rsdp_phys;
    return UACPI_STATUS_OK;
}

static int mapping_matches(uint64_t *entry, uint64_t physical_page,
                           int require_writable)
{
    if (!entry || !(*entry & PTE_PRESENT))
        return 0;
    if ((*entry & MM_PTE_ADDR) != physical_page || (*entry & VMM_USER))
        return 0;
    return !require_writable || (*entry & VMM_WRITABLE);
}

/* Called under map_lock. Rechecking the leaf matters because vmm_map_page has
 * no failure return: a requested mapping is not proof of an installed PTE. */
static int map_firmware_page(uint64_t physical_page)
{
    uint64_t virtual_page = ACPI_ALIAS_BASE + physical_page;
    uint64_t *entry = vmm_pte(mm_read_cr3(), virtual_page);
    uint64_t flags = VMM_WRITABLE;
    if (cpu_prot_nx_usable())
        flags |= MM_PTE_NX;

    if (entry && (*entry & PTE_PRESENT)) {
        if (!mapping_matches(entry, physical_page, 0))
            return -1;
        /* Existing ACPI table pages retain their cache type. FACS and AML
         * SystemMemory fields require writes, unlike the old table reader. */
        flags |= *entry & (VMM_NOCACHE | PTE_WRITE_THROUGH);
        if (!(*entry & VMM_WRITABLE))
            vmm_map_page(virtual_page, physical_page, flags);
    } else {
        vmm_map_page(virtual_page, physical_page, flags | VMM_NOCACHE);
    }

    entry = vmm_pte(mm_read_cr3(), virtual_page);
    return mapping_matches(entry, physical_page, 1) ? 0 : -1;
}

void *uacpi_kernel_map(uacpi_phys_addr physical_address, uacpi_size length)
{
    if (!length || length > NATIVE_MAP_MAX_BYTES || physical_address >= PHYSMAP_SIZE ||
        length > PHYSMAP_SIZE - physical_address)
        return UACPI_MAP_FAILED;

    if (physical_address >= rsdp_phys) {
        uint64_t offset = physical_address - rsdp_phys;
        if (offset < rsdp_size && length <= rsdp_size - offset)
            return rsdp_copy + offset;
    }

    /* Ordinary RAM keeps its established WB direct-map alias. Firmware tables
     * share acpi.c's existing WB alias; fresh device pages are UC. Never map
     * physical holes into the allocator's AVAILABLE-RAM direct map. */
    if (pmm_is_ram(physical_address, length)) {
        if (!pmm_physmap_low_ready() ||
            (physical_address + length > PMM_LOW_LIMIT && !pmm_physmap_ready()))
            return UACPI_MAP_FAILED;
        return mm_physmap_ptr(physical_address);
    }

    uint64_t first_page = physical_address & ~PAGE_OFFSET_MASK;
    uint64_t end_page =
        (physical_address + length + PAGE_OFFSET_MASK) & ~PAGE_OFFSET_MASK;
    void *result = UACPI_MAP_FAILED;

    mutex_lock(&map_lock);
    for (uint64_t page = first_page; page < end_page; page += PAGE_BYTES) {
        if (map_firmware_page(page))
            goto unlock;
    }
    result = (void *)(uintptr_t)(ACPI_ALIAS_BASE + physical_address);
unlock:
    mutex_unlock(&map_lock);
    return result;
}

void uacpi_kernel_unmap(void *address, uacpi_size length)
{
    /* These are permanent shared kernel aliases, like acpi_find_table's.
     * Removing a PTE here would invalidate another concurrent table/OpRegion
     * mapping. No RAM frame or allocator ownership is acquired by map(). */
    (void)address;
    (void)length;
}

void uacpi_kernel_log(uacpi_log_level level, const char *message)
{
    (void)level;
    kprintf("[acpi-aml] %s", message);
}

void *uacpi_kernel_alloc(uacpi_size size)
{
    return kmalloc(size);
}

void uacpi_kernel_free(void *allocation)
{
    kfree(allocation);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    return time_mono_ns();
}

void uacpi_kernel_stall(uacpi_u8 microseconds)
{
    uint64_t started_at = time_mono_ns();
    uint64_t duration = (uint64_t)microseconds * NANOSECONDS_PER_MICROSECOND;
    while (time_mono_ns() - started_at < duration)
        __asm__ volatile("pause");
}

void uacpi_kernel_sleep(uacpi_u64 milliseconds)
{
    /* AML supplies a 64-bit duration; the scheduler accepts unsigned ms.
     * Chunking preserves that duration without truncating it at the boundary. */
    while (milliseconds) {
        unsigned chunk = milliseconds > MAX_SLEEP_CHUNK_MS ? MAX_SLEEP_CHUNK_MS
                                                           : (unsigned)milliseconds;
        sched_sleep_ms(chunk);
        milliseconds -= chunk;
    }
}

/* uACPI owns these opaque handles after creation. Its destruction callbacks
 * release the matching allocation only after interpreter users have drained. */
uacpi_handle uacpi_kernel_create_mutex(void)
{
    struct mutex *mutex = kmalloc(sizeof *mutex);
    if (mutex)
        mutex_init(mutex);
    return mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle handle)
{
    kfree(handle);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout_ms)
{
    if (!handle)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (timeout_ms == WAIT_FOREVER) {
        mutex_lock(handle);
        return UACPI_STATUS_OK;
    }

    uint64_t started_at = time_mono_ns();
    uint64_t duration = (uint64_t)timeout_ms * NANOSECONDS_PER_MILLISECOND;
    do {
        if (mutex_trylock(handle))
            return UACPI_STATUS_OK;
        if (!timeout_ms)
            break;
        sched_sleep_ms(1);
    } while (time_mono_ns() - started_at < duration);
    return UACPI_STATUS_TIMEOUT;
}

void uacpi_kernel_release_mutex(uacpi_handle handle)
{
    mutex_unlock(handle);
}

uacpi_handle uacpi_kernel_create_event(void)
{
    struct semaphore *event = kmalloc(sizeof *event);
    if (event)
        semaphore_init(event, 0);
    return event;
}

void uacpi_kernel_free_event(uacpi_handle handle)
{
    kfree(handle);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout_ms)
{
    if (timeout_ms == WAIT_FOREVER) {
        sem_wait(handle);
        return 1;
    }
    return sem_wait_timeout(handle, timeout_ms);
}

void uacpi_kernel_signal_event(uacpi_handle handle)
{
    sem_post(handle);
}

void uacpi_kernel_reset_event(uacpi_handle handle)
{
    struct semaphore *event = handle;
    uint64_t saved_flags = spin_lock_irqsave(&event->wq.lock);
    event->count = 0;
    spin_unlock_irqrestore(&event->wq.lock, saved_flags);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void)
{
    return (uacpi_thread_id)sched_current_thread();
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)
{
    uint64_t saved_flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(saved_flags) : : "memory");
    return saved_flags;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state saved_flags)
{
    __asm__ volatile("pushq %0; popfq" : : "r"((uint64_t)saved_flags) : "memory", "cc");
}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    spinlock_t *lock = kmalloc(sizeof *lock);
    if (lock)
        *lock = (spinlock_t)SPINLOCK_INIT;
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle)
{
    kfree(handle);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle)
{
    return spin_lock_irqsave(handle);
}

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags saved_flags)
{
    spin_unlock_irqrestore(handle, saved_flags);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request)
{
    /* Breakpoint/Fatal have no installed debugger or termination policy yet.
     * Reporting success here would hide a firmware request we did not service. */
    (void)request;
    return UACPI_STATUS_DENIED;
}

struct port_range {
    uint32_t base;
    uint32_t length;
};

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size length,
                                 uacpi_handle *out_handle)
{
    if (!out_handle || !length || base >= IO_PORT_COUNT ||
        length > IO_PORT_COUNT - base)
        return UACPI_STATUS_INVALID_ARGUMENT;

    struct port_range *range = kmalloc(sizeof *range);
    if (!range)
        return UACPI_STATUS_OUT_OF_MEMORY;
    range->base = base;
    range->length = length;
    *out_handle = range;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    kfree(handle);
}

static int port_access_fits(const struct port_range *range, uacpi_size offset,
                            size_t width)
{
    return range && offset <= range->length && width <= range->length - offset;
}

/* Keep accesses at the requested hardware width. Four inb/outb operations are
 * not equivalent to an inl/outl on registers with read or write side effects. */
uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset,
                                   uacpi_u8 *out_value)
{
    struct port_range *range = handle;
    if (!out_value || !port_access_fits(range, offset, sizeof *out_value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inb(range->base + offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u8 value)
{
    struct port_range *range = handle;
    if (!port_access_fits(range, offset, sizeof value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    outb(range->base + offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u16 *out_value)
{
    struct port_range *range = handle;
    if (!out_value || !port_access_fits(range, offset, sizeof *out_value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inw(range->base + offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u16 value)
{
    struct port_range *range = handle;
    if (!port_access_fits(range, offset, sizeof value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    outw(range->base + offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u32 *out_value)
{
    struct port_range *range = handle;
    if (!out_value || !port_access_fits(range, offset, sizeof *out_value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = inl(range->base + offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u32 value)
{
    struct port_range *range = handle;
    if (!port_access_fits(range, offset, sizeof value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    outl(range->base + offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address,
                                          uacpi_handle *out_handle)
{
    /* The PCI backend currently addresses segment zero only. Do not silently
     * send a nonzero-segment AML request to an unrelated segment-zero device. */
    if (!out_handle || address.segment || address.device > PCI_MAX_DEVICE ||
        address.function > PCI_MAX_FUNCTION)
        return UACPI_STATUS_UNIMPLEMENTED;

    uacpi_pci_address *handle = kmalloc(sizeof *handle);
    if (!handle)
        return UACPI_STATUS_OUT_OF_MEMORY;
    *handle = address;
    *out_handle = handle;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle)
{
    kfree(handle);
}

static int pci_access_fits(const uacpi_pci_address *address, uacpi_size offset,
                           size_t width)
{
    return address && offset <= PCI_CONFIG_BYTES - width && !(offset & (width - 1));
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle handle, uacpi_size offset,
                                    uacpi_u8 *out_value)
{
    const uacpi_pci_address *address = handle;
    if (!out_value || !pci_access_fits(address, offset, sizeof *out_value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value =
        pci_cfg_read8(address->bus, address->device, address->function, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u8 value)
{
    const uacpi_pci_address *address = handle;
    if (!pci_access_fits(address, offset, sizeof value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    pci_cfg_write8(address->bus, address->device, address->function, offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u16 *out_value)
{
    const uacpi_pci_address *address = handle;
    if (!out_value || !pci_access_fits(address, offset, sizeof *out_value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value =
        pci_cfg_read16(address->bus, address->device, address->function, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle handle, uacpi_size offset,
                                      uacpi_u16 value)
{
    const uacpi_pci_address *address = handle;
    if (!pci_access_fits(address, offset, sizeof value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    pci_cfg_write16(address->bus, address->device, address->function, offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle handle, uacpi_size offset,
                                     uacpi_u32 *out_value)
{
    const uacpi_pci_address *address = handle;
    if (!out_value || !pci_access_fits(address, offset, sizeof *out_value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    *out_value = pci_cfg_read(address->bus, address->device, address->function, offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle handle, uacpi_size offset,
                                      uacpi_u32 value)
{
    const uacpi_pci_address *address = handle;
    if (!pci_access_fits(address, offset, sizeof value))
        return UACPI_STATUS_INVALID_ARGUMENT;
    pci_cfg_write(address->bus, address->device, address->function, offset, value);
    return UACPI_STATUS_OK;
}

#define DEFERRED_JOB_CAPACITY 64u
#define NATIVE_START_TIMEOUT_MS 1000u

struct deferred_job {
    uacpi_work_handler handler;
    uacpi_handle context;
};

static struct work deferred_work;
static struct work startup_probe;
static struct deferred_job deferred_jobs[DEFERRED_JOB_CAPACITY];
static spinlock_t deferred_lock = SPINLOCK_INIT;
static unsigned deferred_head;
static unsigned deferred_count;
static unsigned deferred_running;
static struct thread *deferred_thread;
static unsigned worker_ready;
static int native_start_result;
static int native_start_attempted;
static struct mutex native_start_lock = MUTEX_INIT;

/* SCI is the only interrupt source this adapter owns. Other ACPI devices need
 * their own resource/IRQ driver rather than borrowing arbitrary GSIs here. */
static struct {
    uacpi_interrupt_handler handler;
    uacpi_handle context;
    uint32_t gsi;
    int vector;
    unsigned running;
    int retained;
} sci;
static struct mutex sci_lock = MUTEX_INIT;

static void execute_deferred_jobs(void *unused)
{
    (void)unused;
    for (;;) {
        uint64_t saved_flags = spin_lock_irqsave(&deferred_lock);
        if (!deferred_count) {
            spin_unlock_irqrestore(&deferred_lock, saved_flags);
            return;
        }
        struct deferred_job job = deferred_jobs[deferred_head];
        deferred_head = (deferred_head + 1) % DEFERRED_JOB_CAPACITY;
        deferred_count--;
        deferred_running++;
        deferred_thread = sched_current_thread();
        spin_unlock_irqrestore(&deferred_lock, saved_flags);

        /* kworker runs in thread context and may sleep. No queue lock spans
         * AML execution, which can itself schedule another Notify/GPE job. */
        job.handler(job.context);

        saved_flags = spin_lock_irqsave(&deferred_lock);
        deferred_running--;
        deferred_thread = NULL;
        spin_unlock_irqrestore(&deferred_lock, saved_flags);
    }
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,
                                      uacpi_work_handler handler,
                                      uacpi_handle context)
{
    if (!handler || (type != UACPI_WORK_GPE_EXECUTION && type != UACPI_WORK_NOTIFICATION))
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (!__atomic_load_n(&worker_ready, __ATOMIC_ACQUIRE))
        return UACPI_STATUS_INIT_LEVEL_MISMATCH;

    /* SCI may submit from interrupt context. The bounded preallocated ring
     * avoids calling kmalloc under an interrupt or silently dropping a job. */
    uint64_t saved_flags = spin_lock_irqsave(&deferred_lock);
    if (deferred_count == DEFERRED_JOB_CAPACITY) {
        spin_unlock_irqrestore(&deferred_lock, saved_flags);
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    unsigned tail = (deferred_head + deferred_count) % DEFERRED_JOB_CAPACITY;
    deferred_jobs[tail] = (struct deferred_job){handler, context};
    deferred_count++;
    spin_unlock_irqrestore(&deferred_lock, saved_flags);

    /* Already queued is not an error: that queued invocation drains the ring.
     * work_queue can also enqueue a running item for the next kworker pass. */
    work_queue(&deferred_work);
    return UACPI_STATUS_OK;
}

static void handle_sci(void *unused)
{
    (void)unused;
    __atomic_fetch_add(&sci.running, 1, __ATOMIC_ACQUIRE);
    sci.handler(sci.context);
    __atomic_fetch_sub(&sci.running, 1, __ATOMIC_RELEASE);
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle context,
    uacpi_handle *out_handle)
{
    if (!handler || !out_handle)
        return UACPI_STATUS_INVALID_ARGUMENT;

    const uint8_t *fadt = acpi_find_table("FACP");
    uint32_t table_length = 0;
    uint16_t firmware_sci = 0;
    if (fadt)
        memcpy(&table_length, fadt + 4, sizeof table_length);
    if (table_length < 48)
        return UACPI_STATUS_NOT_FOUND;
    memcpy(&firmware_sci, fadt + 46, sizeof firmware_sci);
    if (irq != firmware_sci)
        return UACPI_STATUS_UNIMPLEMENTED;

    uint32_t gsi = irq < 16 ? acpi_gsi_for_irq((int)irq) : irq;
    uint16_t flags = irq < 16 ? acpi_gsi_flags((int)irq) : 0;
    unsigned polarity = flags & 3;
    unsigned trigger = (flags >> 2) & 3;
    if ((flags & ~15u) || polarity == 2 || trigger == 2)
        return UACPI_STATUS_INVALID_ARGUMENT;
    int active_low = polarity != 1;
    int level_triggered = trigger != 1;
    if (!ioapic_can_route(gsi, g_cpus[0].lapic_id))
        return UACPI_STATUS_UNIMPLEMENTED;

    mutex_lock(&sci_lock);
    uacpi_status status = UACPI_STATUS_ALREADY_EXISTS;
    /* The existing model has no shared-GSI dispatcher. Refuse an active route
     * instead of replacing another device's vector. SCI defaults to level/low
     * unless the MADT explicitly overrides it. */
    if (sci.retained || ioapic_is_masked(gsi) != 1)
        goto unlock;

    int vector = irq_alloc_vector(handle_sci, NULL, "acpi-sci");
    if (vector < 0) {
        status = UACPI_STATUS_OUT_OF_MEMORY;
        goto unlock;
    }
    sci.handler = handler;
    sci.context = context;
    sci.gsi = gsi;
    sci.vector = vector;
    sci.retained = 1;

    int routed = ioapic_route(gsi, (uint8_t)vector, g_cpus[0].lapic_id,
                              level_triggered, active_low);
    if (routed != IOAPIC_ROUTE_OK) {
        if (routed == IOAPIC_ROUTE_SAFE_REJECT) {
            irq_free_vector(vector);
            sci.retained = 0;
        }
        /* An unconfirmed mask must retain the callback and vector. Failed
         * namespace initialization is latched and never freed/retried here. */
        status = UACPI_STATUS_HARDWARE_TIMEOUT;
        goto unlock;
    }
    *out_handle = &sci;
    status = UACPI_STATUS_OK;
unlock:
    mutex_unlock(&sci_lock);
    return status;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle handle)
{
    mutex_lock(&sci_lock);
    uacpi_status status = UACPI_STATUS_INVALID_ARGUMENT;
    if (handle != &sci || !sci.retained || sci.handler != handler)
        goto unlock;
    if (ioapic_mask(sci.gsi)) {
        status = UACPI_STATUS_HARDWARE_TIMEOUT;
        goto unlock;
    }

    /* The vector layer drains callbacks and EOI after the hardware source is
     * acknowledged masked. Only then may uACPI release handler context. */
    irq_free_vector(sci.vector);
    sci.retained = 0;
    sci.handler = NULL;
    sci.context = NULL;
    status = UACPI_STATUS_OK;
unlock:
    mutex_unlock(&sci_lock);
    return status;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
    /* uACPI disables/unpublishes the relevant source before calling this
     * barrier. Drain IRQ producers before looking at their deferred jobs. */
    while (__atomic_load_n(&sci.running, __ATOMIC_ACQUIRE))
        sched_sleep_ms(1);

    for (;;) {
        uint64_t saved_flags = spin_lock_irqsave(&deferred_lock);
        int complete = !deferred_count && !deferred_running;
        int self_wait = deferred_thread == sched_current_thread();
        spin_unlock_irqrestore(&deferred_lock, saved_flags);
        if (complete)
            return UACPI_STATUS_OK;
        if (self_wait)
            return UACPI_STATUS_DENIED;
        sched_sleep_ms(1);
    }
}

static void confirm_worker_started(void *unused)
{
    (void)unused;
    __atomic_store_n(&worker_ready, 1, __ATOMIC_RELEASE);
}

int power_acpi_native_start(void)
{
    mutex_lock(&native_start_lock);
    if (native_start_attempted)
        goto done;
    native_start_attempted = 1;
    native_start_result = UACPI_STATUS_INIT_LEVEL_MISMATCH;
    if (!sched_current_thread() || !time_ready())
        goto done;
    if (acpi_tables_init()) {
        native_start_result = UACPI_STATUS_NOT_FOUND;
        goto done;
    }

    int copied = acpi_copy_rsdp(rsdp_copy, sizeof rsdp_copy);
    uint64_t physical_address = mm_v2p(rsdp_copy);
    if (copied < 0 || physical_address == MM_PHYS_INVALID) {
        native_start_result = UACPI_STATUS_MAPPING_FAILED;
        goto done;
    }
    rsdp_phys = physical_address;
    rsdp_size = (unsigned)copied;

    work_item_init(&deferred_work, execute_deferred_jobs, NULL);
    work_item_init(&startup_probe, confirm_worker_started, NULL);
    work_queue(&startup_probe);
    uint64_t started_at = time_mono_ns();
    uint64_t timeout = NATIVE_START_TIMEOUT_MS * NANOSECONDS_PER_MILLISECOND;
    while (!__atomic_load_n(&worker_ready, __ATOMIC_ACQUIRE)) {
        if (time_mono_ns() - started_at >= timeout) {
            native_start_result = UACPI_STATUS_TIMEOUT;
            goto done;
        }
        sched_sleep_ms(1);
    }
    native_start_result = UACPI_STATUS_OK;
    kprintf("[acpi-aml] native services ready; GPE work uses shared kworker\n");
done:
    mutex_unlock(&native_start_lock);
    return native_start_result;
}
