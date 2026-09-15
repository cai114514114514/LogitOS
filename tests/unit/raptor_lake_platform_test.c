/* Deterministic per-core CPUID fixtures for Core i7-14700KF.  The values are
 * the architectural shapes that matter to the OS: B0671, 28 package threads,
 * sparse seven-bit topology IDs, 40H P-cores, 20H E-cores, and different
 * cache geometry on each core type.  This does not claim a physical boot. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "cpu_platform.h"

static int checks, failures;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("FAIL: %s\n", what); }
}

struct fixture {
    uint32_t model;
    uint32_t stepping;
    uint32_t apic_id;
    uint32_t core_type;
    int ecore;
    int hybrid;
    int malformed_leaf_1f;
    int unknown_level_type;
    int duplicate_smt_level;
    int duplicate_core_level;
    int repeated_level_number;
    int equal_level_shift;
    int decreasing_level_shift;
    int missing_smt_level;
    int missing_core_level;
    int zero_count_level;
    int no_leaf_1f;
    int malformed_leaf_b;
    int no_leaf_1a;
    int no_leaf_6;
    int no_hfi;
    int no_thread_director;
    int non_intel;
    int osxsave;
    int no_fxsave;
    int no_tsc;
    int no_invariant;
    int leaf_1f_calls;
    int leaf_b_calls;
    int leaf_6_calls;
    int rdmsr_calls;
};

static uint32_t signature(uint32_t model, uint32_t stepping)
{
    return (6u << 8) | ((model & 0xfu) << 4) |
           ((model >> 4) << 16) | (stepping & 0xfu);
}

static uint32_t cache_eax(uint32_t type, uint32_t level, uint32_t sharing)
{
    /* The 64-core field mirrors B0671 hardware but is deliberately ignored for
     * hybrid physical-core counts: it is an addressability maximum, not 20. */
    return type | (level << 5) | ((sharing - 1u) << 14) | (63u << 26);
}

static uint32_t cache_ebx(uint32_t line, uint32_t partitions, uint32_t ways)
{
    return (line - 1u) | ((partitions - 1u) << 12) |
           ((ways - 1u) << 22);
}

static void cache_leaf(struct fixture *f, uint32_t sub, uint32_t out[4])
{
    uint32_t type = 0, level = 0, sharing = 1, ways = 1, sets = 1;
    if (sub == 0) {
        type = 1; level = 1; sharing = 2;
        ways = f->ecore ? 8 : 12;
        sets = 64;
    } else if (sub == 1) {
        type = 2; level = 1; sharing = 2; ways = 8;
        sets = f->ecore ? 128 : 64;
    } else if (sub == 2) {
        type = 3; level = 2; sharing = 8; ways = 16;
        sets = f->ecore ? 4096 : 2048;
    } else if (sub == 3) {
        type = 3; level = 3; sharing = 128; ways = 11; sets = 49152;
    } else {
        return;
    }
    out[0] = cache_eax(type, level, sharing);
    out[1] = cache_ebx(64, 1, ways);
    out[2] = sets - 1u;
}

