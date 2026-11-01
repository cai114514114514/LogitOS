/* Shared timing hooks (S0) and the SIMD pixel kernels (S2) of the compositor.
 * Having this header does not claim hardware proof: every kernel below is
 * checked on the host and in QEMU only, never yet on X79-VG4. */
#ifndef LOGIT_GFX_SIMD_H
#define LOGIT_GFX_SIMD_H

#include <stddef.h>
#include <stdint.h>

struct gfx_glass_prof {
    /* px counts visible panel bounding boxes, excluding the sampling halo;
     * rounded coverage and the write clip do not change that unit. */
    uint64_t calls, px, blur_ns, render_ns, memo_hit, memo_miss, memo_bypass;
};

/* Install the caller's monotonic ns clock; NULL disables timing, not drawing. */
void gfx_set_clock(uint64_t (*ns)(void));
const struct gfx_glass_prof *gfx_glass_prof(void);

/* Material instrumentation uses these without depending on a kernel timer.
 * Live profile readers must load each field atomically; it is not a snapshot. */
uint64_t gfx_now_ns(void);
void gfx_glass_record(uint64_t px, uint64_t blur_ns, uint64_t render_ns);

/* ---- S2: tiers --------------------------------------------------------- */

/* SCALAR runs the *_ref loops. VEC is one source written with clang vector
 * extensions: SSE2 on x86-64, NEON on the Apple Silicon host, so host tests
 * execute the very code the kernel runs (modulo the ISA lowering, which the
 * Rosetta x86-64 builds cover). SSE41 is reserved for an optional x86-only
 * variant; it is not implemented, so gfx_simd_tier() never returns it. */
enum gfx_simd_tier { GFX_SIMD_SCALAR = 0, GFX_SIMD_VEC = 1, GFX_SIMD_SSE41 = 2 };

enum gfx_simd_tier gfx_simd_tier(void);

/* Tests only: -1 returns to automatic selection. A forced tier overrides a
 * selftest pin, and is clamped to what this build implements. */
void gfx_simd_force(int tier);

/* Run every VEC kernel against its _ref on fixed inputs. Returns the number of
 * kernels that disagree; a non-zero result pins the automatic tier at SCALAR,
 * so a CPU or emulator that behaves differently never draws wrong pixels. */
int gfx_simd_selftest(void);

/* Name of the first kernel the last selftest rejected, or NULL if it passed
 * (or never ran). The boot log prints it on its own line. */
const char *gfx_simd_selftest_failure(void);

/* Writes "tier=vec isa=sse2 max=sse41 avx=no (osxsave off) selftest=PASS
 * kernels=6" (no prefix, no newline). snprintf contract: returns the full
 * length, truncates, always NUL-terminates when capacity > 0. */
size_t gfx_simd_describe(char *buf, size_t capacity);

/* The kernels do not know where a display keeps R, G and B: they treat bytes
 * 0..2 alike and clear byte 3. That is only the display's behaviour when its
 * three channel positions are a permutation of {0,8,16}, which
 * openlogit_display.h documents for every packed target. Anything else must
 * take the display's own scalar loops. */
static inline int gfx_simd_lanes_ok(unsigned rpos, unsigned gpos, unsigned bpos)
{
    int in_set = (rpos == 0 || rpos == 8 || rpos == 16) &&
                 (gpos == 0 || gpos == 8 || gpos == 16) &&
                 (bpos == 0 || bpos == 8 || bpos == 16);
    return in_set && rpos != gpos && rpos != bpos && gpos != bpos;
}

/* ---- S2: kernels ------------------------------------------------------- */

/* count is a pixel count, any alignment; dst and src never overlap. Every blend
 * computes per byte lane 0..2 floor((src*a + dst*(255-a)) / 255) and writes
 * byte 3 as zero, exactly like ol_display_rgb().  Each has a scalar _ref that
 * is the original loop body; the unsuffixed name dispatches on the tier. */

/* Constant colour over dst, alpha 0..255 with no special case (a == 0 still
 * rewrites the pixel and clears byte 3, as the display code always did). */
void gfx_k_blend_cc(uint32_t *dst, int count, uint32_t col, int alpha);
void gfx_k_blend_cc_ref(uint32_t *dst, int count, uint32_t col, int alpha);

/* Per-pixel 8-bit coverage of a constant colour: cov 0 leaves the pixel
 * untouched (byte 3 included), 255 stores col, the rest blend. */
void gfx_k_blend_cv(uint32_t *dst, const uint8_t *cov, int count, uint32_t col);
void gfx_k_blend_cv_ref(uint32_t *dst, const uint8_t *cov, int count, uint32_t col);

/* Source pixels over dst with one alpha (0..255) for the whole span. */
void gfx_k_blend_vc(uint32_t *dst, const uint32_t *src, int count, int alpha);
void gfx_k_blend_vc_ref(uint32_t *dst, const uint32_t *src, int count, int alpha);

/* ARGB source: alpha is s >> 24; 0 skips, 255 stores s & 0xffffff. */
void gfx_k_blend_vv(uint32_t *dst, const uint32_t *argb, int count);
void gfx_k_blend_vv_ref(uint32_t *dst, const uint32_t *argb, int count);

/* Plain copy and fill. The compiler already vectorises the display loops they
 * would replace, so S2 measures them but does not wire them in. */
void gfx_k_copy(uint32_t *dst, const uint32_t *src, int count);
void gfx_k_copy_ref(uint32_t *dst, const uint32_t *src, int count);
void gfx_k_fill(uint32_t *dst, uint32_t value, int count);
void gfx_k_fill_ref(uint32_t *dst, uint32_t value, int count);

/* ---- S2: boot benchmark and verification -------------------------------- */

#define GFX_SIMD_KERNELS 6
const char *gfx_simd_kernel_name(unsigned kernel);

/* The boot benchmark runs on one row of this many pixels. The caller provides
 * the scratch (GFX_SIMD_BENCH_WORDS 32-bit words: dst, src, coverage bytes),
 * because the library may not allocate and a kernel stack must stay small. */
#define GFX_SIMD_BENCH_PX 1280
#define GFX_SIMD_BENCH_WORDS (2 * GFX_SIMD_BENCH_PX + GFX_SIMD_BENCH_PX / 4)

/* Total ns for reps rows of the given kernel at the given tier, by the
 * caller's clock; 0 for an unknown kernel. Restores the previous forced tier. */
uint64_t gfx_simd_bench_ns(unsigned kernel, int tier, uint32_t *scratch,
                           uint64_t (*now)(void), unsigned reps);

/* A build with -DGFX_SIMD_VERIFY re-runs every VEC call through _ref on a
 * scratch copy and compares. Returns 1 and fills the totals (pixels that
 * differ, calls checked) when verification is compiled in, otherwise 0. */
int gfx_simd_verify_stats(uint64_t *calls, uint64_t *mismatch);

#endif
