/* Boot-time CPU feature report + the preemption selftest. See cpu_report.h
 * for why this TU lives here rather than in c/kernel/cpu.
 *
 * The selftest exists because of one specific failure mode. The kernel runs
 * with SSE on and boot/isr.asm wraps every C interrupt handler in
 * FXSAVE/FXRSTOR. FXSAVE saves x87 state and XMM0-15 -- 512 bytes -- and
 * NOTHING ELSE. It does not save the upper halves of YMM registers. So the day
 * someone sets CR4.OSXSAVE and enables AVX in XCR0 without first migrating
 * isr.asm to XSAVE/XRSTOR, every interrupt silently truncates the top 128 bits
 * of every YMM register in the interrupted code. That bug appears only when an
 * interrupt lands inside a vectorised routine -- rarely, non-deterministically
 * -- and it corrupts data rather than crashing.
 *
 * AVX is therefore NOT enabled; see the comment block in boot/long.asm.
 * AES-NI and PCLMULQDQ are, because they use XMM0-15 only, which FXSAVE does
 * cover. That is the whole argument, and this selftest is what turns it from
 * an argument into evidence: it runs the accelerated crypto against the
 * portable reference with interrupts enabled, reports how many interrupts
 * actually landed during the run, and checks the XMM register file directly
 * across those interrupts. M15 set the precedent ("kernel + ring-3 double math
 * exact under heavy timer preemption, 0 corruptions"); the same bar applies.
 *
 * Negative control (to be run, not assumed): delete the fxsave/fxrstor pair
 * from isr_common in boot/isr.asm and this prints CPU_SIMD_SELFTEST_FAIL with
 * a nonzero xmm_corrupt count. */

#include <stdint.h>
#include "cpu_report.h"
#include "cpufeat.h"
#include "crypto.h"
#include "kprintf.h"
#include "serial.h"
#include "pit.h"

/* The report runs before kernel_main, so it goes out over the UART directly
 * rather than through kprintf (which also drives VGA and the klog ring,
 * neither of which exists yet). That costs a couple of formatting helpers;
 * it buys a feature module that no other subsystem has to remember to call. */
static void put(const char *s) { serial_puts(s); }

static void put_u(uint64_t v)
{
    char b[24];
    int i = 0;
    if (!v) { put("0"); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) { char c[2] = { b[i], 0 }; put(c); }
}

static void put_hex(uint64_t v)
{
    static const char d[] = "0123456789abcdef";
    char b[17];
    int i = 0;
    if (!v) { put("0"); return; }
    while (v) { b[i++] = d[v & 0xf]; v >>= 4; }
    while (i--) { char c[2] = { b[i], 0 }; put(c); }
}

/* ---- the AES-NI-versus-C question, answered on the machine ---------------
 *
 * aes_ni.c's header says any speedup "cannot be measured under QEMU/TCG, where
 * both instructions are C helper functions". That is a statement about why the
 * backend exists (constant time, not speed) -- it is not a reason to leave the
 * cost unknown, and it is exactly backwards as advice: TCG is the machine this
 * OS is actually run on, so if AESENC-through-a-TCG-helper were SLOWER than the
 * portable S-box, the dispatch would be choosing the slow path on every real
 * boot and nobody would know. crypto_simd_force_baseline() exists so the two
 * can be compared; this is the comparison, run on every boot, on identical
 * bytes, back to back, so the answer travels with the machine instead of living
 * in one agent's terminal.
 *
 * TSC, not the PIT: this runs before kernel_main, so there is no tick yet. A
 * raw cycle count is fine because the only claim made is a RATIO between two
 * measurements taken microseconds apart on the same core.
 *
 * Cost: one pass of 16 KiB through each backend. Under TCG that is roughly ten
 * milliseconds of a multi-second boot, which is a small price for a number that
 * would otherwise be a guess. */

static uint8_t bench_buf[16384];
static uint8_t bench_out[16384];

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t aes_pass(void)
{
    static const uint8_t key[16] = {
        0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81 };
    static const uint8_t iv[12] = {
        0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88 };
    uint8_t tag[16];
    uint64_t t0 = rdtsc();
    aes128_gcm_seal(key, iv, 0, 0, bench_buf, (int)sizeof bench_buf, bench_out, tag);
    return rdtsc() - t0;
}

static void aes_backend_bench(void)
{
    for (unsigned i = 0; i < sizeof bench_buf; i++) bench_buf[i] = (uint8_t)(i * 31 + 7);

    /* Whichever backend the dispatch chose, then the portable one, then put the
     * selection back exactly as it was -- this must not change what the rest of
     * the boot runs on. */
    const char *sel = crypto_simd_backend_name();
    uint64_t c_sel = aes_pass();
    crypto_simd_force_baseline(1);
    uint64_t c_ref = aes_pass();
    crypto_simd_force_baseline(0);

    put("[cpu] aes-gcm 16 KiB: "); put(sel); put("="); put_u(c_sel);
    put(" cycles, c="); put_u(c_ref); put(" cycles");
    if (c_sel) {
        /* Printed as hundredths so no floating point is needed on a path that
         * runs before the FPU state is anybody's business. */
        put(", speedup x"); put_u(c_ref * 100 / c_sel / 100);
        put(".");
        uint64_t frac = (c_ref * 100 / c_sel) % 100;
        if (frac < 10) put("0");
        put_u(frac);
    }
    put("\n");
}

