/* Capability-based CPU platform inventory. Model numbers label diagnostics;
 * no boot decision is allowlisted by a brand string or model id. */
#include "cpu_platform.h"

#define IA32_APIC_BASE 0x01bu
#define IA32_MCG_CAP   0x179u

static uint32_t low_mask(unsigned bits)
{
    if (bits >= 32) return 0xffffffffu;
    return bits ? ((1u << bits) - 1u) : 0u;
}

static unsigned ceil_log2_u32(uint32_t n)
{
    unsigned bits = 0;
    uint32_t v = n > 1 ? n - 1 : 0;
    while (v) { bits++; v >>= 1; }
    return bits;
}

static int mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static void copy4(char *dst, uint32_t v)
{
    dst[0] = (char)(v & 0xffu);
    dst[1] = (char)((v >> 8) & 0xffu);
    dst[2] = (char)((v >> 16) & 0xffu);
    dst[3] = (char)((v >> 24) & 0xffu);
}

static int vendor_is_intel(const char vendor[13])
{
    static const char intel[] = "GenuineIntel";
    for (int i = 0; i < 12; i++) if (vendor[i] != intel[i]) return 0;
    return 1;
}

static void decode_signature(struct cpu_platform_info *out, uint32_t sig)
{
    uint32_t base_family = (sig >> 8) & 0xfu;
    uint32_t base_model = (sig >> 4) & 0xfu;
    uint32_t ext_family = (sig >> 20) & 0xffu;
    uint32_t ext_model = (sig >> 16) & 0xfu;
    out->signature = sig;
    out->stepping = sig & 0xfu;
    out->family = base_family == 0xfu ? base_family + ext_family : base_family;
    out->model = base_model;
    if (base_family == 0x6u || base_family == 0xfu)
        out->model |= ext_model << 4;

    /* These ids label what was observed. All feature and boot decisions use
     * CPUID capabilities decoded elsewhere in this structure. */
    if (vendor_is_intel(out->vendor) && out->family == 6u) {
        if (out->model == 0x2du)
            out->generation = CPU_PLATFORM_SANDY_BRIDGE_EP;
        else if (out->model == 0x3eu)
            out->generation = CPU_PLATFORM_IVY_BRIDGE_EP;
        /* Intel documents B0671 for Raptor Lake-S and its 14th-generation
         * refresh.  06_BFH is also Raptor Lake-S only at steppings 2 and 5;
         * accepting every BF stepping would turn an observed model number
         * into an invented platform identity.  Brand strings are deliberately
         * irrelevant because firmware and VMMs can rewrite them. */
        else if ((out->model == 0xb7u && out->stepping == 1u) ||
                 (out->model == 0xbfu &&
                  (out->stepping == 2u || out->stepping == 5u)))
            out->generation = CPU_PLATFORM_RAPTOR_LAKE_S;
    }
}

static int decode_extended_topology(const struct cpu_platform_ops *ops,
                                    uint32_t leaf, int hybrid,
                                    struct cpu_platform_topology *top)
{
    uint32_t smt_count = 0, package_count = 0, apic_id = 0;
    uint32_t previous_count = 0;
    unsigned smt_shift = 0, package_shift = 0, previous_shift = 0;
    int have_smt = 0, have_core = 0, have_package = 0, have_level = 0;
    int terminated = 0;

