/* Integer arithmetic across the edge values, both widths.
 *
 * THE SHIFT CASES ARE THE POINT, and they are written the way they are for a
 * measured reason.  `x << (s & 31)` compiles on wasm to a bare `i32.shl` with
 * an UNMASKED count -- clang drops the mask because the wasm instruction
 * already takes its count modulo the width -- and on the host to a shift with
 * the mask still in it, because C leaves a shift by 32 undefined.  So the two
 * sides compute the same thing by different routes, the C is free of undefined
 * behaviour, and an interpreter that forgets to mask disagrees on every count
 * at or above the width.  Verified with wasm-objdump: `i32.shl`, one
 * instruction, no `i32.and`.
 *
 * Division and remainder appear only with operands that do not trap.  See
 * wdiff.h: the trapping pairs are the oracle's limit, not the interpreter's,
 * and they are asserted by name elsewhere.
 */
#include "wdiff.h"

static const u32 V32[] = {
	0u, 1u, 2u, 7u, 0x7Fu, 0x80u, 0xFFu, 0x100u,
	0x7FFFu, 0x8000u, 0xFFFFu, 0x10000u,
	0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu,
	0xFFFFFFFEu, 0x55555555u, 0xAAAAAAAAu, 0x12345678u, 3u, 0xFFFFFFFDu
};
#define N32 (sizeof V32 / sizeof V32[0])

static const u64 V64[] = {
	0ull, 1ull, 2ull, 0xFFull, 0xFFFFull, 0xFFFFFFFFull, 0x100000000ull,
	0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0x8000000000000001ull,
	0xFFFFFFFFFFFFFFFFull, 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull,
	0x0123456789ABCDEFull, 3ull
};
#define N64 (sizeof V64 / sizeof V64[0])

static void ints32(void)
{
	u32 i, j;
	for (i = 0; i < N32; i++) {
		u32 a = opaque_u32(V32[i]);
		out_i32(a ? (u32)__builtin_clz(a) : 32u);
		out_i32(a ? (u32)__builtin_ctz(a) : 32u);
		out_i32((u32)__builtin_popcount(a));
		out_i32((u32)-(s32)a);
		out_i64((u64)(s64)(s32)a);      /* i64.extend_i32_s */
		out_i64((u64)a);                /* i64.extend_i32_u */
		for (j = 0; j < N32; j++) {
			u32 b = opaque_u32(V32[j]);
			u32 s = b;
			out_i32(a + b);
			out_i32(a - b);
			out_i32(a * b);
			out_i32(a & b);
			out_i32(a | b);
			out_i32(a ^ b);
			/* the masked-count family; see the header comment */
			out_i32(a << (s & 31));
			out_i32(a >> (s & 31));
			out_i32((u32)((s32)a >> (s & 31)));
			{
				u32 r = s & 31;
				out_i32(r ? ((a << r) | (a >> (32 - r))) : a);   /* rotl */
				out_i32(r ? ((a >> r) | (a << (32 - r))) : a);   /* rotr */
			}
			out_i32(a == b); out_i32(a != b);
			out_i32(a < b);  out_i32(a > b);
			out_i32(a <= b); out_i32(a >= b);
			out_i32((s32)a < (s32)b);  out_i32((s32)a > (s32)b);
			out_i32((s32)a <= (s32)b); out_i32((s32)a >= (s32)b);
			if (b != 0) {
				out_i32(a / b);
				out_i32(a % b);
				/* signed division traps only at INT_MIN / -1 */
				if (!(a == 0x80000000u && b == 0xFFFFFFFFu)) {
					out_i32((u32)((s32)a / (s32)b));
					out_i32((u32)((s32)a % (s32)b));
				}
			}
		}
	}
}

static void ints64(void)
{
	u32 i, j;
	for (i = 0; i < N64; i++) {
		u64 a = opaque_u64(V64[i]);
		out_i64(a ? (u64)__builtin_clzll(a) : 64ull);
		out_i64(a ? (u64)__builtin_ctzll(a) : 64ull);
		out_i64((u64)__builtin_popcountll(a));
		out_i32((u32)a);                /* i32.wrap_i64 */
		for (j = 0; j < N64; j++) {
			u64 b = opaque_u64(V64[j]);
			u32 s = (u32)b;
			out_i64(a + b);
			out_i64(a - b);
			out_i64(a * b);
			out_i64(a & b);
			out_i64(a | b);
			out_i64(a ^ b);
			out_i64(a << (s & 63));
			out_i64(a >> (s & 63));
			out_i64((u64)((s64)a >> (s & 63)));
			{
				u32 r = s & 63;
				out_i64(r ? ((a << r) | (a >> (64 - r))) : a);
				out_i64(r ? ((a >> r) | (a << (64 - r))) : a);
			}
			out_i32(a == b); out_i32(a != b);
			out_i32(a < b);  out_i32(a > b);
			out_i32((s64)a < (s64)b);  out_i32((s64)a >= (s64)b);
			if (b != 0) {
				out_i64(a / b);
				out_i64(a % b);
				if (!(a == 0x8000000000000000ull && b == 0xFFFFFFFFFFFFFFFFull)) {
					out_i64((u64)((s64)a / (s64)b));
					out_i64((u64)((s64)a % (s64)b));
				}
			}
		}
	}
}

void wbody(void)
{
	ints32();
	ints64();
}
