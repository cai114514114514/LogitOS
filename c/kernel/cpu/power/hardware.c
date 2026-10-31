#include "hardware.h"
#include "percpu.h"
#include "ktime.h"
#include "io.h"

void power_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile ("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                      : "a"(leaf), "c"(sub));
}
uint64_t power_rdmsr(uint32_t reg)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(reg));
    return ((uint64_t)hi << 32) | lo;
}
void power_wrmsr(uint32_t reg, uint64_t value)
{ __asm__ volatile ("wrmsr" :: "c"(reg), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory"); }
uint64_t power_read_cr(unsigned reg)
{
    uint64_t value;
    if (reg == 0) __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    else if (reg == 3) __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    else __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}
void power_write_cr(unsigned reg, uint64_t value)
{
    if (reg == 0) __asm__ volatile ("mov %0, %%cr0" :: "r"(value) : "memory");
    else if (reg == 3) __asm__ volatile ("mov %0, %%cr3" :: "r"(value) : "memory");
    else __asm__ volatile ("mov %0, %%cr4" :: "r"(value) : "memory");
}
uint64_t power_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
void power_irq_restore(uint64_t flags)
{ __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc"); }
void power_wbinvd(void) { __asm__ volatile ("wbinvd" ::: "memory"); }
unsigned power_cpu(void) { return (unsigned)this_cpu()->index; }
uint64_t power_now(void) { return time_mono_ns(); }
void power_halt(void) { __asm__ volatile ("sti; hlt; cli" ::: "memory"); }
void power_mwait(unsigned hint, const void *address)
{
    /* IF stays zero until after MWAIT; CPUID.5's interrupt-break capability
     * is mandatory. Enabling IF before MWAIT loses a wakeup when an IRQ is
     * serviced between MONITOR and MWAIT. Service the pending IRQ afterwards.
     * Intel SDM MONITOR/MWAIT; Linux arch/x86/include/asm/mwait.h. */
    __asm__ volatile ("monitor" :: "a"(address), "c"(0), "d"(0) : "memory");
    __asm__ volatile ("mwait; sti; nop; cli" :: "a"(hint), "c"(1) : "memory");
}

void power_mce_putc(char c)
{
    /* serial_putc takes tx_lock. #MC can interrupt the very instruction
     * holding it, even with IF=0; the ordinary "raw" serial API is unsafe.
     * A bounded emergency write deliberately bypasses both console locks. */
    for (unsigned spin = 0; spin < 1000000; spin++) {
        if (inb(0x3fd) & 0x20) { outb(0x3f8, (uint8_t)c); return; }
        __asm__ volatile ("pause");
    }
}