void cpu_early_init(void)
{
    static int done;
    if (done) return;
    done = 1;

    cpu_features_init();
    crypto_simd_init();

    serial_init();                       /* idempotent; kernel_main repeats it */

    const struct cpu_features *c = cpu_features();
    static char list[640];
    int n = cpu_features_str(list, (int)sizeof list);

    put("[cpu] "); put(c->vendor);
    if (c->brand[0]) { put(" -- "); put(c->brand); }
    put("\n[cpu] "); put_u((uint64_t)n); put("/"); put_u((uint64_t)CPU_FEAT_COUNT);
    put(" known features present: "); put(list); put("\n");

    /* Print the XSAVE geometry even though we do not use it: it is exactly the
     * number a future AVX migration needs (how big the XSAVE area must be),
     * and printing it at boot is cheaper than someone guessing 512 or 832. */
    if (cpu_has(CPU_XSAVE)) {
        put("[cpu] xsave: "); put_u(c->xsave_max_size);
        put(" B holds every supported state, xcr0 mask 0x");
        put_hex(c->xcr0_supported);
        put(", osxsave="); put_u((uint64_t)cpu_has(CPU_OSXSAVE));
        put(" (AVX deliberately off -- see boot/long.asm)\n");
    } else {
        put("[cpu] xsave: not supported\n");
    }

    put("[cpu] aes-gcm backend: "); put(crypto_simd_backend_name());
    put(crypto_simd_constant_time() ? " (constant-time)"
                                    : " (table-driven, NOT constant-time)");
    put("; rdrand="); put_u((uint64_t)cpu_has(CPU_RDRAND));
    put(" rdseed="); put_u((uint64_t)cpu_has(CPU_RDSEED));
    put("\n");

    aes_backend_bench();
}

/* --- the XMM register-file probe ----------------------------------------
 * Load 16 known values into XMM0-15, spin with IF=1 long enough for timer
 * interrupts to land, then read all 16 back -- all inside ONE asm block, so no
 * compiler-generated code runs between the write and the read. Anything the
 * interrupt path fails to save/restore shows up as a mismatch.
 *
 * The pattern is per-register and per-byte, so no two registers hold the same
 * value: a save/restore that transposed two registers would otherwise pass.
 *
 * IF handling: called with IF=0 from the boot path; pushfq/popfq brackets the
 * sti so the caller's flags come back exactly as they were. */
static uint8_t probe_in[256];

