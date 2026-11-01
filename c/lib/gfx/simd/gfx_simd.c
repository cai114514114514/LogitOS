#include "gfx_simd.h"

static uint64_t (*glass_clock)(void);
static struct gfx_glass_prof glass_profile;

/* The library also runs in ring 3 and host tests: inject the clock rather than
 * importing time_mono_ns. Publishing atomically keeps band readers race-free. */
void gfx_set_clock(uint64_t (*ns)(void))
{
    __atomic_store_n(&glass_clock, ns, __ATOMIC_RELEASE);
}

/* Read once per stage boundary. An unconfigured library keeps counting calls
 * and pixels with zero durations; it never invents a timing source. */
uint64_t gfx_now_ns(void)
{
    uint64_t (*ns)(void) = __atomic_load_n(&glass_clock, __ATOMIC_ACQUIRE);
    return ns ? ns() : 0;
}

/* Band workers finish independently. Relaxed adds preserve every contribution
 * without imposing ordering on pixels; their existing completion barrier owns
 * that ordering. Memo counters deliberately stay zero until S4 exists. */
void gfx_glass_record(uint64_t px, uint64_t blur_ns, uint64_t render_ns)
{
    __atomic_fetch_add(&glass_profile.calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&glass_profile.px, px, __ATOMIC_RELAXED);
    __atomic_fetch_add(&glass_profile.blur_ns, blur_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&glass_profile.render_ns, render_ns, __ATOMIC_RELAXED);
}

/* Return cumulative storage so reporting can take relaxed atomic field loads
 * while workers run. A plain struct copy would race with their atomic adds. */
const struct gfx_glass_prof *gfx_glass_prof(void)
{
    return &glass_profile;
}

/* ======================================================================== *
 * S2: SIMD pixel kernels.
 *
 * The blend maths is pixman's (pixman/pixman-combine32.h DIV_ONE_UN8 and
 * pixman/pixman-sse2.c pix_multiply_1x128) kept in 16-bit lanes: with
 * x = c*a + d*(255-a) <= 65025 the identity
 *     floor(x / 255) == (x + 1 + (x >> 8)) >> 8
 * needs only PMULLW, PADDW and PSRLW (Intel SDM Vol.2: PMULLW, PADDW, PSRLW),
 * so SSE2 is enough and no 32-bit multiply is needed. simd_test.c proves the
 * identity for every x, not by assumption.
 * ======================================================================== */

/* Clang vector extensions: SSE2 on x86-64, NEON on the Apple Silicon host. The
 * unaligned types are for loads and stores of rows at arbitrary pixel offsets;
 * may_alias lets them read the uint32_t pixels without breaking strict
 * aliasing, and no memcpy is involved (this file is also linked into libc-less
 * ring-3 applications). */
typedef uint8_t gv16u8 __attribute__((vector_size(16)));
typedef uint16_t gv8u16 __attribute__((vector_size(16)));
typedef uint32_t gv4u32 __attribute__((vector_size(16)));
typedef uint8_t gv16u8_unaligned __attribute__((vector_size(16), aligned(1), may_alias));
typedef uint32_t gv4u32_unaligned __attribute__((vector_size(16), aligned(4), may_alias));
typedef uint32_t u32_unaligned __attribute__((aligned(1), may_alias));

#define GFX_PIXELS_PER_VECTOR 4

/* Mutation hook for tests/hw-gfx.mk: losing the count % 4 pixels at the end of a
 * row is the classic SIMD bug, so the host suite proves it can see it. */
#ifdef GFX_NEGCTL_SIMD_DROP_TAIL
#define FINISH_TAIL(call) ((void)0)
#else
#define FINISH_TAIL(call) call
#endif

/* ---- scalar references: the display's original loop bodies --------------- */

/* The references must stay scalar. With fixed channel shifts clang would
 * auto-vectorise them to the very SSE2 instructions the VEC tier uses, so a
 * CPU that mishandled those instructions would corrupt the "safe" tier too,
 * and the S2 benchmark would compare VEC against an unnamed second VEC. */

/* One pixel of the original per-channel formula, with the channel positions
 * already known to be a permutation of {0,8,16}. The /255 is the compiler's
 * ordinary integer division, i.e. the existing semantics bit for bit. */
static inline uint32_t blend_pixel_ref(uint32_t src, uint32_t dst, int alpha)
{
    int inverse = 255 - alpha;
    uint32_t red = (uint32_t)(((int)(src & 255) * alpha + (int)(dst & 255) * inverse) / 255);
    uint32_t green = (uint32_t)(((int)((src >> 8) & 255) * alpha +
                                 (int)((dst >> 8) & 255) * inverse) / 255);
    uint32_t blue = (uint32_t)(((int)((src >> 16) & 255) * alpha +
                                (int)((dst >> 16) & 255) * inverse) / 255);
    return red | green << 8 | blue << 16;
}