    /* CPUID.1FH can add module/tile/die levels after Core.  Package is not a
     * named level, so the last valid non-SMT level supplies its ID boundary.
     * Eight queries cover all six current nonzero types, one future level,
     * and the terminating zero without trusting an endless VMM. */
    for (uint32_t sub = 0; sub < 8; sub++) {
        uint32_t r[4];
        ops->cpuid(ops->ctx, leaf, sub, r);
        uint32_t count = r[1] & 0xffffu;
        uint32_t type = (r[2] >> 8) & 0xffu;
        uint32_t level = r[2] & 0xffu;
        unsigned shift = r[0] & 0x1fu;
        if (!type) { terminated = 1; break; }
        if (!count) return 0;
        /* ECX[7:0] echoes the requested level number.  Shift zero and equal
         * adjacent shifts are legal for one-entity domains; only a decrease
         * would make the cumulative x2APIC masks contradictory. */
        if (level != sub || shift >= 32u ||
            (have_level &&
             (shift < previous_shift || count < previous_count)) ||
            (uint64_t)count > (1ull << shift))
            return 0;
        if (have_level && r[3] != apic_id) return 0;
        have_level = 1;
        previous_count = count;
        previous_shift = shift;
        apic_id = r[3];
        if (type == 1u) {
            if (have_smt || have_package) return 0;
            have_smt = 1;
            smt_count = count;
            smt_shift = shift;
        } else {
            if (type == 2u) {
                if (!have_smt || have_core) return 0;
                have_core = 1;
            }
            /* Unknown nonzero types are real hierarchy domains.  Intel says
             * software must absorb them into the known domain below rather
             * than ignore them; extending this composite core-ID boundary
             * keeps distinct cores unique inside the package. */
            have_package = 1;
            package_count = count;
            package_shift = shift;
        }
    }
    if (!terminated || !have_smt || !have_core || !have_package || !smt_count ||
        package_count < smt_count || smt_shift > package_shift)
        return 0;
    if (!hybrid && package_count % smt_count) return 0;

    if (leaf == 0x1fu) top->leaf_1f = 1;
    else top->leaf_b = 1;
    top->heterogeneous_smt = hybrid;
    top->core_count_exact = !hybrid;
    top->x2apic_id = apic_id;
    /* EBX is the number of addressable logical-processor IDs in this topology
     * domain.  It is capacity data, never an online-CPU enumeration; MADT and
     * successful per-CPU bring-up own that count. */
    top->logical_per_package = package_count;
    top->threads_per_core = smt_count;
    /* CPUID topology is local to the executing logical processor.  On a
     * 14700KF it says 2 threads on a P-core and 1 on an E-core; neither answer
     * can be divided into 28 to recover the package's 20 physical cores.
     * Per-CPU sampling plus unique APIC core IDs is the honest enumeration. */
    top->cores_per_package = hybrid ? 0 : package_count / smt_count;
    top->smt_shift = (uint8_t)smt_shift;
    top->package_shift = (uint8_t)package_shift;
    top->thread_id = apic_id & low_mask(smt_shift);
    top->core_id = (apic_id >> smt_shift) &
                   low_mask(package_shift - smt_shift);
    top->package_id = apic_id >> package_shift;
    return 1;
}

static void decode_topology(const struct cpu_platform_ops *ops,
                            uint32_t max_leaf, const uint32_t leaf1[4],
                            int hybrid, struct cpu_platform_topology *top)
{
    uint32_t logical = (leaf1[1] >> 16) & 0xffu;
    uint32_t cores = 1;
    if (!logical) logical = 1;

    top->valid = 1;
    top->x2apic_id = leaf1[1] >> 24;
    top->logical_per_package = logical;
    top->cores_per_package = hybrid ? 0 : 1;
    top->threads_per_core = hybrid ? 0 : logical;
    top->heterogeneous_smt = hybrid;
    top->core_count_exact = !hybrid;

    if (!hybrid && max_leaf >= 4u) {
        uint32_t r[4];
        ops->cpuid(ops->ctx, 4, 0, r);
        if ((r[0] & 0x1fu) != 0) cores = ((r[0] >> 26) & 0x3fu) + 1u;
        if (cores > logical) cores = logical;
        top->cores_per_package = cores;
        top->threads_per_core =
            (cores && logical % cores == 0) ? logical / cores : 1;
    }

    unsigned smt_shift = ceil_log2_u32(top->threads_per_core);
    unsigned package_shift = ceil_log2_u32(top->logical_per_package);
    top->smt_shift = (uint8_t)smt_shift;
    top->package_shift = (uint8_t)package_shift;
    top->thread_id = top->x2apic_id & low_mask(smt_shift);
    top->core_id = (top->x2apic_id >> smt_shift) &
                   low_mask(package_shift - smt_shift);
    top->package_id = package_shift < 32 ?
                      top->x2apic_id >> package_shift : 0;

    /* Intel specifies 1FH as the preferred V2 topology leaf.  Falling back to
     * 0BH remains necessary for the E5 v1/v2 machines this decoder already
     * serves and for VMMs that advertise 1FH but return a null first level. */
    if (max_leaf >= 0x1fu &&
        decode_extended_topology(ops, 0x1fu, hybrid, top)) return;
    if (max_leaf >= 0x0bu &&
        decode_extended_topology(ops, 0x0bu, hybrid, top)) return;

    /* Leaf 1's eight-bit initial APIC ID cannot describe wide x2APIC IDs, and
     * a hybrid package has no uniform threads-per-core value from which a core
     * identity can be reconstructed.  Mark this sample unusable for physical-
     * core deduplication; SMP keeps the logical CPU online and isolates it by
     * its firmware MADT ID instead. */
#ifndef LOGIT_RAPTOR_NEGCTL_KEEP_HYBRID_LEGACY_TOPOLOGY
    if (hybrid) *top = (struct cpu_platform_topology){0};
#endif
}

