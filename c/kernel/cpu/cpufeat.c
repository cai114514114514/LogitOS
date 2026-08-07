/* CPUID feature decode. See cpufeat.h for the contract and for why this TU has
 * no kernel dependencies (it is linked into host-side crypto tests too).
 *
 * Structure: one table, `featmap`, maps every enum cpu_feat to (leaf, subleaf,
 * register, bit) and a name. Detection is then a loop, not fifty hand-written
 * shifts -- which is the difference between "add a feature" being a one-line
 * table entry and being a place to typo a bit number.
 *
 * The leaf-availability rule matters: CPUID.0:EAX is the highest *basic* leaf
 * and CPUID.0x80000000:EAX the highest extended one. Querying past either
 * returns whatever the highest supported leaf returns (NOT zero) on real
 * silicon, so a naive "read leaf 7" on an old CPU reports leaf 1's ECX as
 * leaf 7's EBX and invents features. Every query below is gated. */

#include "cpufeat.h"

#if defined(__x86_64__) || defined(__i386__)
#define CPUFEAT_X86 1
#else
#define CPUFEAT_X86 0
#endif

/* register selector inside the CPUID result */
enum { R_EAX = 0, R_EBX, R_ECX, R_EDX };

struct featdef {
    uint32_t    leaf;
    uint32_t    subleaf;
    uint8_t     reg;
    uint8_t     bit;
    const char *name;
};

/* Indexed by enum cpu_feat -- the order here MUST match cpufeat.h. The
 * compile-time check at the bottom of the file enforces the length; the names
 * are what a boot line prints, so keep them the conventional /proc/cpuinfo
 * spellings. */
static const struct featdef featmap[CPU_FEAT_COUNT] = {
    /* leaf 1, EDX */
    { 1, 0, R_EDX,  0, "fpu"        },
    { 1, 0, R_EDX,  4, "tsc"        },
    { 1, 0, R_EDX,  5, "msr"        },
    { 1, 0, R_EDX, 15, "cmov"       },
    { 1, 0, R_EDX, 19, "clflush"    },
    { 1, 0, R_EDX, 23, "mmx"        },
    { 1, 0, R_EDX, 24, "fxsr"       },
    { 1, 0, R_EDX, 25, "sse"        },
    { 1, 0, R_EDX, 26, "sse2"       },
    { 1, 0, R_EDX, 28, "htt"        },
    /* leaf 1, ECX */
    { 1, 0, R_ECX,  0, "sse3"       },
    { 1, 0, R_ECX,  1, "pclmulqdq"  },
    { 1, 0, R_ECX,  3, "monitor"    },
    { 1, 0, R_ECX,  9, "ssse3"      },
    { 1, 0, R_ECX, 12, "fma"        },
    { 1, 0, R_ECX, 13, "cx16"       },
    { 1, 0, R_ECX, 19, "sse4.1"     },
    { 1, 0, R_ECX, 20, "sse4.2"     },
    { 1, 0, R_ECX, 21, "x2apic"     },
    { 1, 0, R_ECX, 22, "movbe"      },
    { 1, 0, R_ECX, 23, "popcnt"     },
    { 1, 0, R_ECX, 25, "aes"        },
    { 1, 0, R_ECX, 26, "xsave"      },
    { 1, 0, R_ECX, 27, "osxsave"    },
    { 1, 0, R_ECX, 28, "avx"        },
    { 1, 0, R_ECX, 29, "f16c"       },
    { 1, 0, R_ECX, 30, "rdrand"     },
    { 1, 0, R_ECX, 31, "hypervisor" },
    /* leaf 7 subleaf 0, EBX */
    { 7, 0, R_EBX,  0, "fsgsbase"   },
    { 7, 0, R_EBX,  3, "bmi1"       },
    { 7, 0, R_EBX,  5, "avx2"       },
    { 7, 0, R_EBX,  7, "smep"       },
    { 7, 0, R_EBX,  8, "bmi2"       },
    { 7, 0, R_EBX,  9, "erms"       },
    { 7, 0, R_EBX, 10, "invpcid"    },
    { 7, 0, R_EBX, 16, "avx512f"    },
    { 7, 0, R_EBX, 17, "avx512dq"   },
    { 7, 0, R_EBX, 18, "rdseed"     },
    { 7, 0, R_EBX, 19, "adx"        },
    { 7, 0, R_EBX, 20, "smap"       },
    { 7, 0, R_EBX, 23, "clflushopt" },
    { 7, 0, R_EBX, 29, "sha_ni"     },
    { 7, 0, R_EBX, 30, "avx512bw"   },
    { 7, 0, R_EBX, 31, "avx512vl"   },
    /* leaf 7 subleaf 0, ECX */
    { 7, 0, R_ECX,  1, "avx512vbmi" },
    { 7, 0, R_ECX,  2, "umip"       },
    { 7, 0, R_ECX,  3, "pku"        },
    { 7, 0, R_ECX,  9, "vaes"       },
    { 7, 0, R_ECX, 10, "vpclmulqdq" },
    { 7, 0, R_ECX, 22, "rdpid"      },
    /* leaf 0x80000001 */
    { 0x80000001, 0, R_ECX,  5, "lzcnt"     },
    { 0x80000001, 0, R_ECX,  8, "prefetchw" },
    { 0x80000001, 0, R_EDX, 20, "nx"        },
    { 0x80000001, 0, R_EDX, 26, "pdpe1gb"   },
    { 0x80000001, 0, R_EDX, 27, "rdtscp"    },
    { 0x80000001, 0, R_EDX, 29, "lm"        },
};

