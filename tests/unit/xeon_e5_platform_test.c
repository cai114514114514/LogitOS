/* Deterministic CPUID/MSR fixtures for LGA2011 Sandy/Ivy Bridge-EP. */
#include <stdint.h>
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
    uint32_t model, logical, cores, apic_id;
    int malformed_leaf_b;
    int no_mca;
    int overflow_cache;
    int x2apic_mode;
    int non_intel;
    int rdmsr_calls;
};

static uint32_t signature(uint32_t model)
{
    return (6u << 8) | ((model & 0xfu) << 4) |
           ((model >> 4) << 16) | 7u;
}

static uint32_t cache_ebx(uint32_t line, uint32_t partitions, uint32_t ways)
{
    return (line - 1u) | ((partitions - 1u) << 12) |
           ((ways - 1u) << 22);
}

static void fake_cpuid(void *opaque, uint32_t leaf, uint32_t sub,
                       uint32_t out[4])
{
    struct fixture *f = opaque;
    out[0] = out[1] = out[2] = out[3] = 0;
    switch (leaf) {
    case 0:
        out[0] = 0x0b;
        if (f->non_intel) {
            out[1] = 0x68747541u; out[3] = 0x69746e65u; out[2] = 0x444d4163u;
        } else {
            out[1] = 0x756e6547u; out[3] = 0x49656e69u; out[2] = 0x6c65746eu;
        }
        break;
    case 1:
        out[0] = signature(f->model);
        out[1] = (f->logical << 16) | (f->apic_id << 24);
        out[2] = 1u << 21;                  /* x2APIC capability */
        out[3] = (1u << 4) | (1u << 5) | (1u << 9);
        if (!f->no_mca) out[3] |= (1u << 7) | (1u << 14);
        break;
    case 4:
        if (f->overflow_cache && sub == 0) {
            out[0] = 1u | (1u << 5) | ((f->cores - 1u) << 26);
            out[1] = 0xffffffffu;
            out[2] = 0xffffffffu;
            break;
        }
        if (sub == 0) {
            out[0] = 1u | (1u << 5) | (1u << 14) |
                     ((f->cores - 1u) << 26);
            out[1] = cache_ebx(64, 1, 8);
            out[2] = 63;                    /* 32 KiB L1 data */
        } else if (sub == 1 && !f->overflow_cache) {
            uint32_t sets = f->model == 0x2d ? 16384u : 24576u;
            out[0] = 3u | (3u << 5) | ((f->logical - 1u) << 14) |
                     ((f->cores - 1u) << 26);
            out[1] = cache_ebx(64, 1, 20);
            out[2] = sets - 1u;              /* 20/30 MiB shared L3 */
        }
        break;
    case 0x0b:
        if (sub == 0) {
            if (f->malformed_leaf_b) break;
            out[0] = 1; out[1] = 2; out[2] = 1u << 8; out[3] = f->apic_id;
        } else if (sub == 1) {
            out[0] = f->logical <= 16 ? 4 : 5;
            out[1] = f->logical; out[2] = (2u << 8) | 1u;
            out[3] = f->apic_id;
        }
        break;
    case 0x80000000u:
        out[0] = 0x80000007u;
        break;
    case 0x80000007u:
        out[3] = 1u << 8;                   /* invariant TSC */
        break;
    }
}

static int fake_rdmsr(void *opaque, uint32_t msr, uint64_t *value)
{
    struct fixture *f = opaque;
    f->rdmsr_calls++;
    if (msr == 0x1bu) {
        *value = 0xfee00000ull | (1ull << 11);
        if (f->x2apic_mode) *value |= 1ull << 10;
        return 0;
    }
    if (msr == 0x179u) {
        *value = 12u | (1ull << 8) | (1ull << 9) | (1ull << 10) |
                 (1ull << 11) | (1ull << 24);
        return 0;
    }
    return -1;
}

static struct cpu_platform_info run_fixture(struct fixture *f)
{
    const struct cpu_platform_ops ops = { fake_cpuid, fake_rdmsr, f };
    struct cpu_platform_info out;
    ck(cpu_platform_decode(&ops, &out) == 0, "fixture decodes");
    return out;
}

