/* Floating point: arithmetic, the rounding operators, min/max, and every
 * conversion that is defined on both sides.
 *
 * THE INPUT SET IS THE TEST.  Ordinary values prove almost nothing here; what
 * separates a correct implementation from a plausible one is +-0, +-inf, the
 * subnormals, the exact halfway values that decide a tie, and the largest and
 * smallest finite magnitudes.  Every one of those is in the table below and
 * every result is compared as BITS, because -0.0 and 0.0 print identically and
 * differ in the only place that matters.
 *
 * NaN IS DELIBERATELY ABSENT FROM min/max AND THE ROUNDING OPERATORS, and
 * that is a limit of this oracle rather than a gap: the specification leaves
 * the payload of a NaN result open, so the host instruction and this
 * interpreter are both allowed to be right and different.  The spec suite
 * asserts those with its `nan:canonical` and `nan:arithmetic` wildcards, which
 * is the only way to state the requirement truthfully.  NaN IS present for
 * add/sub/mul/div, where both sides run the same host FPU and so must agree
 * bit for bit.
 */
#include "wdiff.h"

static float f32of(u32 b) { float f; wmemcpy(&f, &b, 4); return f; }
static u32 bitsof32(float f) { u32 b; wmemcpy(&b, &f, 4); return b; }
static double f64of(u64 b) { double d; wmemcpy(&d, &b, 8); return d; }

static const u32 F32V[] = {
	0x00000000u, 0x80000000u,             /* +-0 */
	0x00000001u, 0x80000001u,             /* +- the smallest subnormal */
	0x007FFFFFu, 0x00800000u,             /* the subnormal/normal boundary */
	0x3F000000u, 0xBF000000u,             /* +-0.5 -- the tie */
	0x3FC00000u, 0xBFC00000u,             /* +-1.5 */
	0x40200000u, 0x40600000u,             /* 2.5, 3.5 -- ties to EVEN both ways */
	0x3F800000u, 0xBF800000u,             /* +-1 */
	0x40490FDBu, 0xC0490FDBu,             /* +-pi */
	0x4B000000u, 0x4B7FFFFFu,             /* 2^23 and just below */
	0x7F7FFFFFu, 0xFF7FFFFFu,             /* +- the largest finite */
	0x7F800000u, 0xFF800000u              /* +-inf */
};
#define NF32 (sizeof F32V / sizeof F32V[0])

static const u64 F64V[] = {
	0x0000000000000000ull, 0x8000000000000000ull,
	0x0000000000000001ull, 0x8000000000000001ull,
	0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
	0x3FE0000000000000ull, 0xBFE0000000000000ull,   /* +-0.5 */
	0x3FF8000000000000ull, 0xBFF8000000000000ull,   /* +-1.5 */
	0x4004000000000000ull, 0x400C000000000000ull,   /* 2.5, 3.5 */
	0x3FF0000000000000ull, 0xBFF0000000000000ull,
	0x400921FB54442D18ull, 0xC00921FB54442D18ull,
	0x4330000000000000ull,                          /* 2^52 */
	0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull,
	0x7FF0000000000000ull, 0xFFF0000000000000ull
};
#define NF64 (sizeof F64V / sizeof F64V[0])

/* the NaNs, used only where both sides run the same hardware operation */
static const u32 NAN32[] = { 0x7FC00000u, 0xFFC00000u, 0x7FA00000u, 0x7F800001u };
#define NNAN32 (sizeof NAN32 / sizeof NAN32[0])

static void unary32(void)
{
	u32 i;
	for (i = 0; i < NF32; i++) {
		float a = opaque_f32(f32of(F32V[i]));
		out_f32(__builtin_fabsf(a));
		out_f32(-a);
		out_f32(__builtin_ceilf(a));
		out_f32(__builtin_floorf(a));
		out_f32(__builtin_truncf(a));
		out_f32(__builtin_nearbyintf(a));
		out_f32(__builtin_sqrtf(a));
		out_f64((double)a);                     /* f64.promote_f32 */
		out_i32(bitsof32(a));                   /* i32.reinterpret_f32 */
	}
}