/* ol_display_blend_rect / rr_blend_span row loop. */
void gfx_k_blend_cc_ref(uint32_t *dst, int count, uint32_t col, int alpha)
{
    #pragma clang loop vectorize(disable) interleave(disable)
    for (int i = 0; i < count; i++) dst[i] = blend_pixel_ref(col, dst[i], alpha);
}

/* ol_display_blit_glyph row loop. */
void gfx_k_blend_cv_ref(uint32_t *dst, const uint8_t *cov, int count, uint32_t col)
{
    #pragma clang loop vectorize(disable) interleave(disable)
    for (int i = 0; i < count; i++) {
        int coverage = cov[i];
        if (!coverage) continue;
        if (coverage >= 255) {
            dst[i] = col;
            continue;
        }
        dst[i] = blend_pixel_ref(col, dst[i], coverage);
    }
}

/* ol_display_blit_surface_alpha row loop. */
void gfx_k_blend_vc_ref(uint32_t *dst, const uint32_t *src, int count, int alpha)
{
    #pragma clang loop vectorize(disable) interleave(disable)
    for (int i = 0; i < count; i++) dst[i] = blend_pixel_ref(src[i], dst[i], alpha);
}

/* ol_display_blit_argb row loop. */
void gfx_k_blend_vv_ref(uint32_t *dst, const uint32_t *argb, int count)
{
    #pragma clang loop vectorize(disable) interleave(disable)
    for (int i = 0; i < count; i++) {
        uint32_t source = argb[i];
        int alpha = (int)(source >> 24);
        if (!alpha) continue;
        if (alpha == 255) {
            dst[i] = source & 0xffffffu;
            continue;
        }
        dst[i] = blend_pixel_ref(source, dst[i], alpha);
    }
}

/* no_builtin: clang's loop-idiom pass would otherwise turn these loops into
 * memcpy/memset calls, and libc-less ring-3 programs have neither. */
__attribute__((no_builtin("memcpy", "memmove", "memset")))
void gfx_k_copy_ref(uint32_t *dst, const uint32_t *src, int count)
{
    for (int i = 0; i < count; i++) dst[i] = src[i];
}

__attribute__((no_builtin("memcpy", "memmove", "memset")))
void gfx_k_fill_ref(uint32_t *dst, uint32_t value, int count)
{
    for (int i = 0; i < count; i++) dst[i] = value;
}

/* ---- vector building blocks ---------------------------------------------- */

/* Zero-extend bytes 0..7 / 8..15 to eight u16 lanes. The interleave with zero
 * is PUNPCKLBW / PUNPCKHBW (Intel SDM Vol.2) against a zero register, the
 * unpack_32_1x128 step of pixman-sse2.c. */
static inline gv8u16 widen_low(gv16u8 bytes)
{
    const gv16u8 zero = {0};
    return (gv8u16)__builtin_shufflevector(bytes, zero, 0, 16, 1, 17, 2, 18, 3, 19,
                                           4, 20, 5, 21, 6, 22, 7, 23);
}

static inline gv8u16 widen_high(gv16u8 bytes)
{
    const gv16u8 zero = {0};
    return (gv8u16)__builtin_shufflevector(bytes, zero, 8, 24, 9, 25, 10, 26, 11, 27,
                                           12, 28, 13, 29, 14, 30, 15, 31);
}

/* Inverse of the widen pair for lanes already <= 255: the even bytes of both
 * halves. LLVM lowers this to PACKUSWB on SSE2 (Intel SDM Vol.2 PACKUSWB). */
static inline gv16u8 narrow_pair(gv8u16 low, gv8u16 high)
{
    return __builtin_shufflevector((gv16u8)low, (gv16u8)high, 0, 2, 4, 6, 8, 10, 12, 14,
                                   16, 18, 20, 22, 24, 26, 28, 30);
}

static inline gv8u16 splat_u16(unsigned value)
{
    gv8u16 lanes = {0, 0, 0, 0, 0, 0, 0, 0};
    return lanes + (uint16_t)value;
}

static inline gv4u32 splat_u32(uint32_t value)
{
    gv4u32 lanes = {0, 0, 0, 0};
    return lanes + value;
}

/* floor(x / 255) for x <= 65025, see the identity above. */
static inline gv8u16 divide_by_255(gv8u16 sum)
{
#ifdef GFX_NEGCTL_SIMD_DIV255_APPROX
    /* Mutation: the tempting (sum * 257) >> 16 rounds 255 down to 0. */
    typedef uint32_t gv8u32 __attribute__((vector_size(32)));
    gv8u32 wide = __builtin_convertvector(sum, gv8u32);
    wide = (wide * 257) >> 16;
    return __builtin_convertvector(wide, gv8u16);
#else
    return (sum + splat_u16(1) + (sum >> 8)) >> 8;
#endif
}

/* src*alpha + dst*(255-alpha), divided by 255, in eight u16 lanes. */
static inline gv8u16 mix_lanes(gv8u16 src, gv8u16 dst, gv8u16 alpha)
{
    gv8u16 sum = src * alpha + dst * (splat_u16(255) - alpha);
    return divide_by_255(sum);
}