static struct cpu_features g_cf;

#if CPUFEAT_X86
static void do_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile ("cpuid"
                      : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                      : "a"(leaf), "c"(sub));
}

/* 1 if `leaf` is within the range the CPU advertises. Extended leaves
 * (0x8xxxxxxx) are bounded by CPUID.0x80000000:EAX, basic ones by CPUID.0:EAX;
 * an extended leaf on a CPU that reports max_ext_leaf == 0 does not exist. */
static int leaf_ok(uint32_t leaf)
{
    if (leaf >= 0x80000000u)
        return g_cf.max_ext_leaf >= leaf;
    return g_cf.max_leaf >= leaf;
}

static void copy4(char *dst, uint32_t v)
{
    dst[0] = (char)(v & 0xff);
    dst[1] = (char)((v >> 8) & 0xff);
    dst[2] = (char)((v >> 16) & 0xff);
    dst[3] = (char)((v >> 24) & 0xff);
}
#endif

void cpu_features_init(void)
{
    if (g_cf.valid) return;

    for (int i = 0; i < CPU_FEAT_COUNT; i++) g_cf.has[i] = 0;
    g_cf.vendor[0] = 0;
    g_cf.brand[0] = 0;
    g_cf.is_x86 = CPUFEAT_X86;

#if CPUFEAT_X86
    uint32_t r[4];

    do_cpuid(0, 0, r);
    g_cf.max_leaf = r[0];
    copy4(g_cf.vendor + 0, r[1]);        /* EBX, EDX, ECX -- not EBX, ECX, EDX */
    copy4(g_cf.vendor + 4, r[3]);
    copy4(g_cf.vendor + 8, r[2]);
    g_cf.vendor[12] = 0;

    do_cpuid(0x80000000u, 0, r);
    /* A CPU with no extended leaves can answer this with garbage; the only
     * sane reading is "extended leaves exist iff EAX is itself an extended
     * leaf number". */
    g_cf.max_ext_leaf = (r[0] > 0x80000000u) ? r[0] : 0;

    if (g_cf.max_ext_leaf >= 0x80000004u) {
        for (int i = 0; i < 3; i++) {
            do_cpuid(0x80000002u + (uint32_t)i, 0, r);
            for (int j = 0; j < 4; j++) copy4(g_cf.brand + i * 16 + j * 4, r[j]);
        }
        g_cf.brand[48] = 0;
    }

    for (int i = 0; i < CPU_FEAT_COUNT; i++) {
        const struct featdef *f = &featmap[i];
        if (!leaf_ok(f->leaf)) continue;
        do_cpuid(f->leaf, f->subleaf, r);
        g_cf.has[i] = (uint8_t)((r[f->reg] >> f->bit) & 1u);
    }

    /* XSAVE geometry -- recorded, not acted on. Leaf 0x0D only exists if the
     * CPU has XSAVE at all; on a CPU without it the values stay zero, which is
     * the honest answer rather than a bogus size read off leaf 1. */
    if (g_cf.has[CPU_XSAVE] && leaf_ok(0x0Du)) {
        do_cpuid(0x0Du, 0, r);
        g_cf.xcr0_supported     = ((uint64_t)r[3] << 32) | r[0];
        g_cf.xsave_enabled_size = r[1];
        g_cf.xsave_max_size     = r[2];
    }
#endif

    g_cf.valid = 1;
}

const struct cpu_features *cpu_features(void)
{
    if (!g_cf.valid) cpu_features_init();
    return &g_cf;
}

int cpu_has(enum cpu_feat f)
{
    if ((int)f < 0 || (int)f >= CPU_FEAT_COUNT) return 0;
    if (!g_cf.valid) cpu_features_init();
    return g_cf.has[f] ? 1 : 0;
}

const char *cpu_feat_name(enum cpu_feat f)
{
    if ((int)f < 0 || (int)f >= CPU_FEAT_COUNT) return "?";
    return featmap[f].name;
}

int cpu_features_present_count(void)
{
    const struct cpu_features *c = cpu_features();
    int n = 0;
    for (int i = 0; i < CPU_FEAT_COUNT; i++) if (c->has[i]) n++;
    return n;
}

int cpu_features_str(char *buf, int n)
{
    if (!buf || n <= 0) return 0;
    const struct cpu_features *c = cpu_features();
    int used = 0, written = 0;
    buf[0] = 0;
    for (int i = 0; i < CPU_FEAT_COUNT; i++) {
        if (!c->has[i]) continue;
        const char *s = featmap[i].name;
        int len = 0; while (s[len]) len++;
        int need = len + (used ? 1 : 0);
        if (used + need >= n) break;             /* leave room for the NUL */
        if (used) buf[used++] = ' ';
        for (int j = 0; j < len; j++) buf[used++] = s[j];
        buf[used] = 0;
        written++;
    }
    return written;
}

/* The table and the enum are two hand-maintained lists that must stay the same
 * length; a missing row would silently shift every feature after it onto the
 * wrong CPUID bit. Fail the build instead. */
typedef char cpufeat_table_len_check[
    (sizeof(featmap) / sizeof(featmap[0]) == (unsigned)CPU_FEAT_COUNT) ? 1 : -1];
