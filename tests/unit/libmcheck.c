/* libmcheck -- is the freestanding cross-build of musl's libm bit-identical to
 * a native build of the SAME sources?
 *
 * WHAT THIS DOES AND DOES NOT CLAIM, because the distinction is the whole
 * design. It does NOT check that musl is correct -- musl and glibc legitimately
 * differ in the last ulp of a transcendental, so comparing against the host's
 * own libm would fail on differences that are nobody's bug. It checks the
 * PORT: that `clang --target=x86_64-elf -ffreestanding` against mini-libc, with
 * SSE turned on by boot code rather than by the ABI, produces the same bits as
 * the same third_party/libm/*.c compiled natively. That is the thing that can
 * break here, and it is exactly the separation c/apps/audio/audiocheck.c:8-20
 * draws for the decoders.
 *
 * OUTPUT IS RAW IEEE BITS, NOT %a, and that is deliberate too. mini-libc has
 * its own dtoa (c/apps/libc/src/dtoa.c) and the host has glibc's; printing
 * floats would put a second implementation between the thing under test and the
 * comparison, and a formatting difference would read as a libm failure. Sixteen
 * hex digits of the payload go through nothing but integer formatting.
 *
 *   build:  target -> /bin/libmcheck (tests/libm.mk), host -> $(BUILD)/libmhost
 *   gate:   make test-libm-cli   -- the two outputs must be byte-identical
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>

static uint64_t bits(double v)
{
    union { double d; uint64_t u; } c;
    c.d = v;
    return c.u;
}

/* A deterministic argument sweep. No rand(): the host and the target must walk
 * the identical list, and "identical" has to survive two different libcs, so
 * the generator is an LCG written out here rather than borrowed from either. */
static uint64_t lcg_state = 0x2545F4914F6CDD1Dull;
static double next_arg(double lo, double hi)
{
    lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
    /* 53 bits of mantissa out of the high half -- the low bits of an LCG are
     * the bad ones, and a lopsided sweep would still be deterministic but would
     * cover the argument range worse. */
    double t = (double)(lcg_state >> 11) / 9007199254740992.0;   /* [0,1) */
    return lo + t * (hi - lo);
}

static int lines;

static void emit1(const char *name, double x, double y)
{
    printf("LIBM %s %016llx %016llx\n", name,
           (unsigned long long)bits(x), (unsigned long long)bits(y));
    lines++;
}
static void emit2(const char *name, double x, double z, double y)
{
    printf("LIBM %s %016llx %016llx %016llx\n", name,
           (unsigned long long)bits(x), (unsigned long long)bits(z),
           (unsigned long long)bits(y));
    lines++;
}

#define SWEEP1(fn, lo, hi, n) do {                                  \
        lcg_state = 0x2545F4914F6CDD1Dull ^ (uint64_t)__LINE__;      \
        for (int i = 0; i < (n); i++) {                              \
            double x = next_arg((lo), (hi));                         \
            emit1(#fn, x, fn(x));                                    \
        }                                                            \
    } while (0)

#define SWEEP2(fn, alo, ahi, blo, bhi, n) do {                      \
        lcg_state = 0x2545F4914F6CDD1Dull ^ (uint64_t)__LINE__;      \
        for (int i = 0; i < (n); i++) {                              \
            double a = next_arg((alo), (ahi));                       \
            double b = next_arg((blo), (bhi));                       \
            emit2(#fn, a, b, fn(a, b));                              \
        }                                                            \
    } while (0)

/* The exact arguments every implementation is expected to get exactly, listed
 * rather than swept: zeroes with both signs, the identities, and the boundaries
 * where a range reduction changes branch. A sweep hits these with probability
 * zero. */
