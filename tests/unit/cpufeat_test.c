/* Host unit test for c/kernel/cpu/cpufeat.c -- the CPUID decode.
 *
 * A feature-detection module is unusually easy to get wrong in a way nothing
 * notices: a mis-numbered bit reports a feature the CPU does not have (and the
 * dispatch then selects an implementation that #UDs) or hides one it does
 * (and the accelerated path silently never runs). Neither shows up in a
 * functional test of anything else.
 *
 * So this test uses an INDEPENDENT ORACLE where one exists: on Linux,
 * /proc/cpuinfo's flags line is the kernel's own decode of the same CPUID
 * bits, and every feature name below is cross-checked against it in BOTH
 * directions. On a host without /proc/cpuinfo the structural checks still run
 * and the cross-check reports itself skipped rather than passing quietly.
 *
 * THIS BINARY RUNS ON THE HOST, NOT ON THE TARGET, and until 2026-08-28 it
 * did not say so. On the host CLAUDE.md documents -- macOS / Apple Silicon --
 * cpufeat.c compiles its CPUID path away entirely (CPUFEAT_X86 in
 * c/kernel/cpu/cpufeat.c) because there is no CPUID instruction to execute,
 * so `cpu_features()` correctly reports is_x86 = 0 and 0/56 features. This
 * test then asserted `c->is_x86 == 1` and eight architectural baselines
 * against it and printed:
 *
 *     0/56 present:
 *     1622 checks, 12 failed
 *     CPUFEAT TEST FAILED
 *
 * Not one of those twelve was about the decode. `make test` reaches this
 * through `test: test-crypto` -> `test-cpufeat`, so the FIRST command in
 * CLAUDE.md has been red on its own documented host since 2026-08-07 for a
 * reason that has nothing to do with the code under test -- which is worse
 * than an absent gate, because it teaches a reader to walk past the word
 * FAIL. It skips loudly now, naming every property it did not check.
 *
 * The split is by what the property depends on, not by convenience: the name
 * table and the out-of-range refusals are pure data and run everywhere; the
 * bit tuples, the leaf gating, the XSAVE geometry, the truncation canary (an
 * empty feature list has nothing to truncate) and the /proc/cpuinfo oracle
 * all need a real x86 and are reported as NOT CHECKED.
 *
 * Build: see the `test-cpufeat` target in the Makefile. */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cpufeat.h"

static int checks, failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", what); }
}

/* --- structural checks --------------------------------------------------- */

static void test_table(void)
{
    /* Every id has a name, and no two ids share one. A duplicated name is the
     * signature of a copy-pasted table row that also copied the CPUID bit. */
    for (int i = 0; i < CPU_FEAT_COUNT; i++) {
        const char *n = cpu_feat_name((enum cpu_feat)i);
        if (!n || !n[0] || !strcmp(n, "?")) {
            printf("FAIL feature %d has no name\n", i);
            failures++;
        }
        checks++;
        for (int j = i + 1; j < CPU_FEAT_COUNT; j++) {
            if (!strcmp(n, cpu_feat_name((enum cpu_feat)j))) {
                printf("FAIL features %d and %d share the name '%s'\n", i, j, n);
                failures++;
            }
            checks++;
        }
    }

    ok(!strcmp(cpu_feat_name((enum cpu_feat)-1), "?"), "negative id -> \"?\"");
    ok(!strcmp(cpu_feat_name((enum cpu_feat)CPU_FEAT_COUNT), "?"), "past-end id -> \"?\"");
    ok(cpu_has((enum cpu_feat)-1) == 0, "negative id -> not present");
    ok(cpu_has((enum cpu_feat)CPU_FEAT_COUNT) == 0, "past-end id -> not present");
}

