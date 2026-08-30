/* Control flow and calls: deep recursion, a large local frame, a dense switch
 * (which clang lowers to br_table), nested blocks with branches out of several
 * levels at once, and call_indirect through a function-pointer table.
 *
 * THE RECURSION DEPTH IS 500 AND THAT NUMBER IS OURS, NOT WEBASSEMBLY'S.
 * wasm_exec.h states WASM_MAX_CALL_DEPTH as an implementation bound of this
 * interpreter (1024, because the recursion is the host C stack's).  A fixture
 * that recursed deeper would be measuring our stack budget rather than the
 * module, and would fail as an exhaustion trap on one side and succeed on the
 * other -- a differential that reports a difference the specification does not
 * require is a broken instrument.
 */
#include "wdiff.h"

/* ---- deep recursion, and a second one that is NOT tail-recursive so the
 *      backend cannot turn it into a loop ---------------------------------- */

static u32 down(u32 n)
{
	if (n == 0) return 1u;
	return n ^ (down(n - 1) * 3u);
}

static u64 ack_ish(u32 d, u64 acc)
{
	if (d == 0) return acc * 2654435761ull + 1ull;
	return ack_ish(d - 1, acc ^ (acc >> 7)) + d;
}

/* ---- a large local frame -------------------------------------------------
 * Sixty-four live locals across a loop, which is what makes clang allocate a
 * real local vector rather than keeping everything in the operand stack.  The
 * decoder's local-declaration groups and the interpreter's frame layout are
 * only exercised by a function that actually has locals. */
static u64 wide(u32 seed)
{
	u32 v[64];
	u32 i, k;
	u64 acc = 0;
	for (i = 0; i < 64; i++) v[i] = seed + i * 2654435761u;
	for (k = 0; k < 40; k++)
		for (i = 0; i < 64; i++)
			v[i] = (v[i] ^ v[(i + 1) & 63]) * 1664525u + k;
	for (i = 0; i < 64; i++) acc = acc * 31u + v[i];
	return acc;
}

/* ---- a dense switch: br_table, including its default arm ----------------- */

static u32 table_of(u32 x)
{
	switch (x) {
	case 0: return 100u;
	case 1: return 101u;
	case 2: return 102u;
	case 3: return 103u;
	case 4: return 104u;
	case 5: return 105u;
	case 6: return 106u;
	case 7: return 107u;
	case 8: return 108u;
	case 9: return 109u;
	case 10: return 110u;
	case 11: return 111u;
	case 12: return 112u;
	case 13: return 113u;
	case 14: return 114u;
	case 15: return 115u;
	default: return 999u;      /* the DEFAULT arm, which is the last entry */
	}
}

/* ---- branching out of several levels at once ----------------------------- */

static u32 nested(u32 a, u32 b, u32 c)
{
	u32 acc = 0, i, j, k;
	for (i = 0; i < 8; i++) {
		for (j = 0; j < 8; j++) {
			for (k = 0; k < 8; k++) {
				acc = acc * 3u + i * 64u + j * 8u + k;
				if (k == c) break;               /* leave one block */
				if (j == b && k == a) goto out;  /* leave three */
			}
			if (j == b) continue;
			acc ^= j;
		}
		acc += i;
	}
out:
	return acc;
}

/* ---- call_indirect through a table --------------------------------------- */

static u32 f_add1(u32 x) { return x + 1u; }
static u32 f_dbl(u32 x)  { return x * 2u; }
static u32 f_neg(u32 x)  { return 0u - x; }
static u32 f_sq(u32 x)   { return x * x; }
static u32 f_rev(u32 x)  { return (x >> 16) | (x << 16); }

static u32 (*const OPS[5])(u32) = { f_add1, f_dbl, f_neg, f_sq, f_rev };

typedef u64 (*binop)(u64, u64);
static u64 b_and(u64 a, u64 b) { return a & b; }
static u64 b_or(u64 a, u64 b)  { return a | b; }
static u64 b_mul(u64 a, u64 b) { return a * b; }
static binop pick(u32 i) { return i == 0 ? b_and : (i == 1 ? b_or : b_mul); }

void wbody(void)
{
	u32 i;

	out_i32(down(opaque_u32(500)));
	out_i32(down(opaque_u32(1)));
	out_i32(down(opaque_u32(0)));
	out_i64(ack_ish(opaque_u32(400), 12345ull));

	out_i64(wide(opaque_u32(0)));
	out_i64(wide(opaque_u32(0xDEADBEEFu)));

	for (i = 0; i < 24; i++) out_i32(table_of(opaque_u32(i)));
	out_i32(table_of(opaque_u32(0xFFFFFFFFu)));

	for (i = 0; i < 10; i++)
		out_i32(nested(opaque_u32(i), opaque_u32(i ^ 3u), opaque_u32(i ^ 5u)));

	for (i = 0; i < 40; i++) {
		u32 x = opaque_u32(i * 2654435761u);
		out_i32(OPS[i % 5](x));
		out_i64(pick(i % 3)(opaque_u64(x), opaque_u64(~(u64)x)));
	}
}
