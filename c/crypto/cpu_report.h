#ifndef LOGIT_CPU_REPORT_H
#define LOGIT_CPU_REPORT_H

/* Boot-path face of the CPU feature module.
 *
 * WHY IT LIVES UNDER c/crypto RATHER THAN c/kernel/cpu: c/kernel/cpu/cpufeat.c
 * is deliberately dependency-free (no kprintf, no timer) because the same TU is
 * compiled into the host-side crypto tests, and the rest of c/kernel/cpu is
 * owned by other workstreams. This TU needs kprintf, the PIT and the AES
 * backend, and its only consumer is the crypto dispatch, so it sits with the
 * thing it reports on. It is kernel-only: c/crypto's top level is outside the
 * host CRYPTO_SRC glob (aead/hash/pubkey), so no host test drags it in.
 *
 * Call order on the boot path:
 *   cpu_early_init()   -- from boot/long.asm, before kernel_main. Pure CPUID +
 *                         a pointer assignment; safe before serial, the heap,
 *                         the IDT or the timer exist.
 *   cpu_boot_report()  -- once serial is up. Prints what was detected.
 *   cpu_simd_selftest()-- once the PIT is ticking. Needs IF=0 on entry.
 * Nothing depends on the last two having been called: every query path
 * initialises lazily. */

/* Detect CPU features and select the crypto backends. Idempotent.
 * Called from boot/long.asm -- keep the name stable. */
void cpu_early_init(void);

/* Print vendor/brand, the present feature list, the XSAVE geometry and which
 * AES-GCM backend was selected. Needs serial (or VGA) to be up. */
void cpu_boot_report(void);

/* Cross-check the selected SIMD backend against the portable reference WHILE
 * interrupts are being taken, and verify the XMM register file survives those
 * interrupts. Prints CPU_SIMD_SELFTEST_OK / CPU_SIMD_SELFTEST_FAIL plus the
 * number of interrupts that actually landed. Call with IF=0 once the PIT
 * ticks. */
void cpu_simd_selftest(void);

#endif /* LOGIT_CPU_REPORT_H */
