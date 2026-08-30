/* The contract every differential fixture is written against.
 *
 * A fixture is compiled TWICE from these same bytes: once with
 * --target=wasm32, where out_i32 and its siblings are UNDEFINED and become
 * imports from module `env`, and once for the host, where they are ordinary
 * functions in tests/unit/wasm_diff_native.c.  Both runs print through the
 * same formatter (tests/unit/wasm_diff_fmt.h) and the two stdouts are diffed
 * BYTE FOR BYTE.
 *
 * EVERY VALUE IS PRINTED AS ITS BIT PATTERN, never as a decimal.  A printed
 * float cannot distinguish -0.0 from 0.0, nor one NaN payload from another,
 * and both are things this interpreter can get wrong while looking right.
 *
 * WHAT MAY NOT GO IN A FIXTURE, and the reason each is a limit of the ORACLE
 * rather than a gap in the interpreter:
 *
 *   - anything that traps in wasm and is undefined in C: integer division by
 *     zero, INT_MIN / -1, and a float-to-integer conversion that is out of
 *     range or NaN.  The native side raises SIGFPE or saturates; the wasm side
 *     traps.  They are SUPPOSED to disagree, so a differential cannot judge
 *     them -- they are asserted by name in tests/unit/wasm_exec_test.c and
 *     exhaustively by the spec suite instead.
 *   - a NaN operand to min/max or to the rounding operators.  The
 *     specification leaves the resulting NaN's payload open, so the two sides
 *     are permitted to differ and a byte-for-byte diff would be asserting
 *     something untrue.  The suite's own `nan:canonical` / `nan:arithmetic`
 *     wildcards exist for exactly this and cover it there.
 *
 * There is no libc on the wasm side, so the fixtures bring their own memcpy
 * and use it on BOTH sides -- clang emits a call to memcpy for a struct copy
 * or a large initialiser, and on wasm there is nothing to call.
 */
#ifndef TESTS_FIXTURES_WASMDIFF_WDIFF_H_
#define TESTS_FIXTURES_WASMDIFF_WDIFF_H_

typedef unsigned u32;
typedef unsigned long long u64;
typedef int s32;
typedef long long s64;

void out_i32(u32 v);
void out_i64(u64 v);
void out_f32(float v);
void out_f64(double v);

void wbody(void);

static void wmemcpy(void *d, const void *s, u32 n)
{
	unsigned char *dd = (unsigned char *)d;
	const unsigned char *ss = (const unsigned char *)s;
	u32 i;
	for (i = 0; i < n; i++) dd[i] = ss[i];
}

/* `volatile` so neither backend can fold a whole test away.  The first draft
 * of tests/fixtures/wasm/indirect.c was folded to a constant by clang because
 * nothing ever wrote to its array, and it compiled, validated and exercised
 * nothing -- checked with wasm-objdump, not assumed. */
static u32 opaque_u32(u32 v) { volatile u32 t = v; return t; }
static u64 opaque_u64(u64 v) { volatile u64 t = v; return t; }
static float opaque_f32(float v) { volatile float t = v; return t; }
static double opaque_f64(double v) { volatile double t = v; return t; }

#endif
