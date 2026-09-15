#ifndef LOGIT_CPU_PLATFORM_H
#define LOGIT_CPU_PLATFORM_H

#include <stdint.h>

#define CPU_PLATFORM_CACHE_MAX 8

enum cpu_platform_generation {
    CPU_PLATFORM_OTHER = 0,
    CPU_PLATFORM_SANDY_BRIDGE_EP,
    CPU_PLATFORM_IVY_BRIDGE_EP,
    /* CPUID cannot distinguish a 13th-generation Raptor Lake-S part from its
     * 14th-generation refresh.  This label therefore covers both, using only
     * Intel's documented display-family/model/stepping tuples. */
    CPU_PLATFORM_RAPTOR_LAKE_S,
};

enum cpu_platform_core_type {
    CPU_PLATFORM_CORE_UNKNOWN = 0,
    CPU_PLATFORM_CORE_ATOM = 0x20,
    CPU_PLATFORM_CORE_CORE = 0x40,
};

enum cpu_platform_xstate_abi {
    CPU_PLATFORM_XSTATE_UNKNOWN = 0,
    CPU_PLATFORM_XSTATE_FXSAVE,
};

struct cpu_platform_ops {
    void (*cpuid)(void *ctx, uint32_t leaf, uint32_t subleaf, uint32_t out[4]);
    int  (*rdmsr)(void *ctx, uint32_t msr, uint64_t *value);
    void *ctx;
};

struct cpu_platform_topology {
    int valid;
    int leaf_b;
    int leaf_1f;
    /* threads_per_core describes the core executing CPUID.  It is 2 on a
     * 14700KF P-core and 1 on an E-core, so hybrid packages cannot derive an
     * exact physical-core count by dividing package threads by this value. */
    int heterogeneous_smt;
    int core_count_exact;
    uint32_t x2apic_id;
    /* CPUID topology-domain capacity. Do not use this as the online CPU count;
     * firmware MADT enumeration and successful AP bring-up are authoritative. */
    uint32_t logical_per_package;
    uint32_t cores_per_package;
    uint32_t threads_per_core;
    uint8_t smt_shift;
    uint8_t package_shift;
    uint32_t package_id;
    uint32_t core_id;
    uint32_t thread_id;
};

/* CPUID.06H inventory only.  LogitOS does not program IA32_HW_FEEDBACK_PTR or
 * IA32_HW_FEEDBACK_CONFIG: these fields describe hardware, never enable it.
 * The table index is logical-processor scoped and may be duplicated or sparse,
 * so callers must not use it as an APIC ID or online-CPU count. */
struct cpu_platform_hfi {
    int leaf_6;
    int hfi_capable;
    int thread_director_capable;
    int performance_capable;
    int energy_efficiency_capable;
    uint8_t capability_bitmap;
    uint8_t thread_director_classes;
    uint8_t table_pages;
    uint16_t table_index;
};

/* CPUID.1AH is per-logical-processor state.  SMP bring-up can call the native
 * wrapper on every online CPU; host tests call the callback form so P- and
 * E-core answers can be checked without pretending the build host is x86. */
struct cpu_platform_core_info {
    int valid;
    int hybrid;
    int native_model_valid;
    enum cpu_platform_core_type type;
    uint32_t native_model_id;
    struct cpu_platform_topology topology;
    struct cpu_platform_hfi hfi;
};

struct cpu_platform_cache {
    uint8_t type;
    uint8_t level;
    uint32_t line_size;
    uint32_t partitions;
    uint32_t ways;
    uint64_t sets;
    /* CPUID.4:EAX[25:14] is the maximum count of addressable logical-processor
     * IDs sharing the cache, not an observed number of active sharers.  Sparse
     * Raptor Lake APIC IDs make that distinction visible (its L3 says 128). */
    uint32_t shared_logical;
    uint64_t size_bytes;
};

struct cpu_platform_mca {
    int mce;
    int mca;
    int cap_valid;
    uint8_t banks;
    int ctl_present;
    int ext_present;
    int cmci_present;
    int tes_present;
    int ser_present;
};

struct cpu_platform_info {
    int valid;
    char vendor[13];
    uint32_t signature;
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    enum cpu_platform_generation generation;
    int x2apic_capable;
    int apic_base_valid;
    int apic_enabled;
    int x2apic_active;
    uint64_t apic_base_phys;
    int tsc_capable;
    int invariant_tsc;
    int fxsave_capable;
    int xsave_capable;
    int osxsave_active;
    int avx_capable;
    enum cpu_platform_xstate_abi xstate_abi;
    int xstate_contract_safe;
    struct cpu_platform_topology topology;
    struct cpu_platform_core_info current_core;
    struct cpu_platform_cache cache[CPU_PLATFORM_CACHE_MAX];
    uint32_t cache_count;
    struct cpu_platform_mca mca;
};

/* Pure decoder. The callback seam makes malformed topology, cache overflow,
 * and MCA gating testable without pretending an Apple Silicon host has CPUID. */
int cpu_platform_decode(const struct cpu_platform_ops *ops,
                        struct cpu_platform_info *out);

int cpu_platform_decode_current_core(const struct cpu_platform_ops *ops,
                                     struct cpu_platform_core_info *out);
int cpu_platform_current_core(struct cpu_platform_core_info *out);

void cpu_platform_runtime_init(void);
const struct cpu_platform_info *cpu_platform_info(void);
const char *cpu_platform_generation_name(enum cpu_platform_generation generation);
const char *cpu_platform_core_type_name(enum cpu_platform_core_type type);

#endif