/* Byte 3 of every pixel must come out zero (ol_display_rgb keeps no alpha). */
static inline gv16u8 clear_byte3(gv16u8 pixels)
{
#ifdef GFX_NEGCTL_SIMD_KEEP_LANE3
    return pixels;
#else
    return (gv16u8)((gv4u32)pixels & splat_u32(0x00ffffffu));
#endif
}

/* Four pixels, alpha already spread to the four lanes of each pixel. */
static inline gv16u8 mix_pixels(gv16u8 src, gv16u8 dst, gv8u16 alpha_low, gv8u16 alpha_high)
{
    gv8u16 low = mix_lanes(widen_low(src), widen_low(dst), alpha_low);
    gv8u16 high = mix_lanes(widen_high(src), widen_high(dst), alpha_high);
    return clear_byte3(narrow_pair(low, high));
}

/* Coverage bytes 0..3 (packed in the low 4 bytes) to per-lane alpha. */
static inline gv8u16 coverage_alpha_low(gv16u8 bytes)
{
    const gv16u8 zero = {0};
    return (gv8u16)__builtin_shufflevector(bytes, zero, 0, 16, 0, 16, 0, 16, 0, 16,
                                           1, 16, 1, 16, 1, 16, 1, 16);
}

static inline gv8u16 coverage_alpha_high(gv16u8 bytes)
{
    const gv16u8 zero = {0};
    return (gv8u16)__builtin_shufflevector(bytes, zero, 2, 16, 2, 16, 2, 16, 2, 16,
                                           3, 16, 3, 16, 3, 16, 3, 16);
}

/* ARGB pixels: each pixel's own byte 3 (bytes 3, 7, 11, 15) as its alpha. */
static inline gv8u16 argb_alpha_low(gv16u8 pixels)
{
    const gv16u8 zero = {0};
    return (gv8u16)__builtin_shufflevector(pixels, zero, 3, 16, 3, 16, 3, 16, 3, 16,
                                           7, 16, 7, 16, 7, 16, 7, 16);
}

static inline gv8u16 argb_alpha_high(gv16u8 pixels)
{
    const gv16u8 zero = {0};
    return (gv8u16)__builtin_shufflevector(pixels, zero, 11, 16, 11, 16, 11, 16, 11, 16,
                                           15, 16, 15, 16, 15, 16, 15, 16);
}

/* Per-pixel select: mask lanes (all ones) take `chosen`, the rest `kept`. */
static inline gv4u32 select_pixels(gv4u32 mask, gv4u32 chosen, gv4u32 kept)
{
    return (chosen & mask) | (kept & ~mask);
}

/* ---- VEC kernels ---------------------------------------------------------- */

/* Same contract as gfx_k_blend_cc. The colour is widened once outside the loop
 * (pixman keeps the source in a register the same way). */
static void blend_cc_vec(uint32_t *dst, int count, uint32_t col, int alpha)
{
    gv4u32 color = splat_u32(col & 0xffffffu);
    gv8u16 color_low = widen_low((gv16u8)color);
    gv8u16 color_high = widen_high((gv16u8)color);
    gv8u16 alpha_lanes = splat_u16((unsigned)alpha);
    int i = 0;
    for (; i + GFX_PIXELS_PER_VECTOR <= count; i += GFX_PIXELS_PER_VECTOR) {
        gv16u8 old = *(const gv16u8_unaligned *)(dst + i);
        gv8u16 low = mix_lanes(color_low, widen_low(old), alpha_lanes);
        gv8u16 high = mix_lanes(color_high, widen_high(old), alpha_lanes);
        gv16u8 result = clear_byte3(narrow_pair(low, high));
#ifdef GFX_NEGCTL_SIMD_VERIFY_OFFBYONE
        /* Mutation for the verify build only: the first pixel of every vector
         * is one too high on rows of 64 pixels or more. The plan says "when
         * alpha is 128", but that cannot work as a control: the compositor's
         * blend_cc alphas measured in the guest are 28, 70 and 100 (never
         * 128), and 128 is a boot selftest case, so the mutation would pin the
         * library at SCALAR before the workload ran. Rows past the selftest's
         * 29 pixels are a bug the selftest cannot see and the verifier must. */
        if (count >= 64) result = result + (gv16u8){1};
#endif
        *(gv16u8_unaligned *)(dst + i) = result;
    }
    FINISH_TAIL(gfx_k_blend_cc_ref(dst + i, count - i, col, alpha));
}

/* Same contract as gfx_k_blend_cv. Text is mostly empty or fully covered, so
 * those quads skip the arithmetic; the select keeps partial quads exact. */