static int xmm_probe(uint64_t spin, uint8_t out[256])
{
    for (int r = 0; r < 16; r++)
        for (int b = 0; b < 16; b++)
            probe_in[r * 16 + b] = (uint8_t)(0xA5 ^ (r * 16 + b) ^ (b << 3));

    const uint8_t *pi = probe_in;
    uint8_t *po = out;
    uint64_t cnt = spin;

    __asm__ volatile (
        "pushfq                     \n\t"
        "movdqu   0(%0), %%xmm0     \n\t"
        "movdqu  16(%0), %%xmm1     \n\t"
        "movdqu  32(%0), %%xmm2     \n\t"
        "movdqu  48(%0), %%xmm3     \n\t"
        "movdqu  64(%0), %%xmm4     \n\t"
        "movdqu  80(%0), %%xmm5     \n\t"
        "movdqu  96(%0), %%xmm6     \n\t"
        "movdqu 112(%0), %%xmm7     \n\t"
        "movdqu 128(%0), %%xmm8     \n\t"
        "movdqu 144(%0), %%xmm9     \n\t"
        "movdqu 160(%0), %%xmm10    \n\t"
        "movdqu 176(%0), %%xmm11    \n\t"
        "movdqu 192(%0), %%xmm12    \n\t"
        "movdqu 208(%0), %%xmm13    \n\t"
        "movdqu 224(%0), %%xmm14    \n\t"
        "movdqu 240(%0), %%xmm15    \n\t"
        "sti                        \n\t"
        "1: dec %2                  \n\t"
        "   jnz 1b                  \n\t"
        "cli                        \n\t"
        "movdqu %%xmm0,   0(%1)     \n\t"
        "movdqu %%xmm1,  16(%1)     \n\t"
        "movdqu %%xmm2,  32(%1)     \n\t"
        "movdqu %%xmm3,  48(%1)     \n\t"
        "movdqu %%xmm4,  64(%1)     \n\t"
        "movdqu %%xmm5,  80(%1)     \n\t"
        "movdqu %%xmm6,  96(%1)     \n\t"
        "movdqu %%xmm7, 112(%1)     \n\t"
        "movdqu %%xmm8, 128(%1)     \n\t"
        "movdqu %%xmm9, 144(%1)     \n\t"
        "movdqu %%xmm10,160(%1)     \n\t"
        "movdqu %%xmm11,176(%1)     \n\t"
        "movdqu %%xmm12,192(%1)     \n\t"
        "movdqu %%xmm13,208(%1)     \n\t"
        "movdqu %%xmm14,224(%1)     \n\t"
        "movdqu %%xmm15,240(%1)     \n\t"
        "popfq                      \n\t"
        : "+r"(pi), "+r"(po), "+r"(cnt)
        :
        : "cc", "memory",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");

    int bad = 0;
    for (int i = 0; i < 256; i++) if (out[i] != probe_in[i]) bad++;
    return bad;
}

/* Known-answer AES-128-GCM: McGrew-Viega / SP 800-38D test case 4 (the one
 * tests/unit/crypto_diff_gen.py also pins). Whichever backend is selected must
 * produce exactly this. Agreeing with the reference backend is necessary but
 * not sufficient -- two identically wrong backends would agree -- so the
 * selftest checks both directions. */
static const uint8_t kat_key[16] = {
    0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08 };
static const uint8_t kat_iv[12] = {
    0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88 };
static const uint8_t kat_aad[20] = {
    0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,
    0xab,0xad,0xda,0xd2 };
static const uint8_t kat_pt[60] = {
    0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,0xaf,0xf5,0x26,0x9a,
    0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,
    0x1c,0x3c,0x0c,0x95,0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
    0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39 };
static const uint8_t kat_ct[60] = {
    0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,0x84,0xd0,0xd4,0x9c,
    0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,
    0x21,0xd5,0x14,0xb2,0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
    0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91 };
static const uint8_t kat_tag[16] = {
    0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47 };

static int gcm_kat(void)
{
    uint8_t ct[60], tag[16], pt[60];
    aes128_gcm_seal(kat_key, kat_iv, kat_aad, 20, kat_pt, 60, ct, tag);
    for (int i = 0; i < 60; i++) if (ct[i] != kat_ct[i]) return 1;
    for (int i = 0; i < 16; i++) if (tag[i] != kat_tag[i]) return 2;
    if (aes128_gcm_open(kat_key, kat_iv, kat_aad, 20, kat_ct, 60, kat_tag, pt) != 0) return 3;
    for (int i = 0; i < 60; i++) if (pt[i] != kat_pt[i]) return 4;
    return 0;
}

void cpu_simd_selftest(void)
{
    static uint8_t probe_out[256];

    /* Both phases run for a TICK BUDGET, not an iteration count: the point of
     * the test is the number of interrupts that land inside it, and how many
     * iterations that takes differs by two orders of magnitude between TCG,
     * KVM and real hardware. A fixed iteration count would silently stop
     * exercising preemption the moment the machine got faster -- which is how
     * a preemption test quietly becomes a no-op. TICKS_WANTED at TIMER_HZ=100
     * is ~60 ms per phase; MIN_TICKS is what the verdict insists on. */
    const uint64_t TICKS_WANTED = 6, MIN_TICKS = 4;

    uint64_t spin = 500000ULL;
    uint64_t t0 = timer_ticks();
    int xmm_bad = xmm_probe(spin, probe_out);
    uint64_t ticks_probe = timer_ticks() - t0;
    for (int retry = 0; retry < 8 && ticks_probe < TICKS_WANTED; retry++) {
        spin *= 4;
        t0 = timer_ticks();
        xmm_bad += xmm_probe(spin, probe_out);
        ticks_probe = timer_ticks() - t0;
    }

    /* The functional half: hammer the selected backend against the portable
     * one, and against a published vector, with IF=1 throughout so timer
     * interrupts land inside the AES-NI instruction stream. */
    int mismatch = 0, kat_err = 0;
    unsigned rounds = 0;
    uint64_t t1 = timer_ticks();
    __asm__ volatile ("sti");
    while (timer_ticks() - t1 < TICKS_WANTED && rounds < 100000u) {
        int e = crypto_simd_selftest();
        if (e) { mismatch = e; break; }
        e = gcm_kat();
        if (e) { kat_err = e; break; }
        rounds++;
    }
    __asm__ volatile ("cli");
    uint64_t ticks_crypto = timer_ticks() - t1;

    kprintf("[cpu] simd selftest: backend=%s xmm_corrupt=%d (%u ticks) "
            "crypto_mismatch=%d kat=%d (%u rounds, %u ticks)\n",
            crypto_simd_backend_name(), xmm_bad, (unsigned)ticks_probe,
            mismatch, kat_err, rounds, (unsigned)ticks_crypto);

    if (ticks_probe < MIN_TICKS || ticks_crypto < MIN_TICKS)
        kprintf("[cpu] WARNING: too few interrupts landed -- preemption not exercised\n");

    if (!xmm_bad && !mismatch && !kat_err &&
        ticks_probe >= MIN_TICKS && ticks_crypto >= MIN_TICKS)
        kprintf("CPU_SIMD_SELFTEST_OK\n");
    else
        kprintf("CPU_SIMD_SELFTEST_FAIL\n");
}