static void fake_cpuid(void *opaque, uint32_t leaf, uint32_t sub,
                       uint32_t out[4])
{
    struct fixture *f = opaque;
    out[0] = out[1] = out[2] = out[3] = 0;
    switch (leaf) {
    case 0:
        out[0] = f->no_leaf_6 ? 5u : f->no_leaf_1f ? 0x1eu : 0x1fu;
        if (f->non_intel) {
            out[1] = 0x68747541u; out[3] = 0x69746e65u; out[2] = 0x444d4163u;
        } else {
            out[1] = 0x756e6547u; out[3] = 0x49656e69u; out[2] = 0x6c65746eu;
        }
        break;
    case 1:
        out[0] = signature(f->model, f->stepping);
        out[1] = (28u << 16) | ((f->apic_id & 0xffu) << 24);
        out[2] = (1u << 21) | (1u << 26) | (1u << 28);
        if (f->osxsave) out[2] |= 1u << 27;
        out[3] = (1u << 5) | (1u << 7) | (1u << 9) | (1u << 14) |
                 (1u << 28);
        if (!f->no_tsc) out[3] |= 1u << 4;
        if (!f->no_fxsave) out[3] |= 1u << 24;
        break;
    case 4:
        cache_leaf(f, sub, out);
        break;
    case 6:
        f->leaf_6_calls++;
        if (!f->no_hfi) out[0] |= 1u << 19;
        if (!f->no_thread_director) out[0] |= 1u << 23;
        /* Deliberately nontrivial synthetic geometry catches field-width and
         * +1 errors.  It is architectural fixture data, not a physical dump. */
        out[2] = 4u << 8;
        out[3] = 3u | (2u << 8) | ((f->ecore ? 0x92u : 0x31u) << 16);
        break;
    case 7:
        if (f->hybrid) out[3] = 1u << 15;
        break;
    case 0x0b:
        f->leaf_b_calls++;
        if (f->malformed_leaf_b) break;
        if (sub == 0) {
            out[0] = f->ecore ? 0u : 1u;
            out[1] = f->ecore ? 1u : 2u;
            out[2] = 1u << 8;
            out[3] = f->apic_id ^ 0x80u; /* detect an incorrect 0BH preference */
        } else if (sub == 1) {
            out[0] = 7; out[1] = 28; out[2] = (2u << 8) | 1u;
            out[3] = f->apic_id ^ 0x80u;
        }
        break;
    case 0x1a:
        if (!f->no_leaf_1a)
            out[0] = (f->core_type << 24) | 1u;
        break;
    case 0x1f:
        f->leaf_1f_calls++;
        if (f->malformed_leaf_1f) break;
        if (sub == 0) {
            out[0] = f->ecore ? 0u : 1u;
            out[1] = f->ecore ? 1u : 2u;
            out[2] = (f->missing_smt_level ? 3u : 1u) << 8;
            out[3] = f->apic_id;
        } else if (sub == 1) {
            out[0] = f->equal_level_shift ? 1u :
                     f->decreasing_level_shift ? 0u : 7u;
            out[1] = f->zero_count_level ? 0u :
                     f->equal_level_shift ? 2u :
                     f->decreasing_level_shift ? 1u : 28u;
            out[2] = ((f->duplicate_smt_level ? 1u :
                       f->missing_core_level ? 3u : 2u) << 8) |
                     (f->repeated_level_number ? 0u : 1u);
            out[3] = f->apic_id;
        } else if (sub == 2 && (f->unknown_level_type ||
                                f->duplicate_core_level ||
                                f->duplicate_smt_level)) {
            out[0] = 8; out[1] = 28;
            out[2] = ((f->unknown_level_type ? 7u : 2u) << 8) | 2u;
            out[3] = f->apic_id;
        }
        break;
    case 0x80000000u:
        out[0] = 0x80000007u;
        break;
    case 0x80000007u:
        if (!f->no_invariant) out[3] = 1u << 8;
        break;
    }
}

static int fake_rdmsr(void *opaque, uint32_t msr, uint64_t *value)
{
    struct fixture *f = opaque;
    f->rdmsr_calls++;
    if (msr == 0x1bu) {
        *value = 0xfee00000ull | (1ull << 11) | (1ull << 10);
        return 0;
    }
    if (msr == 0x179u) {
        *value = 20u | (1ull << 8) | (1ull << 10) | (1ull << 24);
        return 0;
    }
    return -1;
}

static struct cpu_platform_info run_fixture(struct fixture *f)
{
    const struct cpu_platform_ops ops = { fake_cpuid, fake_rdmsr, f };
    struct cpu_platform_info out;
    ck(cpu_platform_decode(&ops, &out) == 0, "14700KF fixture decodes");
    return out;
}

static struct fixture pcore_fixture(void)
{
    return (struct fixture){
        .model = 0xb7, .stepping = 1, .apic_id = 0x11,
        .core_type = CPU_PLATFORM_CORE_CORE, .hybrid = 1,
    };
}

static struct fixture ecore_fixture(void)
{
    return (struct fixture){
        .model = 0xb7, .stepping = 1, .apic_id = 0x40,
        .core_type = CPU_PLATFORM_CORE_ATOM, .ecore = 1, .hybrid = 1,
    };
}