static void test_ep(struct fixture f, enum cpu_platform_generation generation,
                    uint64_t l3_size)
{
    struct cpu_platform_info p = run_fixture(&f);
    ck(p.valid, "platform marked valid");
    ck(!strcmp(p.vendor, "GenuineIntel"), "vendor decoded in EBX/EDX/ECX order");
    ck(p.family == 6 && p.model == f.model && p.stepping == 7,
       "display family/model/stepping decoded");
    ck(p.generation == generation, "EP generation label follows display model");
    ck(p.x2apic_capable && p.invariant_tsc, "x2APIC and invariant TSC capabilities");
    ck(p.topology.valid && p.topology.leaf_b, "CPUID.0B topology selected");
    ck(p.topology.logical_per_package == f.logical &&
       p.topology.cores_per_package == f.cores &&
       p.topology.threads_per_core == 2, "package/core/SMT counts");
    ck(p.topology.package_id == 0 && p.topology.core_id == f.cores - 1 &&
       p.topology.thread_id == 1, "x2APIC id split into package/core/thread");
    ck(p.cache_count == 2, "two deterministic cache leaves retained");
    ck(p.cache[0].level == 1 && p.cache[0].size_bytes == 32u * 1024u,
       "L1 cache geometry");
    ck(p.cache[1].level == 3 && p.cache[1].size_bytes == l3_size &&
       p.cache[1].shared_logical == f.logical, "shared L3 cache geometry");
    ck(p.apic_base_valid && p.apic_enabled && !p.x2apic_active &&
       p.apic_base_phys == 0xfee00000ull, "IA32_APIC_BASE xAPIC mode");
    ck(p.mca.mce && p.mca.mca && p.mca.cap_valid && p.mca.banks == 12,
       "MCE/MCA and bank count enumerated");
    ck(p.mca.ctl_present && p.mca.ext_present && p.mca.cmci_present &&
       p.mca.tes_present && p.mca.ser_present, "MCG_CAP optional fields decoded");
    ck(f.rdmsr_calls == 2, "only APIC_BASE and MCG_CAP MSRs read");
}

static void test_fallback_and_guards(void)
{
    struct fixture f = { 0x3e, 24, 12, 23, 1, 0, 0, 0, 0, 0 };
    struct cpu_platform_info p = run_fixture(&f);
    ck(!p.topology.leaf_b, "malformed leaf 0B rejected");
    ck(p.topology.logical_per_package == 24 &&
       p.topology.cores_per_package == 12 &&
       p.topology.threads_per_core == 2, "legacy leaf1/leaf4 fallback");

    f = (struct fixture){ 0x2d, 16, 8, 15, 0, 1, 0, 0, 0, 0 };
    p = run_fixture(&f);
    ck(!p.mca.mce && !p.mca.mca && !p.mca.cap_valid,
       "MCA MSR is not read without CPUID prerequisites");
    ck(f.rdmsr_calls == 1, "only APIC_BASE read when MCA absent");

    f = (struct fixture){ 0x2d, 16, 8, 15, 0, 0, 1, 0, 0, 0 };
    p = run_fixture(&f);
    ck(p.cache_count == 0, "overflowing cache geometry rejected");

    f = (struct fixture){ 0x2d, 16, 8, 15, 0, 0, 0, 1, 0, 0 };
    p = run_fixture(&f);
    ck(p.x2apic_active, "firmware-left x2APIC mode is diagnosed");

    f = (struct fixture){ 0x2d, 16, 8, 15, 0, 0, 0, 0, 1, 0 };
    p = run_fixture(&f);
    ck(p.generation == CPU_PLATFORM_OTHER,
       "model number never overrides vendor identity");
}

int main(void)
{
    test_ep((struct fixture){ 0x2d, 16, 8, 15, 0, 0, 0, 0, 0, 0 },
            CPU_PLATFORM_SANDY_BRIDGE_EP, 20ull * 1024 * 1024);
    test_ep((struct fixture){ 0x3e, 24, 12, 23, 0, 0, 0, 0, 0, 0 },
            CPU_PLATFORM_IVY_BRIDGE_EP, 30ull * 1024 * 1024);
    test_fallback_and_guards();

    /* This is also an x86-host privilege canary: hosted runtime discovery may
     * execute CPUID, but cpu_platform.c must never inject native RDMSR here. */
    ck(cpu_platform_info() != 0, "host runtime view is safe and non-null");

    printf("xeon_e5_platform: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