static void decode_hfi(const struct cpu_platform_ops *ops, uint32_t max_leaf,
                       const char vendor[13], struct cpu_platform_hfi *out)
{
    *out = (struct cpu_platform_hfi){0};
    if (!vendor_is_intel(vendor) || max_leaf < 6u) return;

    uint32_t r[4];
    ops->cpuid(ops->ctx, 6, 0, r);
    out->leaf_6 = 1;
    out->hfi_capable = (r[0] & (1u << 19)) != 0;

    /* EDX geometry is architectural only when the base HFI capability is
     * present.  In particular, a stray Thread Director bit from broken VMM
     * emulation must not expose table geometry or invite future MSR writes. */
#ifndef LOGIT_RAPTOR_NEGCTL_DECODE_HFI_WITHOUT_CAP
    if (!out->hfi_capable) return;
#endif
    out->thread_director_capable = (r[0] & (1u << 23)) != 0;
    out->capability_bitmap = (uint8_t)(r[3] & 0xffu);
    out->performance_capable = (out->capability_bitmap & 1u) != 0;
    out->energy_efficiency_capable =
        (out->capability_bitmap & (1u << 1)) != 0;
    out->table_pages = (uint8_t)(((r[3] >> 8) & 0xfu) + 1u);
    out->table_index = (uint16_t)(r[3] >> 16);
    if (out->thread_director_capable)
        out->thread_director_classes = (uint8_t)((r[2] >> 8) & 0xffu);
}

static void decode_caches(const struct cpu_platform_ops *ops, uint32_t max_leaf,
                          struct cpu_platform_info *out)
{
    if (max_leaf < 4u) return;
    for (uint32_t sub = 0; sub < CPU_PLATFORM_CACHE_MAX; sub++) {
        uint32_t r[4];
        ops->cpuid(ops->ctx, 4, sub, r);
        uint32_t type = r[0] & 0x1fu;
        if (!type) break;

        struct cpu_platform_cache c = {0};
        c.type = (uint8_t)type;
        c.level = (uint8_t)((r[0] >> 5) & 0x7u);
        c.shared_logical = ((r[0] >> 14) & 0xfffu) + 1u;
        c.line_size = (r[1] & 0xfffu) + 1u;
        c.partitions = ((r[1] >> 12) & 0x3ffu) + 1u;
        c.ways = ((r[1] >> 22) & 0x3ffu) + 1u;
        c.sets = (uint64_t)r[2] + 1u;
        uint64_t size;
        if (!mul_u64(c.line_size, c.partitions, &size) ||
            !mul_u64(size, c.ways, &size) ||
            !mul_u64(size, c.sets, &size))
            continue;
        c.size_bytes = size;
        out->cache[out->cache_count++] = c;
    }
}

static void decode_core_info(const struct cpu_platform_ops *ops,
                             uint32_t max_leaf, const char vendor[13],
                             const uint32_t leaf1[4],
                             struct cpu_platform_core_info *out)
{
    *out = (struct cpu_platform_core_info){0};
    if (max_leaf >= 7u) {
        uint32_t r[4];
        ops->cpuid(ops->ctx, 7, 0, r);
        out->hybrid = (r[3] & (1u << 15)) != 0;
    }
    decode_topology(ops, max_leaf, leaf1, out->hybrid, &out->topology);
    decode_hfi(ops, max_leaf, vendor, &out->hfi);