static void blend_cv_vec(uint32_t *dst, const uint8_t *cov, int count, uint32_t col)
{
    gv4u32 color = splat_u32(col);
    gv4u32 color24 = splat_u32(col & 0xffffffu);
    gv8u16 color_low = widen_low((gv16u8)color24);
    gv8u16 color_high = widen_high((gv16u8)color24);
    const gv16u8 zero = {0};
    int i = 0;
    for (; i + GFX_PIXELS_PER_VECTOR <= count; i += GFX_PIXELS_PER_VECTOR) {
        uint32_t quad = *(const u32_unaligned *)(cov + i);
#ifndef GFX_NEGCTL_SIMD_ZERO_COV_WRITE
        if (quad == 0) continue;
#endif
        if (quad == 0xffffffffu) {
            *(gv4u32_unaligned *)(dst + i) = color;
            continue;
        }
        gv4u32 packed = {quad, 0, 0, 0};
        gv16u8 bytes = (gv16u8)packed;
        gv4u32 old = *(const gv4u32_unaligned *)(dst + i);
        gv8u16 low = mix_lanes(color_low, widen_low((gv16u8)old), coverage_alpha_low(bytes));
        gv8u16 high = mix_lanes(color_high, widen_high((gv16u8)old), coverage_alpha_high(bytes));
        gv4u32 mixed = (gv4u32)clear_byte3(narrow_pair(low, high));
        /* Coverage as one u32 per pixel so it can be compared lane-wise. */
        gv4u32 per_pixel = (gv4u32)__builtin_shufflevector(bytes, zero, 0, 16, 16, 16,
                                                           1, 16, 16, 16, 2, 16, 16, 16,
                                                           3, 16, 16, 16);
        gv4u32 is_empty = (gv4u32)(per_pixel == splat_u32(0));
        gv4u32 is_full = (gv4u32)(per_pixel == splat_u32(255));
#ifdef GFX_NEGCTL_SIMD_ZERO_COV_WRITE
        /* Mutation: coverage 0 rewrites the pixel (and clears byte 3). */
        is_empty = splat_u32(0);
#endif
        gv4u32 result = select_pixels(is_empty, old, mixed);
        result = select_pixels(is_full, color, result);
        *(gv4u32_unaligned *)(dst + i) = result;
    }
    FINISH_TAIL(gfx_k_blend_cv_ref(dst + i, cov + i, count - i, col));
}

/* Same contract as gfx_k_blend_vc. */
static void blend_vc_vec(uint32_t *dst, const uint32_t *src, int count, int alpha)
{
    gv8u16 alpha_lanes = splat_u16((unsigned)alpha);
    int i = 0;
    for (; i + GFX_PIXELS_PER_VECTOR <= count; i += GFX_PIXELS_PER_VECTOR) {
        gv16u8 source = *(const gv16u8_unaligned *)(src + i);
        gv16u8 old = *(const gv16u8_unaligned *)(dst + i);
        *(gv16u8_unaligned *)(dst + i) = mix_pixels(source, old, alpha_lanes, alpha_lanes);
    }
    FINISH_TAIL(gfx_k_blend_vc_ref(dst + i, src + i, count - i, alpha));
}

/* Same contract as gfx_k_blend_vv. Cursor and icon sprites are mostly
 * transparent, so a quad of four zero alphas is skipped before any load of
 * the destination. */
static void blend_vv_vec(uint32_t *dst, const uint32_t *argb, int count)
{
    int i = 0;
    for (; i + GFX_PIXELS_PER_VECTOR <= count; i += GFX_PIXELS_PER_VECTOR) {
        if (((argb[i] | argb[i + 1] | argb[i + 2] | argb[i + 3]) >> 24) == 0) continue;
        gv16u8 source = *(const gv16u8_unaligned *)(argb + i);
        gv4u32 old = *(const gv4u32_unaligned *)(dst + i);
        gv4u32 mixed = (gv4u32)mix_pixels(source, (gv16u8)old, argb_alpha_low(source),
                                          argb_alpha_high(source));
        gv4u32 alpha = (gv4u32)source >> 24;
        gv4u32 is_clear = (gv4u32)(alpha == splat_u32(0));
        gv4u32 is_opaque = (gv4u32)(alpha == splat_u32(255));
        gv4u32 opaque_value = (gv4u32)source & splat_u32(0x00ffffffu);
        gv4u32 result = select_pixels(is_clear, old, mixed);
        result = select_pixels(is_opaque, opaque_value, result);
        *(gv4u32_unaligned *)(dst + i) = result;
    }
    FINISH_TAIL(gfx_k_blend_vv_ref(dst + i, argb + i, count - i));
}

/* Plain 16-byte moves, two vectors per iteration. no_builtin as above. */
__attribute__((no_builtin("memcpy", "memmove", "memset")))
static void copy_vec(uint32_t *dst, const uint32_t *src, int count)
{
    int i = 0;
    for (; i + 2 * GFX_PIXELS_PER_VECTOR <= count; i += 2 * GFX_PIXELS_PER_VECTOR) {
        gv4u32 first = *(const gv4u32_unaligned *)(src + i);
        gv4u32 second = *(const gv4u32_unaligned *)(src + i + GFX_PIXELS_PER_VECTOR);
        *(gv4u32_unaligned *)(dst + i) = first;
        *(gv4u32_unaligned *)(dst + i + GFX_PIXELS_PER_VECTOR) = second;
    }
    gfx_k_copy_ref(dst + i, src + i, count - i);
}

