/* ilogb() of a NaN and of an infinity must be different numbers, and the header's
 * FP_ILOGBNAN / FP_ILOGB0 must be what the implementation actually returns.
 * mini_ilogb is third_party/libm/ilogb.c built against the mini-libc headers;
 * ilogb is the host's glibc, the reference. */
#include <limits.h>
#include <math.h>
#include <stdio.h>

int mini_ilogb(double);
extern int mini_fp_ilogbnan, mini_fp_ilogb0;

static int bad;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    int nan_ = mini_ilogb(NAN), inf = mini_ilogb(INFINITY), zero = mini_ilogb(0.0);
    CHECK(nan_ != inf, "ilogb(NaN)==ilogb(Inf)==%d: a NaN cannot be told from an infinity", nan_);
    CHECK(nan_ == ilogb(NAN), "ilogb(NaN) = %d, glibc says %d", nan_, ilogb(NAN));
    CHECK(inf == INT_MAX, "ilogb(Inf) = %d, want INT_MAX", inf);
    CHECK(zero == ilogb(0.0), "ilogb(0) = %d, glibc says %d", zero, ilogb(0.0));
    CHECK(nan_ == mini_fp_ilogbnan, "header FP_ILOGBNAN = %d but ilogb(NaN) returns %d", mini_fp_ilogbnan, nan_);
    CHECK(zero == mini_fp_ilogb0, "header FP_ILOGB0 = %d but ilogb(0) returns %d", mini_fp_ilogb0, zero);
    for (int e = -1074; e <= 1023; e += 37) {
        double x = ldexp(1.0, e);
        CHECK(mini_ilogb(x) == ilogb(x), "ilogb(2^%d) = %d, glibc says %d", e, mini_ilogb(x), ilogb(x));
    }
    printf("libm ilogb: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
