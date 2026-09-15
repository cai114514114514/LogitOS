/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include "percpu.h"
#include "spinlock.h"
#include "tlb.h"
#include "ktime.h"
#include "kprintf.h"
#include "sched.h"
#include "pit.h"
#include "mmhost.h"
static unsigned cpus_seen;
#ifdef BKL_VERIFY_SERIAL
static spinlock_t serial_entry=SPINLOCK_INIT;
#endif
long bkl_verify_entry(long expected)
{
    if(expected<1||expected>8)return -1;
#ifdef BKL_VERIFY_SERIAL
    uint64_t flags=spin_lock_irqsave(&serial_entry);
#endif
    unsigned cpu=(unsigned)this_cpu()->index;
    __atomic_fetch_or(&cpus_seen,1u<<cpu,__ATOMIC_ACQ_REL);
    uint64_t end=time_mono_raw_ns()+3000000000ull;
    unsigned mask;
    do {
        mask=__atomic_load_n(&cpus_seen,__ATOMIC_ACQUIRE);
        if(__builtin_popcount(mask)>=expected)break;
        tlb_service();
        __asm__ volatile("pause");
    } while(time_mono_raw_ns()<end);
    /* The caller never sleeps or leaves ring 0 during this rendezvous. A CPU
     * bit therefore proves simultaneous kernel execution, unlike counting
     * syscalls that are merely outstanding while their owners are parked. */
    kprintf("[bklentry] cpu=%d simultaneous=%d expected=%d mask=%x\n",cpu,
            __builtin_popcount(mask),(int)expected,mask);
#ifdef BKL_VERIFY_SERIAL
    spin_unlock_irqrestore(&serial_entry,flags);
#endif
    if (__builtin_popcount(mask)<expected) return -1;
    /* All four CPUs stay inside this syscall. BSP ticks must still advance,
     * while nested IRQs must not schedule or change this thread's CR3/TLS. */
    struct thread *thread=sched_current_thread();
    uint64_t cr3=mm_read_cr3(); unsigned lo,hi;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000100));
    uint64_t tls=((uint64_t)hi<<32)|lo;
    unsigned long ticks=timer_ticks();
    end=time_mono_raw_ns()+200000000ull;
    while (time_mono_raw_ns()<end) __asm__ volatile("pause");
    unsigned long delta=timer_ticks()-ticks;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000100));
    int stable=(unsigned)this_cpu()->index==cpu && this_cpu()->in_kernel==1 &&
        thread==sched_current_thread() && cr3==mm_read_cr3() && tls==(((uint64_t)hi<<32)|lo);
    kprintf("[bklirq] cpu=%d ticks=%lu context_stable=%d\n",cpu,delta,stable);
    return delta>=3 && stable ? 0 : -2;
}
