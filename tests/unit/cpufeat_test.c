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

static void test_string(void)
{
    char buf[1024];
    int n = cpu_features_str(buf, (int)sizeof buf);
    ok(n == cpu_features_present_count(),
       "cpu_features_str writes every present feature");
    ok(strlen(buf) < sizeof buf, "feature string is NUL-terminated");
    printf("     %d/%d present: %s\n", n, CPU_FEAT_COUNT, buf);

    /* Truncation must not overrun and must not lie about the count. The
     * canary catches a one-past-the-end NUL, which a bounds check that
     * forgets the terminator writes every time. */
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

int main(void)
{
    printf("cpufeat_test: CPUID decode\n");
    test_basics();
    test_table();
    test_string();
    test_against_proc_cpuinfo();
    printf("\n%d checks, %d failed\n", checks, failures);
    printf("%s\n", failures ? "CPUFEAT TEST FAILED" : "CPUFEAT TEST PASSED");
    return failures ? 1 : 0;
}
