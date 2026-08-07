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
 *   cpu_early_init()   -- from boot/long.asm, before kernel_main. Detects,
 *                         selects the backends, and prints the report.
 *   cpu_simd_selftest()-- once the PIT is ticking. Needs IF=0 on entry.
 * Nothing depends on either having been called: every query path initialises
 * lazily, so a caller that skips the boot path still gets real answers. */

/* Detect CPU features, select the crypto backends, and print vendor/brand,
 * the present feature list, the XSAVE geometry and the chosen AES-GCM
 * backend. Idempotent. Called from boot/long.asm -- keep the name stable.
 *
 * It prints through serial_init/serial_puts rather than kprintf because it
 * runs BEFORE kernel_main: kprintf also drives VGA and the klog ring, neither
 * of which is up yet, whereas the UART is pure port I/O and serial_init is
 * idempotent (kernel_main calls it again a few instructions later). Doing the
 * report here rather than from kernel_main keeps the whole feature module
 * self-contained -- no other subsystem has to remember to call it. */
void cpu_early_init(void);

/* Cross-check the selected SIMD backend against the portable reference WHILE
 * interrupts are being taken, and verify the XMM register file survives those
 * interrupts. Prints CPU_SIMD_SELFTEST_OK / CPU_SIMD_SELFTEST_FAIL plus the
 * number of interrupts that actually landed. Call with IF=0 once the PIT
 * ticks. */
void cpu_simd_selftest(void);

#endif /* LOGIT_CPU_REPORT_H */