static void test_basics(void)
{
    const struct cpu_features *c = cpu_features();
    ok(c != NULL, "cpu_features() non-NULL");
    ok(c->valid, "cpu_features() initialises on first use");
    ok(c->is_x86 == 1, "host build is x86 (this test is only meaningful there)");
    ok(c->max_leaf >= 1, "CPUID max basic leaf >= 1");
    ok(strlen(c->vendor) == 12, "vendor string is 12 chars");
    printf("     vendor=%s max_leaf=%u max_ext_leaf=%#x\n",
           c->vendor, c->max_leaf, c->max_ext_leaf);
    printf("     brand=%s\n", c->brand[0] ? c->brand : "(none)");

    /* Anything running this binary is x86-64, so these are architecturally
     * guaranteed. If the bit decode is off by one, at least one of them
     * flips. */
    ok(cpu_has(CPU_FPU), "x86-64 baseline: fpu");
    ok(cpu_has(CPU_TSC), "x86-64 baseline: tsc");
    ok(cpu_has(CPU_MSR), "x86-64 baseline: msr");
    ok(cpu_has(CPU_CMOV), "x86-64 baseline: cmov");
    ok(cpu_has(CPU_MMX), "x86-64 baseline: mmx");
    ok(cpu_has(CPU_FXSR), "x86-64 baseline: fxsr");
    ok(cpu_has(CPU_SSE), "x86-64 baseline: sse");
    ok(cpu_has(CPU_SSE2), "x86-64 baseline: sse2");
    ok(cpu_has(CPU_LM), "x86-64 baseline: long mode (leaf 0x80000001)");

    /* Implications that hold on every real CPU. These catch a decode that
     * reports a superset feature without its prerequisite -- e.g. reading
     * leaf 7 on a CPU whose max_leaf is 6 and getting leaf 6's registers. */
    if (cpu_has(CPU_AVX2))  ok(cpu_has(CPU_AVX), "avx2 implies avx");
    if (cpu_has(CPU_AVX))   ok(cpu_has(CPU_XSAVE), "avx implies xsave");
    if (cpu_has(CPU_AVX))   ok(cpu_has(CPU_SSE42), "avx implies sse4.2");
    if (cpu_has(CPU_SSE42)) ok(cpu_has(CPU_SSE41), "sse4.2 implies sse4.1");
    if (cpu_has(CPU_SSE41)) ok(cpu_has(CPU_SSSE3), "sse4.1 implies ssse3");
    if (cpu_has(CPU_SSSE3)) ok(cpu_has(CPU_SSE3), "ssse3 implies sse3");
    if (cpu_has(CPU_AES))   ok(cpu_has(CPU_SSE2), "aes-ni implies sse2");
    if (cpu_has(CPU_VAES))  ok(cpu_has(CPU_AES), "vaes implies aes");

    /* XSAVE geometry: only meaningful when XSAVE exists, and then the area
     * must be at least the 512-byte legacy region plus the 64-byte header.
     * This is the number a future AVX/XSAVE migration would size its save
     * area from, so a zero here is a silent trap, not a cosmetic gap. */
    if (cpu_has(CPU_XSAVE)) {
        ok(c->xsave_max_size >= 576, "xsave area >= 576 B when xsave present");
        ok((c->xcr0_supported & 1) != 0, "xcr0 bit 0 (x87) always supported");
        ok((c->xcr0_supported & 2) != 0, "xcr0 bit 1 (SSE) supported");
        printf("     xsave_max=%u xsave_enabled=%u xcr0=%#llx\n",
               c->xsave_max_size, c->xsave_enabled_size,
               (unsigned long long)c->xcr0_supported);
    } else {
        ok(c->xsave_max_size == 0, "no xsave -> geometry reported as zero, not guessed");
    }
}

static void test_string(int x86)
{
    char buf[1024];
    int n = cpu_features_str(buf, (int)sizeof buf);
    ok(n == cpu_features_present_count(),
       "cpu_features_str writes every present feature");
    ok(strlen(buf) < sizeof buf, "feature string is NUL-terminated");
    printf("     %d/%d present: %s\n", n, CPU_FEAT_COUNT, buf);

    /* Truncation must not overrun and must not lie about the count. The
     * canary catches a one-past-the-end NUL, which a bounds check that
     * forgets the terminator writes every time.
     *
     * Only meaningful where features are actually present: off x86 the list
     * is empty, cpu_features_str writes one NUL into a 24-byte buffer, and
     * all three assertions below pass without the truncation path having been
     * entered at all. Running them there would be a control that cannot fail
     * -- three green lines that say nothing, which is exactly the shape
     * tests/audit_tests.py's MUTE category exists to find. */
    if (x86) {
        char small[24];
        char guard[8];
        memset(guard, 0x5A, sizeof guard);
        char *heap = malloc(sizeof small + sizeof guard);
        memset(heap, 0x5A, sizeof small + sizeof guard);
        int m = cpu_features_str(heap, (int)sizeof small);
        ok(strlen(heap) < sizeof small, "truncated string stays inside the buffer");
        ok(memcmp(heap + sizeof small, guard, sizeof guard) == 0,
           "cpu_features_str does not write past the buffer");
        ok(m <= n, "truncated string reports fewer features, not more");
        free(heap);
    } else {
        printf("SKIP truncation canary: 0 features present, so a 24-byte buffer\n"
               "     never truncates and the three assertions cannot fail\n");
    }

    ok(cpu_features_str(NULL, 100) == 0, "NULL buffer is refused");
    ok(cpu_features_str(buf, 0) == 0, "zero-length buffer is refused");
}