__attribute__((no_builtin("memcpy", "memmove", "memset")))
static void fill_vec(uint32_t *dst, uint32_t value, int count)
{
    gv4u32 lanes = splat_u32(value);
    int i = 0;
    for (; i + 2 * GFX_PIXELS_PER_VECTOR <= count; i += 2 * GFX_PIXELS_PER_VECTOR) {
        *(gv4u32_unaligned *)(dst + i) = lanes;
        *(gv4u32_unaligned *)(dst + i + GFX_PIXELS_PER_VECTOR) = lanes;
    }
    gfx_k_fill_ref(dst + i, value, count - i);
}

/* ---- CPU feature probe and tier selection -------------------------------- */

/* What CPUID leaf 1 says. Only the x86-64 build asks the hardware; the Apple
 * Silicon host has NEON by definition. The library stays independent of the
 * kernel's cpufeat module because it also runs in ring 3 and host tests. */
struct cpu_isa {
    int ready;
    int vector_ok;      /* VEC kernels can run (SSE2 on x86-64) */
    int ssse3;
    int sse41;
    int avx_bit;        /* CPU advertises AVX */
    int osxsave;        /* OS enabled XSAVE: CR4.OSXSAVE */
};

static struct cpu_isa cpu_isa_cache;

#if defined(__x86_64__)
/* Intel SDM Vol.2 CPUID leaf 01H: EDX bit 26 SSE2; ECX bit 9 SSSE3,
 * bit 19 SSE4.1, bit 27 OSXSAVE, bit 28 AVX. Duplicated probes by two threads
 * store identical values, so no lock is needed. */
static void probe_cpu(struct cpu_isa *isa)
{
    uint32_t eax = 1;
    uint32_t ebx;
    uint32_t ecx = 0;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    isa->vector_ok = (int)((edx >> 26) & 1);
    isa->ssse3 = (int)((ecx >> 9) & 1);
    isa->sse41 = (int)((ecx >> 19) & 1);
    isa->osxsave = (int)((ecx >> 27) & 1);
    isa->avx_bit = (int)((ecx >> 28) & 1);
}
#else
static void probe_cpu(struct cpu_isa *isa)
{
    isa->vector_ok = 1;
    isa->ssse3 = 0;
    isa->sse41 = 0;
    isa->osxsave = 0;
    isa->avx_bit = 0;
}
#endif

static const struct cpu_isa *cpu_isa(void)
{
    if (!__atomic_load_n(&cpu_isa_cache.ready, __ATOMIC_ACQUIRE)) {
        struct cpu_isa probed = {0};
        probe_cpu(&probed);
        cpu_isa_cache.vector_ok = probed.vector_ok;
        cpu_isa_cache.ssse3 = probed.ssse3;
        cpu_isa_cache.sse41 = probed.sse41;
        cpu_isa_cache.avx_bit = probed.avx_bit;
        cpu_isa_cache.osxsave = probed.osxsave;
        __atomic_store_n(&cpu_isa_cache.ready, 1, __ATOMIC_RELEASE);
    }
    return &cpu_isa_cache;
}

static int forced_tier = -1;
static int pinned_scalar;
/* 0 never run, 1 passed, 2 failed (selftest_failed names the first kernel). */
static int selftest_state;
static unsigned selftest_failed;

/* Highest tier this build implements AND the CPU can run. SSE41 is reserved,
 * so VEC is the ceiling. */
static enum gfx_simd_tier implemented_ceiling(void)
{
    return cpu_isa()->vector_ok ? GFX_SIMD_VEC : GFX_SIMD_SCALAR;
}

enum gfx_simd_tier gfx_simd_tier(void)
{
    enum gfx_simd_tier ceiling = implemented_ceiling();
    int forced = __atomic_load_n(&forced_tier, __ATOMIC_RELAXED);
    if (forced >= 0) return forced > (int)ceiling ? ceiling : (enum gfx_simd_tier)forced;
    if (__atomic_load_n(&pinned_scalar, __ATOMIC_RELAXED)) return GFX_SIMD_SCALAR;
    return ceiling;
}

void gfx_simd_force(int tier)
{
    __atomic_store_n(&forced_tier, tier < 0 ? -1 : tier, __ATOMIC_RELAXED);
}

/* ---- selftest ------------------------------------------------------------- */

const char *gfx_simd_kernel_name(unsigned kernel)
{
    static const char *const names[GFX_SIMD_KERNELS] = {
        "blend_cc", "blend_cv", "blend_vc", "blend_vv", "copy", "fill",
    };
    return kernel < GFX_SIMD_KERNELS ? names[kernel] : "?";
}

/* xorshift32 (Marsaglia): fixed-seed pixels so a failure reproduces. */
static uint32_t next_random(uint32_t *state)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

/* Rows are at most 29 pixels plus an offset of 0..3 so a stack of three
 * 32-pixel arrays stays well inside the 512-byte budget for kernel threads
 * (64 KiB stacks). */
#define SELFTEST_ROW 32
#define SELFTEST_MAX_N 29

