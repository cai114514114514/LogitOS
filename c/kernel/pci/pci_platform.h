/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOGIT_PCI_PLATFORM_H
#define LOGIT_PCI_PLATFORM_H
#include <stdint.h>
/* Correction to the historical all-machines edge workaround: physical PCI
 * INTx is level/active-low. smp.c recorded a TCG remote-IRR/EOI interrupt storm
 * (~2M IRQ/s); retain that workaround only for the explicit TCG CPUID vendor.
 * QEMU publishes this vendor in target/i386/cpu.c leaf 0x40000000. KVM, other
 * hypervisors and bare metal use normal level triggering. No DMI/board guess. */
static inline int pci_intx_level_for_cpu(uint32_t features, uint32_t maxleaf,
                                        const uint32_t vendor[3])
{
    static const char tcg[] = "TCGTCGTCGTCG";
    const unsigned char *bytes = (const unsigned char *)vendor;
    if (!(features & (1u << 31)) || maxleaf < 0x40000000u) return 1;
    for (unsigned i = 0; i < 12; i++) if (bytes[i] != (unsigned char)tcg[i]) return 1;
    return 0;
}
static inline int pci_intx_level(void)
{
#if defined(__x86_64__) && !defined(LOGIT_HOST_TEST)
    uint32_t a, b, c, d, features;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0u));
    features = c;
    if (!(features & (1u << 31))) return 1;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000000u), "c"(0u));
    uint32_t vendor[3] = { b, c, d };
    return pci_intx_level_for_cpu(features, a, vendor);
#else
    return 1; /* Host fixtures represent physical PCI unless they supply CPUID. */
#endif
}
#endif