/* --- the independent oracle: /proc/cpuinfo ------------------------------- */

struct namemap { enum cpu_feat id; const char *proc_name; };

/* Only features whose Linux flag name is unambiguous. Ones Linux spells
 * differently for historical reasons (prefetchw -> "3dnowprefetch",
 * lzcnt -> "abm") are still listed, with the Linux spelling. Deliberately
 * omitted: osxsave (Linux reports "xsave"/"osxsave" inconsistently across
 * versions) and hypervisor (present but not always exported). */
static const struct namemap procmap[] = {
    { CPU_FPU, "fpu" }, { CPU_TSC, "tsc" }, { CPU_MSR, "msr" },
    { CPU_CMOV, "cmov" }, { CPU_CLFLUSH, "clflush" }, { CPU_MMX, "mmx" },
    { CPU_FXSR, "fxsr" }, { CPU_SSE, "sse" }, { CPU_SSE2, "sse2" },
    { CPU_SSE3, "pni" }, { CPU_PCLMULQDQ, "pclmulqdq" },
    { CPU_MONITOR, "monitor" }, { CPU_SSSE3, "ssse3" }, { CPU_FMA, "fma" },
    { CPU_CX16, "cx16" }, { CPU_SSE41, "sse4_1" }, { CPU_SSE42, "sse4_2" },
    { CPU_X2APIC, "x2apic" }, { CPU_MOVBE, "movbe" }, { CPU_POPCNT, "popcnt" },
    { CPU_AES, "aes" }, { CPU_XSAVE, "xsave" }, { CPU_AVX, "avx" },
    { CPU_F16C, "f16c" }, { CPU_RDRAND, "rdrand" },
    { CPU_FSGSBASE, "fsgsbase" }, { CPU_BMI1, "bmi1" }, { CPU_AVX2, "avx2" },
    { CPU_SMEP, "smep" }, { CPU_BMI2, "bmi2" }, { CPU_ERMS, "erms" },
    { CPU_INVPCID, "invpcid" }, { CPU_AVX512F, "avx512f" },
    { CPU_AVX512DQ, "avx512dq" }, { CPU_RDSEED, "rdseed" },
    { CPU_ADX, "adx" }, { CPU_SMAP, "smap" },
    { CPU_CLFLUSHOPT, "clflushopt" }, { CPU_SHA, "sha_ni" },
    { CPU_AVX512BW, "avx512bw" }, { CPU_AVX512VL, "avx512vl" },
    { CPU_AVX512VBMI, "avx512vbmi" }, { CPU_UMIP, "umip" }, { CPU_PKU, "pku" },
    { CPU_VAES, "vaes" }, { CPU_VPCLMULQDQ, "vpclmulqdq" },
    { CPU_RDPID, "rdpid" },
    { CPU_LZCNT, "abm" }, { CPU_PREFETCHW, "3dnowprefetch" },
    { CPU_NX, "nx" }, { CPU_PDPE1GB, "pdpe1gb" }, { CPU_RDTSCP, "rdtscp" },
    { CPU_LM, "lm" },
};

