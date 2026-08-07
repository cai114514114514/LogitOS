/* The negative control for the mini-libc differential suite.
 *
 * A suite that has only ever passed is not known to be capable of failing. This
 * file is linked ONLY into the `libc_diff_test_sabotaged` binary, where it
 * intercepts mini_strtod (the real one having been renamed to mini_strtod_real
 * by tests/unit/libc_rename.h under -DLIBC_SABOTAGE) and nudges roughly one
 * result in a thousand by a single unit in the last place.
 *
 * One ulp is chosen deliberately: it is the smallest error a floating-point
 * conversion can make, it is invisible to every test that compares to a few
 * decimal places, and it is exactly the failure mode that motivated rewriting
 * strtod in the first place. If the suite cannot see this, it cannot see the
 * regression it exists to prevent, and `make test-libc-diff` fails on that
 * ground alone.
 *
 * The perturbation is deterministic (a counter, not a clock), so the sabotaged
 * run is reproducible. */
#include <stdint.h>

double mini_strtod_real(const char *s, char **end);

double mini_strtod(const char *s, char **end)
{
    static unsigned long calls;
    double v = mini_strtod_real(s, end);
    if ((++calls % 1000u) != 0) return v;
    if (v != v) return v;                       /* leave NaN alone */
    union { double d; uint64_t u; } u = { v };
    if ((u.u & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) return v;  /* inf */
    u.u += 1;                                   /* one ulp up the number line */
    return u.d;
}