static int rows_equal(const uint32_t *got, const uint32_t *want, int count)
{
    for (int i = 0; i < count; i++) {
        if (got[i] != want[i]) return 0;
    }
    return 1;
}

/* One kernel, all cases. Calls the VEC functions directly (not the dispatcher)
 * so a forced or pinned tier cannot hide a broken kernel. Returns 1 on a
 * mismatch. */
static int selftest_kernel(unsigned kernel)
{
    static const int lengths[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 16, 17, 29};
    static const int alphas[] = {0, 1, 127, 128, 254, 255};
    uint32_t rng = 0x9e3779b9u + kernel;
    uint32_t vec_row[SELFTEST_ROW];
    uint32_t ref_row[SELFTEST_ROW];
    uint32_t src_row[SELFTEST_ROW];
    uint8_t cov_row[SELFTEST_ROW];
    int case_index = 0;
    for (unsigned li = 0; li < sizeof lengths / sizeof lengths[0]; li++) {
        for (unsigned ai = 0; ai < sizeof alphas / sizeof alphas[0]; ai++) {
            int length = lengths[li];
            int offset = case_index++ & 3;
            for (int i = 0; i < SELFTEST_ROW; i++) {
                vec_row[i] = next_random(&rng);
                ref_row[i] = vec_row[i];
                src_row[i] = next_random(&rng);
                cov_row[i] = (uint8_t)next_random(&rng);
                /* Force the 0 and 255 paths into every case. */
                if (i % 5 == 0) cov_row[i] = 0;
                if (i % 7 == 0) cov_row[i] = 255;
                if (i % 5 == 0) src_row[i] &= 0x00ffffffu;
                if (i % 7 == 0) src_row[i] |= 0xff000000u;
            }
            uint32_t col = next_random(&rng);
            int alpha = alphas[ai];
            switch (kernel) {
            case 0:
                blend_cc_vec(vec_row + offset, length, col, alpha);
                gfx_k_blend_cc_ref(ref_row + offset, length, col, alpha);
                break;
            case 1:
                blend_cv_vec(vec_row + offset, cov_row + offset, length, col);
                gfx_k_blend_cv_ref(ref_row + offset, cov_row + offset, length, col);
                break;
            case 2:
                blend_vc_vec(vec_row + offset, src_row + offset, length, alpha);
                gfx_k_blend_vc_ref(ref_row + offset, src_row + offset, length, alpha);
                break;
            case 3:
                blend_vv_vec(vec_row + offset, src_row + offset, length);
                gfx_k_blend_vv_ref(ref_row + offset, src_row + offset, length);
                break;
            case 4:
                copy_vec(vec_row + offset, src_row + offset, length);
                gfx_k_copy_ref(ref_row + offset, src_row + offset, length);
                break;
            default:
                fill_vec(vec_row + offset, col, length);
                gfx_k_fill_ref(ref_row + offset, col, length);
                break;
            }
            /* The whole row, so a write past length is a mismatch too. */
            if (!rows_equal(vec_row, ref_row, SELFTEST_ROW)) return 1;
        }
    }
    return 0;
}

int gfx_simd_selftest(void)
{
    int failures = 0;
    unsigned first_failed = GFX_SIMD_KERNELS;
    if (implemented_ceiling() == GFX_SIMD_SCALAR) {
        /* No vector unit: nothing to compare, and nothing to pin. */
        selftest_state = 1;
        return 0;
    }
    for (unsigned kernel = 0; kernel < GFX_SIMD_KERNELS; kernel++) {
        if (selftest_kernel(kernel)) {
            failures++;
            if (first_failed == GFX_SIMD_KERNELS) first_failed = kernel;
        }
    }
    selftest_failed = first_failed;
    selftest_state = failures ? 2 : 1;
    __atomic_store_n(&pinned_scalar, failures ? 1 : 0, __ATOMIC_RELAXED);
    return failures;
}

const char *gfx_simd_selftest_failure(void)
{
    return selftest_state == 2 ? gfx_simd_kernel_name(selftest_failed) : 0;
}

/* ---- description ---------------------------------------------------------- */

struct text_out {
    char *buf;
    size_t cap;
    size_t len;
};

/* snprintf-style append: always counts, writes only while there is room for
 * the character and a terminating NUL. */
static void text_put(struct text_out *out, const char *text)
{
    for (; *text; text++) {
        if (out->cap && out->len + 1 < out->cap) out->buf[out->len] = *text;
        out->len++;
    }
}

static void text_put_number(struct text_out *out, unsigned value)
{
    char digits[12];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (count) {
        char one[2] = {digits[--count], 0};
        text_put(out, one);
    }
}

static const char *tier_name(enum gfx_simd_tier tier)
{
    if (tier == GFX_SIMD_VEC) return "vec";
    if (tier == GFX_SIMD_SSE41) return "sse41";
    return "scalar";
}