static char *read_flags_line(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return NULL;
    static char line[8192];
    char *found = NULL;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "flags", 5)) {
            char *colon = strchr(line, ':');
            if (colon) found = colon + 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* whole-word search in a space-separated list */
static int has_flag(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; (p = strstr(p, name)) != NULL; p += n) {
        int left_ok  = (p == list) || p[-1] == ' ' || p[-1] == '\t';
        int right_ok = p[n] == ' ' || p[n] == '\t' || p[n] == '\n' || p[n] == 0;
        if (left_ok && right_ok) return 1;
    }
    return 0;
}

static void test_against_proc_cpuinfo(void)
{
    char *flags = read_flags_line();
    if (!flags) {
        printf("SKIP /proc/cpuinfo cross-check (no flags line on this host)\n");
        return;
    }
    int agree = 0, disagree = 0;
    for (unsigned i = 0; i < sizeof procmap / sizeof procmap[0]; i++) {
        int ours = cpu_has(procmap[i].id);
        int theirs = has_flag(flags, procmap[i].proc_name);
        checks++;
        if (ours != theirs) {
            failures++; disagree++;
            printf("FAIL %s: cpufeat.c says %d, /proc/cpuinfo says %d (flag '%s')\n",
                   cpu_feat_name(procmap[i].id), ours, theirs, procmap[i].proc_name);
        } else {
            agree++;
        }
    }
    printf("     /proc/cpuinfo cross-check: %d agree, %d disagree\n", agree, disagree);
}

/* --- the host-capability gate -------------------------------------------- */

/* Named by the module itself rather than by a #ifdef here: cpufeat.c decides
 * whether it has a CPUID path (CPUFEAT_X86) and reports the answer in is_x86,
 * so this asks the code under test instead of re-deriving the predicate beside
 * it. Two copies of "is this x86" is how one of them ends up wrong. */
static int host_is_x86(void)
{
    const struct cpu_features *c = cpu_features();
    return c && c->is_x86;
}

static void skip_loudly(void)
{
    printf("SKIP cpufeat: this host is not x86, so there is no CPUID to decode.\n");
    printf("     c/kernel/cpu/cpufeat.c compiles its whole detection path out off\n");
    printf("     x86 (CPUFEAT_X86), and correctly reports is_x86=0, 0/%d features.\n",
           CPU_FEAT_COUNT);
    printf("     NOT CHECKED, and nothing below should be read as covering them:\n");
    printf("       - every feature's (leaf, subleaf, register, bit) tuple\n");
    printf("       - the leaf-availability gating on max_leaf / max_ext_leaf\n");
    printf("       - the vendor and brand strings\n");
    printf("       - the XSAVE area geometry and xcr0 bits\n");
    printf("       - the x86-64 baseline set and the feature implications\n");
    printf("       - the /proc/cpuinfo cross-check (its independent oracle)\n");
    printf("     STILL CHECKED below: the name table (every id named, no two\n");
    printf("     ids sharing a name) and the out-of-range refusals -- pure data,\n");
    printf("     host-independent, and where a copy-pasted table row shows up.\n");
    printf("     Settle the rest on an x86-64 host (a Linux box is what supplies\n");
    printf("     the oracle) with: make test-cpufeat\n");
    printf("     NOT with `clang -arch x86_64` under Rosetta 2: measured\n");
    printf("     2026-08-28, that build runs and reports vendor=GenuineIntel,\n");
    printf("     max_leaf=13, 20/%d features -- and fails `x86-64 baseline: msr`,\n",
           CPU_FEAT_COUNT);
    printf("     because Rosetta's synthetic CPUID clears the MSR bit it has no\n");
    printf("     MSRs to back. An emulator's CPUID is not an independent oracle.\n");
}

int main(void)
{
    printf("cpufeat_test: CPUID decode\n");

    int x86 = host_is_x86();
    if (x86) {
        test_basics();
    } else {
        skip_loudly();
    }
    test_table();
    test_string(x86);
    if (x86) test_against_proc_cpuinfo();

    printf("\n%d checks, %d failed\n", checks, failures);
    if (failures)
        printf("CPUFEAT TEST FAILED\n");
    else if (x86)
        printf("CPUFEAT TEST PASSED\n");
    else
        printf("CPUFEAT TEST SKIPPED (non-x86 host): table checks only, "
               "the CPUID decode is UNMEASURED here\n");
    return failures ? 1 : 0;
}