static void binary32(void)
{
	u32 i, j;
	for (i = 0; i < NF32; i++) {
		float a = opaque_f32(f32of(F32V[i]));
		for (j = 0; j < NF32; j++) {
			float b = opaque_f32(f32of(F32V[j]));
			out_f32(a + b);
			out_f32(a - b);
			out_f32(a * b);
			out_f32(a / b);
			out_f32(__builtin_elementwise_minimum(a, b));
			out_f32(__builtin_elementwise_maximum(a, b));
			out_f32(__builtin_copysignf(a, b));
			out_i32(a == b); out_i32(a != b);
			out_i32(a < b);  out_i32(a > b);
			out_i32(a <= b); out_i32(a >= b);
		}
	}
	/* NaN through the four arithmetic operators only: both sides are the same
	 * host instruction there, so the payload must match exactly. */
	for (i = 0; i < NNAN32; i++) {
		float a = opaque_f32(f32of(NAN32[i]));
		for (j = 0; j < NF32; j++) {
			float b = opaque_f32(f32of(F32V[j]));
			out_f32(a + b); out_f32(b + a);
			out_f32(a - b); out_f32(a * b); out_f32(a / b);
			out_i32(a == b); out_i32(a != b); out_i32(a < b); out_i32(a >= b);
		}
	}
}

static void unary64(void)
{
	u32 i;
	for (i = 0; i < NF64; i++) {
		double a = opaque_f64(f64of(F64V[i]));
		out_f64(__builtin_fabs(a));
		out_f64(-a);
		out_f64(__builtin_ceil(a));
		out_f64(__builtin_floor(a));
		out_f64(__builtin_trunc(a));
		out_f64(__builtin_nearbyint(a));
		out_f64(__builtin_sqrt(a));
		out_f32((float)a);                      /* f32.demote_f64 */
	}
}

static void binary64(void)
{
	u32 i, j;
	for (i = 0; i < NF64; i++) {
		double a = opaque_f64(f64of(F64V[i]));
		for (j = 0; j < NF64; j++) {
			double b = opaque_f64(f64of(F64V[j]));
			out_f64(a + b);
			out_f64(a - b);
			out_f64(a * b);
			out_f64(a / b);
			out_f64(__builtin_elementwise_minimum(a, b));
			out_f64(__builtin_elementwise_maximum(a, b));
			out_f64(__builtin_copysign(a, b));
			out_i32(a == b); out_i32(a < b); out_i32(a >= b);
		}
	}
}

/* Integer <-> float, in the direction that is total, and in the other
 * direction only for values whose truncation is in range.  Out of range is a
 * TRAP in wasm and undefined in C, so the two are supposed to disagree; see
 * wdiff.h. */
static void conversions(void)
{
	static const s32 I32[] = { 0, 1, -1, 2, -2, 127, -128, 0x7FFFFFFF,
	                           (s32)0x80000000, 16777216, 16777217, -16777217 };
	static const s64 I64[] = { 0, 1, -1, 0x7FFFFFFFFFFFFFFFll,
	                           (s64)0x8000000000000000ull, 9007199254740993ll,
	                           -9007199254740993ll, 0xFFFFFFFFll };
	u32 i;
	for (i = 0; i < sizeof I32 / sizeof I32[0]; i++) {
		s32 v = (s32)opaque_u32((u32)I32[i]);
		out_f32((float)v);
		out_f32((float)(u32)v);
		out_f64((double)v);
		out_f64((double)(u32)v);
	}
	for (i = 0; i < sizeof I64 / sizeof I64[0]; i++) {
		s64 v = (s64)opaque_u64((u64)I64[i]);
		out_f32((float)v);
		out_f32((float)(u64)v);
		out_f64((double)v);
		out_f64((double)(u64)v);
	}
	{
		static const double SAFE[] = {
			0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.9999, -1.9999,
			2147483647.0, -2147483648.0, 4294967295.0,
			1e15, -1e15, 0.99999999, -0.99999999
		};
		for (i = 0; i < sizeof SAFE / sizeof SAFE[0]; i++) {
			double d = opaque_f64(SAFE[i]);
			float f = (float)d;
			if (d >= -2147483648.0 && d <= 2147483647.0) out_i32((u32)(s32)d);
			if (d >= 0.0 && d <= 4294967295.0) out_i32((u32)d);
			out_i64((u64)(s64)d);
			if (d >= 0.0) out_i64((u64)d);
			if (f >= -2147483648.0f && f <= 2147483520.0f) out_i32((u32)(s32)f);
			out_i64((u64)(s64)f);
		}
	}
}

void wbody(void)
{
	unary32();
	binary32();
	unary64();
	binary64();
	conversions();
}