size_t gfx_simd_describe(char *buf, size_t capacity)
{
    struct text_out out = {buf, capacity, 0};
    text_put(&out, "tier=");
    text_put(&out, tier_name(gfx_simd_tier()));
#if defined(__x86_64__)
    const struct cpu_isa *isa = cpu_isa();
    text_put(&out, " isa=sse2 max=");
    /* max= is what the CPU could run; tier= is what this build implements.
     * The two differ on purpose until the optional SSE4.1 variant exists. */
    text_put(&out, isa->sse41 ? "sse41" : isa->ssse3 ? "ssse3" : "sse2");
    /* The kernel never enables AVX (CR4.OSXSAVE stays clear and every state
     * save path is FXSAVE), so avx is always "no"; the reason is evidence. */
    text_put(&out, " avx=no (");
    text_put(&out, !isa->avx_bit ? "cpu lacks avx" : !isa->osxsave ? "osxsave off" : "unused");
    text_put(&out, ")");
#else
    text_put(&out, " isa=neon max=neon avx=no (not x86)");
#endif
    text_put(&out, " selftest=");
    if (selftest_state == 1) {
        text_put(&out, "PASS");
    } else if (selftest_state == 2) {
        text_put(&out, "FAIL k=");
        text_put(&out, gfx_simd_kernel_name(selftest_failed));
    } else {
        text_put(&out, "not-run");
    }
    text_put(&out, " kernels=");
    text_put_number(&out, GFX_SIMD_KERNELS);
    if (capacity) out.buf[out.len < capacity ? out.len : capacity - 1] = 0;
    return out.len;
}

/* ---- verification build --------------------------------------------------- */

#ifdef GFX_SIMD_VERIFY
/* Every VEC call is shadowed by _ref on a stack copy of the destination, in
 * chunks of 64 pixels (256 bytes) so the stack stays small. The shadow run
 * happens before the real one only to keep one code path; both start from the
 * same destination contents. */
#define VERIFY_CHUNK 64

static uint64_t verify_calls;
static uint64_t verify_mismatch;
/* The boot benchmark runs fixed rows through the public kernels; counting them
 * would let it stand in for the compositor workload that verification is for. */
static int bench_active;

static inline int verification_enabled(void)
{
    return !__atomic_load_n(&bench_active, __ATOMIC_RELAXED);
}

__attribute__((no_builtin("memcpy", "memmove", "memset")))
static void verify_save(uint32_t *shadow, const uint32_t *dst, int count)
{
    for (int i = 0; i < count; i++) shadow[i] = dst[i];
}

static void verify_compare(const uint32_t *got, const uint32_t *want, int count)
{
    uint64_t bad = 0;
    for (int i = 0; i < count; i++) {
        if (got[i] != want[i]) bad++;
    }
    if (bad) __atomic_fetch_add(&verify_mismatch, bad, __ATOMIC_RELAXED);
}

int gfx_simd_verify_stats(uint64_t *calls, uint64_t *mismatch)
{
    *calls = __atomic_load_n(&verify_calls, __ATOMIC_RELAXED);
    *mismatch = __atomic_load_n(&verify_mismatch, __ATOMIC_RELAXED);
    return 1;
}
#else
int gfx_simd_verify_stats(uint64_t *calls, uint64_t *mismatch)
{
    *calls = 0;
    *mismatch = 0;
    return 0;
}
#endif

/* ---- dispatching entry points --------------------------------------------- */

static inline int vector_selected(void)
{
    return gfx_simd_tier() != GFX_SIMD_SCALAR;
}

void gfx_k_blend_cc(uint32_t *dst, int count, uint32_t col, int alpha)
{
    if (!vector_selected()) {
        gfx_k_blend_cc_ref(dst, count, col, alpha);
        return;
    }
#ifdef GFX_SIMD_VERIFY
    if (verification_enabled()) {
        __atomic_fetch_add(&verify_calls, 1, __ATOMIC_RELAXED);
        for (int done = 0; done < count; done += VERIFY_CHUNK) {
            int chunk = count - done < VERIFY_CHUNK ? count - done : VERIFY_CHUNK;
            uint32_t shadow[VERIFY_CHUNK];
            verify_save(shadow, dst + done, chunk);
            gfx_k_blend_cc_ref(shadow, chunk, col, alpha);
            blend_cc_vec(dst + done, chunk, col, alpha);
            verify_compare(dst + done, shadow, chunk);
        }
        return;
    }
#endif
    blend_cc_vec(dst, count, col, alpha);
}

void gfx_k_blend_cv(uint32_t *dst, const uint8_t *cov, int count, uint32_t col)
{
    if (!vector_selected()) {
        gfx_k_blend_cv_ref(dst, cov, count, col);
        return;
    }
#ifdef GFX_SIMD_VERIFY
    if (verification_enabled()) {
        __atomic_fetch_add(&verify_calls, 1, __ATOMIC_RELAXED);
        for (int done = 0; done < count; done += VERIFY_CHUNK) {
            int chunk = count - done < VERIFY_CHUNK ? count - done : VERIFY_CHUNK;
            uint32_t shadow[VERIFY_CHUNK];
            verify_save(shadow, dst + done, chunk);
            gfx_k_blend_cv_ref(shadow, cov + done, chunk, col);
            blend_cv_vec(dst + done, cov + done, chunk, col);
            verify_compare(dst + done, shadow, chunk);
        }
        return;
    }
#endif
    blend_cv_vec(dst, cov, count, col);
}