static void exact_cases(void)
{
    static const double v[] = {
        0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0,
        3.141592653589793, 1.5707963267948966, 0.7853981633974483,
        2.718281828459045, 1e-300, 1e300, 4.9406564584124654e-324,
        2.2250738585072014e-308, 0.9999999999999999, 1.0000000000000002,
    };
    for (unsigned i = 0; i < sizeof v / sizeof *v; i++) {
        double x = v[i];
        emit1("exact.fabs",  x, fabs(x));
        emit1("exact.floor", x, floor(x));
        emit1("exact.ceil",  x, ceil(x));
        emit1("exact.trunc", x, trunc(x));
        emit1("exact.round", x, round(x));
        emit1("exact.sqrt",  x, sqrt(x < 0 ? -x : x));
        emit1("exact.exp",   x, exp(x));
        emit1("exact.sin",   x, sin(x));
        emit1("exact.cos",   x, cos(x));
        emit1("exact.atan",  x, atan(x));
        emit1("exact.cbrt",  x, cbrt(x));
        emit1("exact.expm1", x, expm1(x));
        emit1("exact.log1p", x, log1p(x));
        if (x > 0) {
            emit1("exact.log",   x, log(x));
            emit1("exact.log2",  x, log2(x));
            emit1("exact.log10", x, log10(x));
        }
    }
}

/* `libmcheck noop` prints the same VOLUME of output through the same printf and
 * the same serial write path, and calls nothing from libm. It exists to split
 * one question in two: when this program wedged the machine under -smp 4, was
 * it the floating point or was it the output? Run both modes; whichever hangs
 * is the answer. Keep it -- the next person to meet an intermittent hang on
 * this machine will want the same discriminator. */
static int noop_mode(void)
{
    for (int i = 0; i < 1214; i++) {
        printf("LIBM noop %016llx %016llx\n",
               (unsigned long long)(0x3ff0000000000000ull + (unsigned)i),
               (unsigned long long)(0x4000000000000000ull + (unsigned)i * 7u));
        lines++;
    }
    printf("LIBM_DONE %d\n", lines);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] == 'n') return noop_mode();

    /* Trig: one sweep inside the primary range and one far outside it, because
     * the interesting code in sin/cos/tan is __rem_pio2, and it only runs on
     * large arguments. A port that lost __rem_pio2_large would pass the first
     * sweep and fail the second. */
    SWEEP1(sin,  -3.15, 3.15, 40);
    SWEEP1(cos,  -3.15, 3.15, 40);
    SWEEP1(tan,  -1.56, 1.56, 40);
    SWEEP1(sin,  -1e6,  1e6,  40);
    SWEEP1(cos,  -1e6,  1e6,  40);
    SWEEP1(tan,  -1e6,  1e6,  40);

    SWEEP1(asin, -1.0, 1.0, 30);
    SWEEP1(acos, -1.0, 1.0, 30);
    SWEEP1(atan, -1e4, 1e4, 30);

    SWEEP1(exp,   -700.0, 700.0, 40);
    SWEEP1(exp2,  -1000.0, 1000.0, 30);
    SWEEP1(expm1, -30.0, 30.0, 30);
    SWEEP1(log,    1e-300, 1e300, 40);
    SWEEP1(log2,   1e-300, 1e300, 30);
    SWEEP1(log10,  1e-300, 1e300, 30);
    SWEEP1(log1p, -0.9, 1e6, 30);

    SWEEP1(sinh, -700.0, 700.0, 30);
    SWEEP1(cosh, -700.0, 700.0, 30);
    SWEEP1(tanh, -30.0, 30.0, 30);

    SWEEP1(sqrt, 0.0, 1e300, 30);
    SWEEP1(cbrt, -1e300, 1e300, 30);

    SWEEP2(pow,   0.0, 100.0, -20.0, 20.0, 60);
    SWEEP2(atan2, -1e3, 1e3, -1e3, 1e3, 40);
    SWEEP2(fmod,  -1e6, 1e6, 1e-3, 1e3, 40);
    SWEEP2(hypot, -1e150, 1e150, -1e150, 1e150, 30);

    exact_cases();

    /* frexp and ldexp have an integer side, so they get their own shape: the
     * exponent has to match too, and it is printed as an integer rather than
     * folded into a double. */
    for (int i = 0; i < 30; i++) {
        lcg_state = 0x9E3779B97F4A7C15ull ^ (uint64_t)i;
        double x = next_arg(-1e100, 1e100);
        int e = 0;
        double m = frexp(x, &e);
        printf("LIBM frexp %016llx %016llx %d\n",
               (unsigned long long)bits(x), (unsigned long long)bits(m), e);
        printf("LIBM ldexp %016llx %d %016llx\n",
               (unsigned long long)bits(m), e, (unsigned long long)bits(ldexp(m, e)));
        lines += 2;
    }

    printf("LIBM_DONE %d\n", lines);
    return 0;
}