    /* Leaf 1AH exists when MAXLEAF reaches it and EAX is nonzero.  It must be
     * sampled on each logical processor: both siblings of a P-core report
     * 40H, while an E-core reports 20H. */
    if (vendor_is_intel(vendor) && max_leaf >= 0x1au) {
        uint32_t r[4];
        ops->cpuid(ops->ctx, 0x1au, 0, r);
        if (r[0]) {
            out->native_model_valid = 1;
            out->type = (enum cpu_platform_core_type)(r[0] >> 24);
            out->native_model_id = r[0] & 0x00ffffffu;
        }
    }
    out->valid = 1;
}

int cpu_platform_decode_current_core(const struct cpu_platform_ops *ops,
                                     struct cpu_platform_core_info *out)
{
    if (!ops || !ops->cpuid || !out) return -1;
    *out = (struct cpu_platform_core_info){0};

    uint32_t r0[4], r1[4];
    char vendor[13];
    ops->cpuid(ops->ctx, 0, 0, r0);
    copy4(vendor + 0, r0[1]);
    copy4(vendor + 4, r0[3]);
    copy4(vendor + 8, r0[2]);
    vendor[12] = 0;
    if (r0[0] < 1u) return -1;
    ops->cpuid(ops->ctx, 1, 0, r1);
    decode_core_info(ops, r0[0], vendor, r1, out);
    return 0;
}

int cpu_platform_decode(const struct cpu_platform_ops *ops,
                        struct cpu_platform_info *out)
{
    if (!ops || !ops->cpuid || !out) return -1;
    *out = (struct cpu_platform_info){0};

    uint32_t r0[4], r1[4];
    ops->cpuid(ops->ctx, 0, 0, r0);
    uint32_t max_leaf = r0[0];
    copy4(out->vendor + 0, r0[1]);
    copy4(out->vendor + 4, r0[3]);
    copy4(out->vendor + 8, r0[2]);
    out->vendor[12] = 0;
    if (max_leaf < 1u) return -1;

    ops->cpuid(ops->ctx, 1, 0, r1);
    decode_signature(out, r1[0]);
    out->mca.mce = (r1[3] & (1u << 7)) != 0;
    out->mca.mca = (r1[3] & (1u << 14)) != 0;
    out->x2apic_capable = (r1[2] & (1u << 21)) != 0;
    out->tsc_capable = (r1[3] & (1u << 4)) != 0;
    out->fxsave_capable = (r1[3] & (1u << 24)) != 0;
    out->xsave_capable = (r1[2] & (1u << 26)) != 0;
    out->osxsave_active = (r1[2] & (1u << 27)) != 0;
    out->avx_capable = (r1[2] & (1u << 28)) != 0;
    /* The interrupt and signal frames are 512-byte FXSAVE images.  Advertising
     * XSAVE/AVX hardware must never silently change that ABI; OSXSAVE being
     * active here means the BSP/AP boot path failed to enforce its contract. */
    out->xstate_abi = CPU_PLATFORM_XSTATE_FXSAVE;
    out->xstate_contract_safe = out->fxsave_capable && !out->osxsave_active;
    decode_core_info(ops, max_leaf, out->vendor, r1, &out->current_core);
    out->topology = out->current_core.topology;
    decode_caches(ops, max_leaf, out);

    uint32_t rext[4];
    ops->cpuid(ops->ctx, 0x80000000u, 0, rext);
    uint32_t max_ext = rext[0] >= 0x80000000u ? rext[0] : 0;
    if (max_ext >= 0x80000007u) {
        ops->cpuid(ops->ctx, 0x80000007u, 0, rext);
        out->invariant_tsc = out->tsc_capable &&
                             (rext[3] & (1u << 8)) != 0;
    }

