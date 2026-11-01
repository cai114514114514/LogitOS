/* The kernel's implementation of the services the scheduler observer needs
 * (see observe_platform.h): interrupt masking, clocks, the CPU number, memory,
 * and access to the system-call caller's memory and identity.  The host test
 * does not compile this file; it supplies fixtures with the same names. */
#include "observe_platform.h"
#include "kheap.h"
#include "ktime.h"
#include "percpu.h"
#include "smp.h"
#include "usercopy.h"
#include "vfs_cred.h"
#include "proc.h"
#ifdef SCHED_OBSERVE_GUEST
#include "sched.h"
#endif

#ifdef SCHED_OBSERVE_GUEST
static int g_test_thread_id;

/* Remember which kernel thread runs the built-in diagnostic program. */
void observe_platform_set_test_thread(int thread_id)
{
    g_test_thread_id = thread_id;
}

/* Only the designated test thread may pass kernel-owned request buffers.
 * Production images and every other caller still copy through the checked
 * user-memory routines.  The program therefore exercises the same system-call
 * dispatcher and pools as a real user process, not alternative code. */
static int caller_is_test_thread(void)
{
    return g_test_thread_id && sched_thread_id(this_cpu()->current) == g_test_thread_id;
}
#else
/* Ordinary images have no diagnostic thread, so no caller is ever trusted to
 * pass a kernel address.  Keeping the answer a function lets the copy routines
 * below read the same in both kinds of image. */
static int caller_is_test_thread(void)
{
    return 0;
}
#endif

/* RDTSC returns the counter in EDX:EAX; EDX holds the upper 32 bits. */
#define TSC_HIGH_HALF_SHIFT 32

/* Disable interrupts with the push-flags / pop / CLI sequence that
 * spin_lock_irqsave() also uses, but without that function's measurement hooks
 * (Intel SDM 325384-093US, Vol. 2, CLI and POPF): all caller flags are kept so
 * the interrupt flag can be put back exactly. */
uint64_t observe_irq_save(void)
{
    uint64_t saved_flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(saved_flags) :: "memory");
    return saved_flags;
}

/* Restore the caller's interrupt flag rather than assuming it was enabled. */
void observe_irq_restore(uint64_t saved_flags)
{
    __asm__ volatile ("pushq %0; popfq" :: "r"(saved_flags) : "memory", "cc");
}

/* Both clocks are recorded everywhere; the monotonic clock's cross-CPU clamp is
 * not evidence that the TSCs agree. */
uint64_t observe_monotonic_ns(void)
{
    return time_mono_ns();
}

/* LFENCE / RDTSC / LFENCE keeps the counter read in order with the loads and
 * instructions around it (Intel SDM 325384-093US, Vol. 2, RDTSC, "Instruction
 * Ordering").  Nothing is inferred about an invariant TSC here: the later probe
 * stage decides, with its own feature check and offset uncertainty. */
uint64_t observe_read_tsc(void)
{
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << TSC_HIGH_HALF_SHIFT) | low;
}

/* Ask afresh every time: the calling thread may have migrated since the last
 * call, so a CPU number cached at interrupt entry could be wrong. */
unsigned observe_current_cpu(void)
{
    return (unsigned)this_cpu()->index;
}

/* Zeroed, overflow-checked allocation.  Process context only; the interrupt
 * paths never allocate.  How much may be asked for is fixed by the ABI, not by
 * how much heap happens to be free. */
void *observe_allocate_zeroed(size_t count, size_t element_size)
{
    if (!count || !element_size || count > SIZE_MAX / element_size) {
        return NULL;
    }
    size_t bytes = count * element_size;
    unsigned char *memory = kmalloc(bytes);
    if (memory) {
        for (size_t i = 0; i < bytes; i++) {
            memory[i] = 0;
        }
    }
    return memory;
}

/* All the observer's memory goes through these two functions, so the host test
 * replaces exactly the allocator and nothing else. */
void observe_free(void *allocation)
{
    if (allocation) {
        kfree(allocation);
    }
}

/* Copy from the system-call caller; 0 on success, non-zero if any byte is not
 * readable.  Done before any lock or pool is touched. */
int observe_copy_from_caller(void *destination, uint64_t caller_address, size_t bytes)
{
    if (caller_is_test_thread()) {
        const unsigned char *source = (const unsigned char *)(uintptr_t)caller_address;
        unsigned char *target = destination;
        for (size_t i = 0; i < bytes; i++) {
            target[i] = source[i];
        }
        return 0;
    }
    return user_copy_from(destination, (const void *)(uintptr_t)caller_address, bytes);
}

/* Copy complete private records out to the caller.  Never a pointer into the
 * observer's live pools, which other CPUs are writing. */
int observe_copy_to_caller(uint64_t caller_address, const void *source, size_t bytes)
{
    if (caller_is_test_thread()) {
        const unsigned char *from = source;
        unsigned char *target = (unsigned char *)(uintptr_t)caller_address;
        for (size_t i = 0; i < bytes; i++) {
            target[i] = from[i];
        }
        return 0;
    }
    return user_copy_to((void *)(uintptr_t)caller_address, source, bytes);
}

/* Check a caller's output range before a command has any effect, so a bad
 * pointer leaves no half-done run behind. */
int observe_caller_range_writable(uint64_t caller_address, size_t bytes)
{
    if (caller_is_test_thread()) {
        return caller_address != 0 && caller_address <= UINTPTR_MAX - bytes;
    }
    return user_range_ok((const void *)(uintptr_t)caller_address, bytes, 1);
}

/* Who is asking: user id and process id.  Ownership of a run is the process plus
 * the user, so another process of the same user cannot export a run's private
 * input records; root keeps the documented diagnostic override. */
void observe_caller_identity(uint32_t *user_id, uint32_t *process_id)
{
    struct vcred credentials;
    vfs_cred_current(&credentials);
    struct proc *process = proc_current();
    *user_id = credentials.uid;
    *process_id = process ? (uint32_t)process->pid : 0;
}

/* The CPUs that exist now.  This stage never brings CPUs online or offline, so
 * the set is fixed for the length of a run.  With 32 or more CPUs every bit is
 * set, since the recorder follows at most SCHED_OBSERVE_MAX_CPUS. */
uint32_t observe_online_cpu_mask(void)
{
    unsigned count = smp_cpu_count();
    if (!count) {
        count = 1;
    }
    if (count >= SCHED_OBSERVE_MAX_CPUS) {
        return UINT32_MAX;
    }
    return (1u << count) - 1;
}
