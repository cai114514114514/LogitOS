#ifndef LOGIT_CPUFEAT_H
#define LOGIT_CPUFEAT_H

#include <stdint.h>

/* CPUID-derived feature detection, decoded once and cached.
 *
 * Before this module the kernel queried CPUID ad hoc in exactly one place
 * (c/kernel/core/rng.c, for RDRAND/RDSEED) and had no way to ask "does this
 * machine have AES-NI?" at all. Everything that wants a runtime-selected
 * implementation -- the AES-GCM backend in c/crypto/aead, and later a
 * vectorised memcpy/strlen in mini-libc -- asks here.
 *
 * The module is deliberately free of kernel dependencies (no kprintf, no
 * heap, no locks): the same translation unit is compiled into the host-side
 * crypto tests, and on a non-x86 host it degrades to "no features" rather
 * than failing to build. cpu_features_print() lives in the kernel-only
 * caller, not here.
 *
 * Note what is NOT here: nothing in this file changes CR4 or XCR0. Detection
 * is passive. See cpu_features::xsave_* and the AVX comment in
 * c/kernel/cpu/cpu_selftest.c for why AVX stays off. */

enum cpu_feat {
    /* leaf 1, EDX */
    CPU_FPU = 0, CPU_TSC, CPU_MSR, CPU_CMOV, CPU_CLFLUSH, CPU_MMX,
    CPU_FXSR, CPU_SSE, CPU_SSE2, CPU_HTT,
    /* leaf 1, ECX */
    CPU_SSE3, CPU_PCLMULQDQ, CPU_MONITOR, CPU_SSSE3, CPU_FMA, CPU_CX16,
    CPU_SSE41, CPU_SSE42, CPU_X2APIC, CPU_MOVBE, CPU_POPCNT, CPU_AES,
    CPU_XSAVE, CPU_OSXSAVE, CPU_AVX, CPU_F16C, CPU_RDRAND, CPU_HYPERVISOR,
    /* leaf 7 subleaf 0, EBX */
    CPU_FSGSBASE, CPU_BMI1, CPU_AVX2, CPU_SMEP, CPU_BMI2, CPU_ERMS,
    CPU_INVPCID, CPU_AVX512F, CPU_AVX512DQ, CPU_RDSEED, CPU_ADX, CPU_SMAP,
    CPU_CLFLUSHOPT, CPU_SHA, CPU_AVX512BW, CPU_AVX512VL,
    /* leaf 7 subleaf 0, ECX */
    CPU_AVX512VBMI, CPU_UMIP, CPU_PKU, CPU_VAES, CPU_VPCLMULQDQ, CPU_RDPID,
    /* leaf 0x80000001 */
    CPU_LZCNT, CPU_PREFETCHW, CPU_NX, CPU_PDPE1GB, CPU_RDTSCP, CPU_LM,

    CPU_FEAT_COUNT
};

struct cpu_features {
    int      valid;              /* cpu_features_init() has run */
    int      is_x86;             /* 0 on a non-x86 host build (all bits clear) */
    uint32_t max_leaf;           /* CPUID.0:EAX */
    uint32_t max_ext_leaf;       /* CPUID.0x80000000:EAX */
    uint8_t  has[CPU_FEAT_COUNT];

    /* CPUID leaf 0x0D subleaf 0. Read for the record even though we never set
     * CR4.OSXSAVE: `xsave_max_size` is the size an XSAVE area would have to be
     * to hold every state component this CPU supports, and `xcr0_supported`
     * is the mask of components it would let us enable. `xsave_enabled_size`
     * (EBX) is only meaningful once OSXSAVE is on -- with OSXSAVE clear the
     * CPU reports it against an XCR0 of 1, so do not trust it as a size for a
     * future AVX migration; size the area from ECX. */
    uint32_t xsave_max_size;
    uint32_t xsave_enabled_size;
    uint64_t xcr0_supported;

    char vendor[13];             /* "GenuineIntel" / "AuthenticAMD" / ... */
    char brand[49];              /* leaves 0x80000002..4, NUL-terminated */
};

/* Idempotent. Safe to call before the heap, the timer or interrupts exist:
 * it executes CPUID and writes static storage, nothing else. */
void cpu_features_init(void);

/* Never NULL; auto-initialises on first use so a caller that runs before the
 * boot-path call (host tests, an early driver) still gets real answers. */
const struct cpu_features *cpu_features(void);

/* 1 / 0. Out-of-range feature ids answer 0. */
int cpu_has(enum cpu_feat f);

/* Short lowercase mnemonic ("sse4.2", "aes", ...) or "?" if out of range. */
const char *cpu_feat_name(enum cpu_feat f);

/* Space-separated list of the PRESENT features, truncated to fit `n` (always
 * NUL-terminated). Returns the number of features actually written -- so a
 * caller can report "37 of 54 present" the way the trust store reports its
 * root count, instead of printing a list nobody counts. */
int cpu_features_str(char *buf, int n);

/* Number of features this build knows how to detect (CPU_FEAT_COUNT) and how
 * many of them this CPU reports. */
int cpu_features_present_count(void);

#endif /* LOGIT_CPUFEAT_H */