static void test_pcore(void)
{
    struct fixture f = pcore_fixture();
    struct cpu_platform_info p = run_fixture(&f);
    ck(p.valid && !strcmp(p.vendor, "GenuineIntel"), "Intel platform identity");
    ck(p.family == 6 && p.model == 0xb7 && p.stepping == 1,
       "B0671 display family/model/stepping");
    ck(p.generation == CPU_PLATFORM_RAPTOR_LAKE_S,
       "B0671 maps to Raptor Lake-S/Refresh");
    ck(!strcmp(cpu_platform_generation_name(p.generation), "raptor-lake-s"),
       "Raptor Lake-S diagnostic name");

    ck(p.current_core.valid && p.current_core.hybrid &&
       p.current_core.native_model_valid, "hybrid native-model leaf retained");
    ck(p.current_core.type == CPU_PLATFORM_CORE_CORE &&
       p.current_core.native_model_id == 1,
       "CPUID.1A 40H identifies the current P-core");
    ck(!strcmp(cpu_platform_core_type_name(p.current_core.type),
               "intel-core/p-core"), "P-core diagnostic name");

    ck(p.topology.leaf_1f && !p.topology.leaf_b,
       "CPUID.1F is preferred over legacy CPUID.0B");
    ck(p.topology.logical_per_package == 28 &&
       p.topology.threads_per_core == 2,
       "P-core topology retains 28 package threads and two siblings");
    ck(p.topology.heterogeneous_smt && !p.topology.core_count_exact &&
       p.topology.cores_per_package == 0,
       "hybrid topology refuses uniform-SMT physical core inference");
    ck(p.topology.smt_shift == 1 && p.topology.package_shift == 7,
       "sparse Raptor APIC topology keeps architectural shifts");
    ck(p.topology.x2apic_id == 0x11 && p.topology.package_id == 0 &&
       p.topology.core_id == 8 && p.topology.thread_id == 1,
       "P-core x2APIC id splits without assuming dense ids");
    ck(f.leaf_1f_calls == 3 && f.leaf_b_calls == 0,
       "valid CPUID.1F does not consult CPUID.0B");

    const struct cpu_platform_hfi *h = &p.current_core.hfi;
    ck(h->leaf_6 && h->hfi_capable && h->thread_director_capable,
       "CPUID.6 capability bits expose HFI and Thread Director inventory");
    ck(h->capability_bitmap == 3 && h->performance_capable &&
       h->energy_efficiency_capable,
       "HFI performance and efficiency capability bitmap retained");
    ck(h->table_pages == 3 && h->table_index == 0x31 &&
       h->thread_director_classes == 4,
       "HFI page count, current-LP row and class geometry decoded exactly");
    ck(f.leaf_6_calls == 1, "full platform decode samples CPUID.6 once");

    ck(p.cache_count == 4, "four P-core cache leaves retained");
    ck(p.cache[0].level == 1 && p.cache[0].type == 1 &&
       p.cache[0].size_bytes == 48u * 1024u && p.cache[0].ways == 12,
       "P-core 48 KiB L1 data geometry");
    ck(p.cache[1].level == 1 && p.cache[1].type == 2 &&
       p.cache[1].size_bytes == 32u * 1024u,
       "P-core 32 KiB L1 instruction geometry");
    ck(p.cache[2].level == 2 &&
       p.cache[2].size_bytes == 2u * 1024u * 1024u,
       "P-core 2 MiB L2 geometry");
    ck(p.cache[3].level == 3 && p.cache[3].ways == 11 &&
       p.cache[3].size_bytes == 33ull * 1024 * 1024 &&
       p.cache[3].shared_logical == 128,
       "33 MiB L3 geometry and addressable-sharing count");

    ck(p.tsc_capable && p.invariant_tsc,
       "TSC presence gates invariant-TSC reporting");
    ck(p.fxsave_capable && p.xsave_capable && p.avx_capable &&
       !p.osxsave_active, "14700KF xstate hardware with OSXSAVE kept clear");
    ck(p.xstate_abi == CPU_PLATFORM_XSTATE_FXSAVE &&
       p.xstate_contract_safe, "interrupt ABI remains safe FXSAVE-only");
    ck(p.apic_base_valid && p.apic_enabled && p.x2apic_active,
       "firmware x2APIC mode retained");
    ck(p.mca.cap_valid && p.mca.banks == 20,
       "machine-check bank geometry retained");
    ck(f.rdmsr_calls == 2, "only APIC_BASE and MCG_CAP MSRs read");
}