void gfx_k_blend_vc(uint32_t *dst, const uint32_t *src, int count, int alpha)
{
    if (!vector_selected()) {
        gfx_k_blend_vc_ref(dst, src, count, alpha);
        return;
    }
#ifdef GFX_SIMD_VERIFY
    if (verification_enabled()) {
        __atomic_fetch_add(&verify_calls, 1, __ATOMIC_RELAXED);
        for (int done = 0; done < count; done += VERIFY_CHUNK) {
            int chunk = count - done < VERIFY_CHUNK ? count - done : VERIFY_CHUNK;
            uint32_t shadow[VERIFY_CHUNK];
            verify_save(shadow, dst + done, chunk);
            gfx_k_blend_vc_ref(shadow, src + done, chunk, alpha);
            blend_vc_vec(dst + done, src + done, chunk, alpha);
            verify_compare(dst + done, shadow, chunk);
        }
        return;
    }
#endif
    blend_vc_vec(dst, src, count, alpha);
}

void gfx_k_blend_vv(uint32_t *dst, const uint32_t *argb, int count)
{
    if (!vector_selected()) {
        gfx_k_blend_vv_ref(dst, argb, count);
        return;
    }
#ifdef GFX_SIMD_VERIFY
    if (verification_enabled()) {
        __atomic_fetch_add(&verify_calls, 1, __ATOMIC_RELAXED);
        for (int done = 0; done < count; done += VERIFY_CHUNK) {
            int chunk = count - done < VERIFY_CHUNK ? count - done : VERIFY_CHUNK;
            uint32_t shadow[VERIFY_CHUNK];
            verify_save(shadow, dst + done, chunk);
            gfx_k_blend_vv_ref(shadow, argb + done, chunk);
            blend_vv_vec(dst + done, argb + done, chunk);
            verify_compare(dst + done, shadow, chunk);
        }
        return;
    }
#endif
    blend_vv_vec(dst, argb, count);
}

void gfx_k_copy(uint32_t *dst, const uint32_t *src, int count)
{
    if (vector_selected()) copy_vec(dst, src, count);
    else gfx_k_copy_ref(dst, src, count);
}

void gfx_k_fill(uint32_t *dst, uint32_t value, int count)
{
    if (vector_selected()) fill_vec(dst, value, count);
    else gfx_k_fill_ref(dst, value, count);
}

/* ---- boot benchmark -------------------------------------------------------- */

/* Runs one kernel on a GFX_SIMD_BENCH_PX row, reps times, at the given tier.
 * Alpha 100 and random coverage keep the blends on their general path, so the
 * number is the expensive one. fb.c prints it as ns per pixel; it never gates
 * anything. */
uint64_t gfx_simd_bench_ns(unsigned kernel, int tier, uint32_t *scratch,
                           uint64_t (*now)(void), unsigned reps)
{
    if (kernel >= GFX_SIMD_KERNELS || !scratch || !now) return 0;
    uint32_t *dst = scratch;
    uint32_t *src = scratch + GFX_SIMD_BENCH_PX;
    const uint8_t *cov = (const uint8_t *)(scratch + 2 * GFX_SIMD_BENCH_PX);
    uint32_t rng = 0x2545f491u;
    for (int i = 0; i < GFX_SIMD_BENCH_PX; i++) {
        dst[i] = next_random(&rng);
        src[i] = next_random(&rng);
    }
    for (int i = 0; i < GFX_SIMD_BENCH_PX / 4; i++) scratch[2 * GFX_SIMD_BENCH_PX + i] = next_random(&rng);
    int previous = __atomic_load_n(&forced_tier, __ATOMIC_RELAXED);
    gfx_simd_force(tier);
#ifdef GFX_SIMD_VERIFY
    __atomic_store_n(&bench_active, 1, __ATOMIC_RELAXED);
#endif
    uint64_t start = now();
    for (unsigned rep = 0; rep < reps; rep++) {
        switch (kernel) {
        case 0: gfx_k_blend_cc(dst, GFX_SIMD_BENCH_PX, 0x00c08040u, 100); break;
        case 1: gfx_k_blend_cv(dst, cov, GFX_SIMD_BENCH_PX, 0x00c08040u); break;
        case 2: gfx_k_blend_vc(dst, src, GFX_SIMD_BENCH_PX, 100); break;
        case 3: gfx_k_blend_vv(dst, src, GFX_SIMD_BENCH_PX); break;
        case 4: gfx_k_copy(dst, src, GFX_SIMD_BENCH_PX); break;
        default: gfx_k_fill(dst, 0x00c08040u, GFX_SIMD_BENCH_PX); break;
        }
    }
    uint64_t elapsed = now() - start;
#ifdef GFX_SIMD_VERIFY
    __atomic_store_n(&bench_active, 0, __ATOMIC_RELAXED);
#endif
    gfx_simd_force(previous);
    return elapsed;
}