    int has_msr = (r1[3] & (1u << 5)) != 0;
    int has_apic = (r1[3] & (1u << 9)) != 0;
    uint64_t value;
    if (has_msr && has_apic && ops->rdmsr &&
        ops->rdmsr(ops->ctx, IA32_APIC_BASE, &value) == 0) {
        out->apic_base_valid = 1;
        out->apic_enabled = (value & (1ull << 11)) != 0;
        out->x2apic_active = (value & (1ull << 10)) != 0;
        out->apic_base_phys = value & 0x000ffffffffff000ull;
    }
    if (has_msr && out->mca.mce && out->mca.mca && ops->rdmsr &&
        ops->rdmsr(ops->ctx, IA32_MCG_CAP, &value) == 0) {
        out->mca.cap_valid = 1;
        out->mca.banks = (uint8_t)(value & 0xffu);
        out->mca.ctl_present = (value & (1ull << 8)) != 0;
        out->mca.ext_present = (value & (1ull << 9)) != 0;
        out->mca.cmci_present = (value & (1ull << 10)) != 0;
        out->mca.tes_present = (value & (1ull << 11)) != 0;
        out->mca.ser_present = (value & (1ull << 24)) != 0;
    }
    out->valid = 1;
    return 0;
}

static struct cpu_platform_info g_runtime;
static int g_runtime_done;

#if defined(__x86_64__) || defined(__i386__)
static void native_cpuid(void *ctx, uint32_t leaf, uint32_t subleaf,
                         uint32_t out[4])
{
    (void)ctx;
    __asm__ volatile ("cpuid"
                      : "=a"(out[0]), "=b"(out[1]), "=c"(out[2]), "=d"(out[3])
                      : "a"(leaf), "c"(subleaf));
}
#else
static void native_cpuid(void *ctx, uint32_t leaf, uint32_t subleaf,
                         uint32_t out[4])
{
    (void)ctx; (void)leaf; (void)subleaf;
    out[0] = out[1] = out[2] = out[3] = 0;
}
#endif

/* RDMSR is privileged. A hosted x86 unit binary may safely ask for the native
 * CPUID view, but its runtime ops deliberately omit this callback. MCA/APIC
 * MSRs are still covered by the pure decoder's mock callback. */
#if defined(__x86_64__) || defined(__i386__)
#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__ == 0
static int native_rdmsr(void *ctx, uint32_t msr, uint64_t *value)
{
    uint32_t lo, hi;
    (void)ctx;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    *value = ((uint64_t)hi << 32) | lo;
    return 0;
}
#define CPU_PLATFORM_NATIVE_RDMSR native_rdmsr
#else
#define CPU_PLATFORM_NATIVE_RDMSR ((int (*)(void *, uint32_t, uint64_t *))0)
#endif
#else
#define CPU_PLATFORM_NATIVE_RDMSR ((int (*)(void *, uint32_t, uint64_t *))0)
#endif

int cpu_platform_current_core(struct cpu_platform_core_info *out)
{
    const struct cpu_platform_ops ops = {
        native_cpuid, CPU_PLATFORM_NATIVE_RDMSR, 0
    };
    return cpu_platform_decode_current_core(&ops, out);
}

void cpu_platform_runtime_init(void)
{
    if (g_runtime_done) return;
    struct cpu_platform_ops ops = {
        native_cpuid, CPU_PLATFORM_NATIVE_RDMSR, 0
    };
    (void)cpu_platform_decode(&ops, &g_runtime);
    g_runtime_done = 1;
}

const struct cpu_platform_info *cpu_platform_info(void)
{
    if (!g_runtime_done) cpu_platform_runtime_init();
    return &g_runtime;
}

const char *cpu_platform_generation_name(enum cpu_platform_generation generation)
{
    switch (generation) {
    case CPU_PLATFORM_SANDY_BRIDGE_EP: return "sandy-bridge-ep";
    case CPU_PLATFORM_IVY_BRIDGE_EP:   return "ivy-bridge-ep";
    case CPU_PLATFORM_RAPTOR_LAKE_S:   return "raptor-lake-s";
    default:                           return "other";
    }
}

const char *cpu_platform_core_type_name(enum cpu_platform_core_type type)
{
    switch (type) {
    case CPU_PLATFORM_CORE_ATOM: return "intel-atom/e-core";
    case CPU_PLATFORM_CORE_CORE: return "intel-core/p-core";
    default:                     return "unknown";
    }
}