static void test_ecore_and_per_cpu_api(void)
{
    struct fixture f = ecore_fixture();
    struct cpu_platform_info p = run_fixture(&f);
    ck(p.current_core.type == CPU_PLATFORM_CORE_ATOM,
       "CPUID.1A 20H identifies the current E-core");
    ck(!strcmp(cpu_platform_core_type_name(p.current_core.type),
               "intel-atom/e-core"), "E-core diagnostic name");
    ck(p.topology.logical_per_package == 28 &&
       p.topology.threads_per_core == 1 && p.topology.smt_shift == 0,
       "E-core reports one thread inside the same 28-thread package");
    ck(p.topology.x2apic_id == 0x40 && p.topology.core_id == 0x40 &&
       p.topology.thread_id == 0, "E-core sparse x2APIC id retained");
    ck(p.cache_count == 4 && p.cache[0].size_bytes == 32u * 1024u &&
       p.cache[1].size_bytes == 64u * 1024u,
       "E-core L1 data/instruction geometry");
    ck(p.cache[2].size_bytes == 4u * 1024u * 1024u,
       "four-E-core cluster 4 MiB L2 geometry");

    const struct cpu_platform_ops ops = { fake_cpuid, 0, &f };
    struct cpu_platform_core_info core;
    ck(cpu_platform_decode_current_core(&ops, &core) == 0 && core.valid,
       "per-CPU callback API decodes without privileged MSRs");
    ck(core.type == CPU_PLATFORM_CORE_ATOM && core.topology.x2apic_id == 0x40,
       "per-CPU callback returns E-core type and topology");
    ck(core.hfi.hfi_capable && core.hfi.table_index == 0x92,
       "per-CPU callback samples this E-core logical processor's HFI row");
}

static void test_fallbacks_and_guards(void)
{
    struct fixture f = pcore_fixture();
    f.malformed_leaf_1f = 1;
    struct cpu_platform_info p = run_fixture(&f);
    ck(!p.topology.leaf_1f && p.topology.leaf_b,
       "null CPUID.1F falls back to CPUID.0B");
    ck(p.topology.x2apic_id == (0x11u ^ 0x80u),
       "fallback consumes the CPUID.0B x2APIC id");
    ck(f.leaf_1f_calls == 1 && f.leaf_b_calls == 3,
       "fallback probes one null 1F level then complete 0B topology");

    f = pcore_fixture();
    f.no_leaf_1f = 1;
    p = run_fixture(&f);
    ck(p.topology.leaf_b && !p.topology.leaf_1f && f.leaf_1f_calls == 0,
       "MAXLEAF below 1F uses CPUID.0B directly");

    f = pcore_fixture();
    f.malformed_leaf_1f = 1;
    f.malformed_leaf_b = 1;
    p = run_fixture(&f);
    ck(!p.topology.valid && !p.topology.leaf_1f && !p.topology.leaf_b &&
       p.topology.cores_per_package == 0 && p.topology.x2apic_id == 0,
       "hybrid CPU without extended topology refuses legacy core identity");
    ck(p.current_core.native_model_valid &&
       p.current_core.type == CPU_PLATFORM_CORE_CORE,
       "missing topology does not discard independent P/E core typing");

    f = pcore_fixture();
    f.no_hfi = 1;
    p = run_fixture(&f);
    ck(p.current_core.hfi.leaf_6 && !p.current_core.hfi.hfi_capable &&
       !p.current_core.hfi.thread_director_capable &&
       p.current_core.hfi.capability_bitmap == 0 &&
       p.current_core.hfi.table_pages == 0 &&
       p.current_core.hfi.table_index == 0 &&
       p.current_core.hfi.thread_director_classes == 0,
       "absent HFI capability gates stray Thread Director geometry");

    f = pcore_fixture();
    f.no_thread_director = 1;
    p = run_fixture(&f);
    ck(p.current_core.hfi.hfi_capable &&
       !p.current_core.hfi.thread_director_capable &&
       p.current_core.hfi.thread_director_classes == 0 &&
       p.current_core.hfi.table_pages == 3,
       "HFI remains enumerable while Thread Director classes stay gated");

    f = pcore_fixture();
    f.no_leaf_6 = 1;
    p = run_fixture(&f);
    ck(!p.current_core.hfi.leaf_6 && !p.current_core.hfi.hfi_capable &&
       f.leaf_6_calls == 0,
       "MAXLEAF below 6 never queries or invents HFI");

    f = pcore_fixture();
    f.no_leaf_1a = 1;
    p = run_fixture(&f);
    ck(p.current_core.hybrid && !p.current_core.native_model_valid &&
       p.current_core.type == CPU_PLATFORM_CORE_UNKNOWN,
       "zero CPUID.1A answer is absent rather than fabricated");

    f = pcore_fixture();
    f.non_intel = 1;
    p = run_fixture(&f);
    ck(p.generation == CPU_PLATFORM_OTHER &&
       !p.current_core.native_model_valid && !p.current_core.hfi.leaf_6 &&
       f.leaf_6_calls == 0,
       "Intel model, core type and HFI inventory require GenuineIntel");

    f = pcore_fixture();
    f.osxsave = 1;
    p = run_fixture(&f);
    ck(p.osxsave_active && p.xstate_abi == CPU_PLATFORM_XSTATE_FXSAVE &&
       !p.xstate_contract_safe,
       "inherited OSXSAVE is diagnosed against the FXSAVE interrupt ABI");

    f = pcore_fixture();
    f.no_fxsave = 1;
    p = run_fixture(&f);
    ck(!p.fxsave_capable && !p.xstate_contract_safe,
       "missing FXSAVE capability cannot satisfy the kernel xstate ABI");

    f = pcore_fixture();
    f.no_tsc = 1;
    p = run_fixture(&f);
    ck(!p.tsc_capable && !p.invariant_tsc,
       "invariant bit cannot create a TSC capability");

    f = pcore_fixture();
    f.no_invariant = 1;
    p = run_fixture(&f);
    ck(p.tsc_capable && !p.invariant_tsc,
       "non-invariant TSC remains distinguishable");

    f = pcore_fixture();
    f.unknown_level_type = 1;
    f.apic_id = 0x111;
    p = run_fixture(&f);
    ck(p.topology.leaf_1f && p.topology.package_shift == 8 &&
       p.topology.package_id == 1 && p.topology.core_id == 8,
       "unknown nonzero domain extends the composite core-ID boundary");

    f = pcore_fixture();
    f.equal_level_shift = 1;
    f.apic_id = 1;
    p = run_fixture(&f);
    ck(p.topology.leaf_1f && p.topology.smt_shift == 1 &&
       p.topology.package_shift == 1 && p.topology.logical_per_package == 2,
       "equal adjacent shifts remain valid for a one-entity domain");

    const struct {
        size_t offset;
        const char *name;
    } malformed[] = {
        { offsetof(struct fixture, duplicate_smt_level),
          "duplicate CPUID.1F SMT level falls back" },
        { offsetof(struct fixture, duplicate_core_level),
          "duplicate CPUID.1F Core level falls back" },
        { offsetof(struct fixture, repeated_level_number),
          "incorrect CPUID.1F level index falls back" },
        { offsetof(struct fixture, decreasing_level_shift),
          "decreasing CPUID.1F shift falls back" },
        { offsetof(struct fixture, missing_smt_level),
          "missing CPUID.1F SMT domain falls back" },
        { offsetof(struct fixture, missing_core_level),
          "missing CPUID.1F Core domain falls back" },
        { offsetof(struct fixture, zero_count_level),
          "nonzero CPUID.1F type with zero count falls back" },
    };
    for (unsigned i = 0; i < sizeof malformed / sizeof malformed[0]; i++) {
        f = pcore_fixture();
        *(int *)((unsigned char *)&f + malformed[i].offset) = 1;
        p = run_fixture(&f);
        ck(!p.topology.leaf_1f && p.topology.leaf_b,
           malformed[i].name);
    }
}

static void test_official_model_ranges(void)
{
    const struct { uint32_t model, stepping; int accepted; } cases[] = {
        { 0xb7, 1, 1 }, { 0xb7, 0, 0 }, { 0xb7, 2, 0 },
        { 0xbf, 2, 1 }, { 0xbf, 5, 1 }, { 0xbf, 1, 0 }, { 0xbf, 3, 0 },
        { 0xba, 2, 0 },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct fixture f = pcore_fixture();
        f.model = cases[i].model;
        f.stepping = cases[i].stepping;
        struct cpu_platform_info p = run_fixture(&f);
        ck((p.generation == CPU_PLATFORM_RAPTOR_LAKE_S) == cases[i].accepted,
           "Raptor Lake-S generation follows documented model/stepping range");
    }
}

static void test_argument_guards(void)
{
    struct fixture f = pcore_fixture();
    const struct cpu_platform_ops good = { fake_cpuid, 0, &f };
    const struct cpu_platform_ops bad = { 0, 0, &f };
    struct cpu_platform_info p;
    struct cpu_platform_core_info core;
    ck(cpu_platform_decode(0, &p) == -1, "null platform ops rejected");
    ck(cpu_platform_decode(&bad, &p) == -1, "null CPUID callback rejected");
    ck(cpu_platform_decode(&good, 0) == -1, "null platform output rejected");
    ck(cpu_platform_decode_current_core(0, &core) == -1,
       "null per-CPU ops rejected");
    ck(cpu_platform_decode_current_core(&bad, &core) == -1,
       "null per-CPU CPUID callback rejected");
    ck(cpu_platform_decode_current_core(&good, 0) == -1,
       "null per-CPU output rejected");
}

int main(void)
{
    test_pcore();
    test_ecore_and_per_cpu_api();
    test_fallbacks_and_guards();
    test_official_model_ranges();
    test_argument_guards();
    printf("raptor_lake_platform: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
