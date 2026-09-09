/* The gate for the WebAssembly interpreter: hand-built checks of the five
 * places this is routinely gotten wrong, then the official spec suite's
 * assert_return / assert_trap / assert_exhaustion corpus.
 *
 * THE BUILT-IN HALF IS NOT A FLOOR, IT IS THE PART THAT MUST NOT DEPEND ON A
 * DOWNLOAD.  CLAUDE.md rule 5: a control that cannot be watched failing is
 * worse than no control, and a property whose only witness is a fetched corpus
 * stops being tested the day the fetch fails -- quietly, with the gate still
 * green.  Every one of the five traps below is therefore asserted here as well
 * as in the suite, and `make test-wasm-exec-negctl` breaks one opcode and
 * requires BOTH halves to redden.
 *
 * WHAT THE SPEC HALF COUNTS, AND WHAT IT REFUSES TO COUNT.  Passed of run,
 * with four separate columns for the things that are not results:
 *
 *   out-of-scope   the module uses a post-MVP proposal and the decoder refused
 *                  it by name.  NOT a pass and NOT a failure -- it is the
 *                  measure of how much of the live suite an MVP interpreter
 *                  cannot judge, and folding it into either column would be a
 *                  number that moves for the wrong reason.
 *   unlinkable     the module imports from another module registered earlier
 *                  in the .wast.  This runner links only `spectest`, so it
 *                  says so instead of guessing at an import.
 *   APPARATUS      our arena ran out, or an implementation bound was hit.  Any
 *                  nonzero count fails the gate: without that, a zero-byte
 *                  arena reports every assert_trap as correctly trapped.
 *   trap-misnamed  it trapped, and not with the trap the suite named.  This is
 *                  counted apart from a pass because "integer overflow" where
 *                  the suite said "invalid conversion to integer" is a real
 *                  defect that a plain trapped/did-not-trap column hides.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wasm_exec.h"

#define ARENA_BYTES (192u * 1024u * 1024u)

static unsigned char *g_arena;
static int g_fail, g_checks;

static void ck(int cond, const char *what)
{
	g_checks++;
	if (!cond) { printf("  FAIL %s\n", what); g_fail++; }
}

/* ---------------------------------------------------------------- part 1
 * A tiny module builder, so the cases below read as the wasm they are instead
 * of as a wall of hex.  One function, exported as "f", optionally with one
 * memory and one table+element segment. */

struct mb {
	unsigned char b[4096];
	unsigned n;
};

static void put(struct mb *m, unsigned v) { m->b[m->n++] = (unsigned char)v; }
static void puts_(struct mb *m, const unsigned char *p, unsigned n)
{ unsigned i; for (i = 0; i < n; i++) put(m, p[i]); }

static unsigned char vt_of(char c)
{
	switch (c) {
	case 'i': return 0x7F;
	case 'I': return 0x7E;
	case 'f': return 0x7D;
	default:  return 0x7C;   /* 'd' */
	}
}

/* type section holding `ntypes` signatures, function/export/code for the
 * LAST one only; sigs is e.g. "ii>i" meaning (i32,i32)->i32. */
static void sec(struct mb *m, unsigned id, const unsigned char *body, unsigned n)
{
	put(m, id);
	put(m, n);            /* every section here is < 128 bytes on purpose */
	puts_(m, body, n);
}

/* `ntypes` is 1, or 2 to emit the SAME signature twice.  Two identical
 * functypes at different indices is the only way to tell structural type
 * equality from index equality, and call_indirect is required to use the
 * former -- an implementation that compares indices refuses a legal call, and
 * only for modules that happen to carry a duplicate type. */
static unsigned build_n(unsigned char *out, const char *sig, int ntypes, int mempages,
                        int tabsize, const unsigned char *elems, unsigned nelems,
                        const unsigned char *body, unsigned nbody)
{
	struct mb m, t;
	const char *arrow = strchr(sig, '>');
	unsigned np = (unsigned)(arrow - sig), nr = (unsigned)strlen(arrow + 1), i;
	int k;

	m.n = 0;
	put(&m, 0); put(&m, 'a'); put(&m, 's'); put(&m, 'm');
	put(&m, 1); put(&m, 0); put(&m, 0); put(&m, 0);

	t.n = 0;
	put(&t, ntypes);
	for (k = 0; k < ntypes; k++) {
		put(&t, 0x60); put(&t, np);
		for (i = 0; i < np; i++) put(&t, vt_of(sig[i]));
		put(&t, nr);
		for (i = 0; i < nr; i++) put(&t, vt_of(arrow[1 + i]));
	}
	sec(&m, 1, t.b, t.n);

	t.n = 0; put(&t, 1); put(&t, 0);
	sec(&m, 3, t.b, t.n);

	if (tabsize >= 0) {
		t.n = 0; put(&t, 1); put(&t, 0x70); put(&t, 0); put(&t, tabsize);
		sec(&m, 4, t.b, t.n);
	}
	if (mempages >= 0) {
		t.n = 0; put(&t, 1); put(&t, 0); put(&t, mempages);
		sec(&m, 5, t.b, t.n);
	}

	t.n = 0; put(&t, 1); put(&t, 1); put(&t, 'f'); put(&t, 0); put(&t, 0);
	sec(&m, 7, t.b, t.n);

	if (nelems) sec(&m, 9, elems, nelems);

	t.n = 0;
	put(&t, 1);
	put(&t, nbody + 1);      /* body size: the locals vector plus the code */
	put(&t, 0);              /* no local declarations */
	puts_(&t, body, nbody);
	sec(&m, 10, t.b, t.n);

	memcpy(out, m.b, m.n);
	return m.n;
}

static unsigned build(unsigned char *out, const char *sig, int mempages,
                      int tabsize, const unsigned char *elems, unsigned nelems,
                      const unsigned char *body, unsigned nbody)
{ return build_n(out, sig, 1, mempages, tabsize, elems, nelems, body, nbody); }

/* ---- running one built module ------------------------------------------ */

struct ran { int loaderr, trap; union wasm_val ret; struct wasm_instance *in; };

static struct ran call(const unsigned char *mod, unsigned n,
                       const union wasm_val *args)
{
	static struct wasm_module m;
	struct wasm_arena a;
	struct ran r;
	unsigned idx;
	union wasm_val rets[2];

	r.trap = 0; r.ret.bits = 0; r.in = 0;
	wasm_arena_init(&a, g_arena, ARENA_BYTES);
	r.loaderr = wasm_load(mod, n, &m, &a);
	if (r.loaderr) return r;
	r.trap = wasm_instantiate(&r.in, &m, &a, 0, 0);
	if (r.trap) return r;
	if (!wasm_export_index(r.in, "f", 1, WASM_EXT_FUNC, &idx)) { r.trap = -1; return r; }
	rets[0].bits = 0;
	r.trap = wasm_invoke(r.in, idx, args, rets);
	r.ret = rets[0];
	return r;
}

static union wasm_val VI(uint32_t v) { union wasm_val x; x.bits = 0; x.i32 = v; return x; }
static union wasm_val VJ(uint64_t v) { union wasm_val x; x.bits = 0; x.i64 = v; return x; }

/* One binary op over two i32 constants folded into the body, so each case is
 * one line and the failure names the opcode. */
static void bin32(unsigned char op, uint32_t a, uint32_t b,
                  int want_trap, uint32_t want, const char *what)
{
	unsigned char mod[4096], body[64];
	unsigned n = 0;
	struct ran r;
	body[n++] = 0x20; body[n++] = 0x00;
	body[n++] = 0x20; body[n++] = 0x01;
	body[n++] = op;
	body[n++] = 0x0B;
	{
		union wasm_val args[2];
		args[0] = VI(a); args[1] = VI(b);
		r = call(mod, build(mod, "ii>i", -1, -1, 0, 0, body, n), args);
	}
	if (r.loaderr) { printf("  FAIL %s: load %s\n", what, wasm_errstr(r.loaderr)); g_fail++; g_checks++; return; }
	g_checks++;
	if (want_trap) {
		if (r.trap != want_trap)
			{ printf("  FAIL %s: wanted trap `%s`, got `%s`\n", what,
			         wasm_trapstr(want_trap), wasm_trapstr(r.trap)); g_fail++; }
	} else if (r.trap) {
		printf("  FAIL %s: unexpected trap `%s`\n", what, wasm_trapstr(r.trap)); g_fail++;
	} else if (r.ret.i32 != want) {
		printf("  FAIL %s: got 0x%08x want 0x%08x\n", what, r.ret.i32, want); g_fail++;
	}
}

static void bin64(unsigned char op, uint64_t a, uint64_t b,
                  int want_trap, uint64_t want, const char *what)
{
	unsigned char mod[4096], body[64];
	unsigned n = 0;
	struct ran r;
	body[n++] = 0x20; body[n++] = 0x00;
	body[n++] = 0x20; body[n++] = 0x01;
	body[n++] = op;
	body[n++] = 0x0B;
	{
		union wasm_val args[2];
		args[0] = VJ(a); args[1] = VJ(b);
		r = call(mod, build(mod, "II>I", -1, -1, 0, 0, body, n), args);
	}
	if (r.loaderr) { printf("  FAIL %s: load %s\n", what, wasm_errstr(r.loaderr)); g_fail++; g_checks++; return; }
	g_checks++;
	if (want_trap) {
		if (r.trap != want_trap)
			{ printf("  FAIL %s: wanted trap `%s`, got `%s`\n", what,
			         wasm_trapstr(want_trap), wasm_trapstr(r.trap)); g_fail++; }
	} else if (r.trap) {
		printf("  FAIL %s: unexpected trap `%s`\n", what, wasm_trapstr(r.trap)); g_fail++;
	} else if (r.ret.i64 != want) {
		printf("  FAIL %s: got 0x%016llx want 0x%016llx\n", what,
		       (unsigned long long)r.ret.i64, (unsigned long long)want); g_fail++;
	}
}

/* one unary op over one float constant, compared as BITS */
static void un_f(const char *sig, unsigned char op, uint64_t argbits,
                 int want_trap, uint64_t want, const char *what)
{
	unsigned char mod[4096], body[64];
	unsigned n = 0;
	struct ran r;
	union wasm_val args[1];
	body[n++] = 0x20; body[n++] = 0x00;
	body[n++] = op;
	body[n++] = 0x0B;
	args[0].bits = 0; args[0].i64 = argbits;
	r = call(mod, build(mod, sig, -1, -1, 0, 0, body, n), args);
	if (r.loaderr) { printf("  FAIL %s: load %s\n", what, wasm_errstr(r.loaderr)); g_fail++; g_checks++; return; }
	g_checks++;
	if (want_trap) {
		if (r.trap != want_trap)
			printf("  FAIL %s: wanted trap `%s`, got `%s`\n", what,
			       wasm_trapstr(want_trap), wasm_trapstr(r.trap)), g_fail++;
	} else if (r.trap) {
		printf("  FAIL %s: unexpected trap `%s`\n", what, wasm_trapstr(r.trap)); g_fail++;
	} else if (r.ret.i64 != want) {
		printf("  FAIL %s: got bits 0x%llx want 0x%llx\n", what,
		       (unsigned long long)r.ret.i64, (unsigned long long)want); g_fail++;
	}
}

/* the no-trap form, which is every rounding and sign case below */
static void un_v(const char *sig, unsigned char op, uint64_t argbits,
                 uint64_t want, const char *what)
{ un_f(sig, op, argbits, 0, want, what); }

static void bin_f(const char *sig, unsigned char op, uint64_t a, uint64_t b,
                  uint64_t want, const char *what)
{
	unsigned char mod[4096], body[64];
	unsigned n = 0;
	struct ran r;
	union wasm_val args[2];
	body[n++] = 0x20; body[n++] = 0x00;
	body[n++] = 0x20; body[n++] = 0x01;
	body[n++] = op;
	body[n++] = 0x0B;
	args[0].bits = 0; args[0].i64 = a;
	args[1].bits = 0; args[1].i64 = b;
	r = call(mod, build(mod, sig, -1, -1, 0, 0, body, n), args);
	if (r.loaderr) { printf("  FAIL %s: load %s\n", what, wasm_errstr(r.loaderr)); g_fail++; g_checks++; return; }
	g_checks++;
	if (r.trap) { printf("  FAIL %s: unexpected trap `%s`\n", what, wasm_trapstr(r.trap)); g_fail++; }
	else if (r.ret.i64 != want)
		printf("  FAIL %s: got bits 0x%llx want 0x%llx\n", what,
		       (unsigned long long)r.ret.i64, (unsigned long long)want), g_fail++;
}

#define F32(x) ((uint64_t)(x))
#define NEG0_32 0x80000000ull
#define QNAN32  0x7FC00000ull
#define NEG0_64 0x8000000000000000ull
#define QNAN64  0x7FF8000000000000ull

static void part1_traps(void)
{
	printf("the five places this is routinely gotten wrong\n");

	/* ---- 1. integer division and remainder ---------------------------
	 * The asymmetry is the whole point: INT_MIN / -1 TRAPS and INT_MIN %% -1
	 * is ZERO.  Writing `a / b` and `a %% b` in C is undefined for both and,
	 * on x86, raises SIGFPE for both -- so the natural code turns one into a
	 * host fault and the other into a host fault that should have been 0. */
	bin32(0x6D, 0x80000000u, 0xFFFFFFFFu, WASM_TRAP_INT_OVERFLOW, 0, "i32.div_s INT_MIN/-1 traps");
	bin32(0x6F, 0x80000000u, 0xFFFFFFFFu, 0, 0,                    "i32.rem_s INT_MIN/-1 is ZERO, not a trap");
	bin32(0x6D, 1, 0, WASM_TRAP_DIV_ZERO, 0, "i32.div_s by zero traps");
	bin32(0x6E, 1, 0, WASM_TRAP_DIV_ZERO, 0, "i32.div_u by zero traps");
	bin32(0x6F, 1, 0, WASM_TRAP_DIV_ZERO, 0, "i32.rem_s by zero traps");
	bin32(0x70, 1, 0, WASM_TRAP_DIV_ZERO, 0, "i32.rem_u by zero traps");
	bin32(0x6E, 0x80000000u, 0xFFFFFFFFu, 0, 0, "i32.div_u INT_MIN/-1 is 0 and does not trap");
	bin32(0x6D, (uint32_t)-7, 2, 0, (uint32_t)-3, "i32.div_s truncates toward zero");
	bin32(0x6F, (uint32_t)-7, 2, 0, (uint32_t)-1, "i32.rem_s takes the dividend's sign");
	bin64(0x7F, 0x8000000000000000ull, ~0ull, WASM_TRAP_INT_OVERFLOW, 0, "i64.div_s INT_MIN/-1 traps");
	bin64(0x81, 0x8000000000000000ull, ~0ull, 0, 0,                    "i64.rem_s INT_MIN/-1 is ZERO");

	/* ---- 2. the shift count is taken modulo the width ----------------- */
	bin32(0x74, 1, 32, 0, 1,          "i32.shl by 32 is a shift by 0");
	bin32(0x74, 1, 33, 0, 2,          "i32.shl by 33 is a shift by 1");
	bin32(0x76, 0x80000000u, 33, 0, 0x40000000u, "i32.shr_u masks the count");
	bin32(0x75, 0xFFFFFFFFu, 32, 0, 0xFFFFFFFFu, "i32.shr_s by 32 is a shift by 0");
	bin32(0x75, 0x80000000u, 31, 0, 0xFFFFFFFFu, "i32.shr_s is arithmetic");
	bin32(0x77, 0x12345678u, 32, 0, 0x12345678u, "i32.rotl by 32 is the identity");
	bin32(0x77, 0x12345678u, 0,  0, 0x12345678u, "i32.rotl by 0 is the identity");
	bin32(0x78, 0x12345678u, 0,  0, 0x12345678u, "i32.rotr by 0 is the identity");
	bin32(0x77, 0x80000000u, 1,  0, 1u,          "i32.rotl wraps the top bit round");
	bin64(0x86, 1, 64, 0, 1,          "i64.shl by 64 is a shift by 0");
	bin64(0x88, 0x8000000000000000ull, 65, 0, 0x4000000000000000ull, "i64.shr_u by 65 is a shift by 1");
	bin64(0x89, 0x8000000000000000ull, 1, 0, 1ull, "i64.rotl wraps");

	/* ---- 3. float to integer TRAPS, it does not clamp -----------------
	 * And the two failure modes stay two answers: a NaN is "invalid
	 * conversion to integer" and a finite out-of-range value is "integer
	 * overflow".  An implementation that folds them passes half the suite's
	 * assertions for the wrong reason. */
	un_f("f>i", 0xA8, QNAN32,     WASM_TRAP_BAD_CONVERSION, 0, "i32.trunc_f32_s(NaN) is an invalid conversion");
	un_f("f>i", 0xA8, 0x7F800000, WASM_TRAP_INT_OVERFLOW, 0,   "i32.trunc_f32_s(inf) overflows");
	un_f("f>i", 0xA8, 0x4F000000, WASM_TRAP_INT_OVERFLOW, 0,   "i32.trunc_f32_s(2^31) overflows");
	un_f("f>i", 0xA8, 0xCF000000, 0, 0x80000000ull,            "i32.trunc_f32_s(-2^31) is exactly INT_MIN");
	un_f("f>i", 0xA8, 0x4EFFFFFF, 0, 2147483520ull,            "i32.trunc_f32_s at the largest float below 2^31");
	un_f("f>i", 0xA9, 0xBF800000, WASM_TRAP_INT_OVERFLOW, 0,   "i32.trunc_f32_u(-1.0) overflows");
	un_f("f>i", 0xA9, 0xBF666666, 0, 0,                        "i32.trunc_f32_u(-0.9) is 0, not a trap");
	un_f("f>i", 0xA9, 0x4F7FFFFF, 0, 4294967040ull,            "i32.trunc_f32_u near 2^32");
	un_f("d>I", 0xB0, 0x43E0000000000000ull, WASM_TRAP_INT_OVERFLOW, 0, "i64.trunc_f64_s(2^63) overflows");
	un_f("d>I", 0xB0, 0xC3E0000000000000ull, 0, 0x8000000000000000ull, "i64.trunc_f64_s(-2^63) is exact");
	un_f("d>i", 0xAA, 0xC1E0000000200000ull, WASM_TRAP_INT_OVERFLOW, 0, "i32.trunc_f64_s just past -2^31-1 overflows");
	un_f("d>i", 0xAA, 0xC1E0000000100000ull, 0, 0x80000000ull, "i32.trunc_f64_s(-2147483648.5) truncates INTO range");

	/* ---- 4. NaN and the sign of zero in min/max -----------------------
	 * `a < b ? a : b` gets both of these wrong, and both are invisible to a
	 * test that prints its floats as decimals. */
	bin_f("ff>f", 0x96, 0, NEG0_32, NEG0_32, "f32.min(+0,-0) is -0");
	bin_f("ff>f", 0x96, NEG0_32, 0, NEG0_32, "f32.min(-0,+0) is -0");
	bin_f("ff>f", 0x97, 0, NEG0_32, 0,       "f32.max(+0,-0) is +0");
	bin_f("ff>f", 0x97, NEG0_32, 0, 0,       "f32.max(-0,+0) is +0");
	bin_f("ff>f", 0x96, QNAN32, 0x3F800000, QNAN32, "f32.min(NaN,1) is NaN, not 1");
	bin_f("ff>f", 0x97, 0x3F800000, QNAN32, QNAN32, "f32.max(1,NaN) is NaN, not 1");
	bin_f("dd>d", 0xA4, 0, NEG0_64, NEG0_64, "f64.min(+0,-0) is -0");
	bin_f("dd>d", 0xA5, NEG0_64, 0, 0,       "f64.max(-0,+0) is +0");
	bin_f("dd>d", 0xA4, QNAN64, 0x3FF0000000000000ull, QNAN64, "f64.min(NaN,1) is NaN");
	/* copysign and neg are the other two places the sign bit is the answer */
	bin_f("ff>f", 0x98, 0x3F800000, NEG0_32, 0xBF800000ull, "f32.copysign takes the sign of the SECOND operand");
	un_v("f>f", 0x8C, 0, NEG0_32, "f32.neg(+0) is -0");
	un_v("f>f", 0x8B, NEG0_32, 0, "f32.abs(-0) is +0");
	/* the rounding functions, where the sign of a zero result is the trap */
	un_v("f>f", 0x8F, 0xBF000000, NEG0_32, "f32.trunc(-0.5) is -0, not +0");
	un_v("f>f", 0x8D, 0xBF000000, NEG0_32, "f32.ceil(-0.5) is -0");
	un_v("f>f", 0x8E, 0xBF000000, 0xBF800000ull, "f32.floor(-0.5) is -1");
	un_v("f>f", 0x90, 0x40200000, 0x40000000ull, "f32.nearest(2.5) is 2 (ties to EVEN)");
	un_v("f>f", 0x90, 0x40600000, 0x40800000ull, "f32.nearest(3.5) is 4 (ties to EVEN)");
	un_v("f>f", 0x90, 0xBF000000, NEG0_32,       "f32.nearest(-0.5) is -0");
	un_v("f>f", 0x90, 0xBFC00000, 0xC0000000ull, "f32.nearest(-1.5) is -2");

	/* ---- 5. br_table: the last entry is the DEFAULT, and depth counts
	 *        outwards from the innermost block ------------------------- */
	{
		/* (func (param i32) (result i32)
		 *   (block (block (block (br_table 0 1 2 (local.get 0)))
		 *                 (return (i32.const 11)))
		 *          (return (i32.const 22)))
		 *   (i32.const 33))
		 * selector 0 -> depth 0 -> innermost block -> 11
		 * selector 1 -> depth 1 -> 22
		 * selector 2 and anything above -> the DEFAULT entry, depth 2 -> 33 */
		static const unsigned char body[] = {
			0x02, 0x40,                   /* block */
			0x02, 0x40,                   /*  block */
			0x02, 0x40,                   /*   block */
			0x20, 0x00,                   /*    local.get 0 */
			0x0E, 0x02, 0x00, 0x01, 0x02, /*    br_table 0 1 (default 2) */
			0x0B,                         /*   end */
			0x41, 0x0B, 0x0F,             /*   i32.const 11; return */
			0x0B,                         /*  end */
			0x41, 0x16, 0x0F,             /*  i32.const 22; return */
			0x0B,                         /* end */
			0x41, 0x21,                   /* i32.const 33 */
			0x0B
		};
		unsigned char mod[4096];
		unsigned n = build(mod, "i>i", -1, -1, 0, 0, body, sizeof body);
		unsigned k;
		static const uint32_t want[] = { 11, 22, 33, 33, 33 };
		for (k = 0; k < 5; k++) {
			union wasm_val a[1];
			struct ran r;
			char msg[96];
			a[0] = VI(k);
			r = call(mod, n, a);
			sprintf(msg, "br_table selector %u -> %u", k, want[k]);
			g_checks++;
			if (r.loaderr) { printf("  FAIL %s: load %s\n", msg, wasm_errstr(r.loaderr)); g_fail++; }
			else if (r.trap) { printf("  FAIL %s: trap %s\n", msg, wasm_trapstr(r.trap)); g_fail++; }
			else if (r.ret.i32 != want[k]) {
				printf("  FAIL %s: got %u\n", msg, r.ret.i32); g_fail++;
			}
		}
	}
	{
		/* A branch to a LOOP goes to its head, a branch to a BLOCK leaves
		 * it.  Getting that backwards makes every loop run once and every
		 * block run forever, and the arity rule differs too: a loop label
		 * carries no values in the MVP.  (func (param i32) (result i32)
		 *   loop counts local 0 down to zero.) */
		static const unsigned char body[] = {
			0x03, 0x40,                         /* loop */
			0x20, 0x00, 0x41, 0x01, 0x6B,       /*  local.get 0; i32.const 1; i32.sub */
			0x21, 0x00,                         /*  local.set 0 */
			0x20, 0x00, 0x0D, 0x00,             /*  local.get 0; br_if 0 (the loop) */
			0x0B,                               /* end */
			0x20, 0x00,                         /* local.get 0 */
			0x0B
		};
		unsigned char mod[4096];
		union wasm_val a[1];
		struct ran r;
		a[0] = VI(1000);
		r = call(mod, build(mod, "i>i", -1, -1, 0, 0, body, sizeof body), a);
		g_checks++;
		if (r.loaderr || r.trap || r.ret.i32 != 0) {
			printf("  FAIL br_if to a loop label iterates: load=%s trap=%s ret=%u\n",
			       wasm_errstr(r.loaderr), wasm_trapstr(r.trap), r.ret.i32);
			g_fail++;
		}
	}
}

/* ---------------------------------------------------------------- part 2
 * Traps are values, not faults; and the things this interpreter refuses are
 * named rather than answered. */

static void part2_traps_are_clean(void)
{
	printf("\ntraps reach the host as values\n");

	{   /* `unreachable` -- the module asks to trap and the process lives */
		static const unsigned char body[] = { 0x00, 0x0B };
		unsigned char mod[4096];
		struct ran r = call(mod, build(mod, ">", -1, -1, 0, 0, body, sizeof body), 0);
		ck(r.trap == WASM_TRAP_UNREACHABLE, "unreachable is caught, not raised");
		ck(wasm_trap_is_module_fault(r.trap), "unreachable is the module's fault");
	}
	{   /* an out-of-bounds load: one page of memory, an address past it */
		static const unsigned char body[] = {
			0x20, 0x00, 0x28, 0x02, 0x00, 0x0B   /* local.get 0; i32.load a=2 o=0 */
		};
		unsigned char mod[4096];
		unsigned n = build(mod, "i>i", 1, -1, 0, 0, body, sizeof body);
		union wasm_val a[1];
		struct ran r;
		a[0] = VI(65533);     /* 4 bytes from 65533 runs one byte past the page */
		r = call(mod, n, a);
		ck(r.trap == WASM_TRAP_MEM_OOB, "a load one byte past the last page traps");
		a[0] = VI(65532);
		r = call(mod, n, a);
		ck(r.trap == 0 && r.ret.i32 == 0, "the last aligned word in the page reads 0");
		a[0] = VI(0xFFFFFFFFu);
		r = call(mod, n, a);
		ck(r.trap == WASM_TRAP_MEM_OOB,
		   "an address of 0xffffffff traps rather than wrapping to 0");
	}
	{   /* call_indirect: an empty slot, and a wrongly typed one.  The table
	     * holds one entry pointing at f itself, whose type is (i32)->i32;
	     * calling it as ()->() must be a type mismatch and NOT a crash. */
		static const unsigned char elems[] = {
			1,                            /* one segment */
			0,                            /* table 0 */
			0x41, 0x00, 0x0B,             /* offset i32.const 0 */
			1, 0                          /* one function: index 0 */
		};
		static const unsigned char body[] = {
			0x20, 0x00,                   /* local.get 0 -- the callee's argument */
			0x20, 0x00,                   /* local.get 0 -- the TABLE INDEX, on top */
			0x11, 0x01, 0x00,             /* call_indirect type 1, table 0 */
			0x0B
		};
		unsigned char mod[4096];
		/* two identical types; f has type 0 and the call names type 1 */
		unsigned n = build_n(mod, "i>i", 2, -1, 4, elems, sizeof elems, body, sizeof body);
		union wasm_val a[1];
		struct ran r;
		a[0] = VI(1);
		r = call(mod, n, a);
		ck(r.loaderr == 0, "a call_indirect module with a duplicate type validates");
		ck(r.trap == WASM_TRAP_UNINIT_ELEM, "call_indirect through an empty slot traps");
		a[0] = VI(9);
		r = call(mod, n, a);
		ck(r.trap == WASM_TRAP_TABLE_OOB, "call_indirect past the table traps");
		a[0] = VI(0);
		r = call(mod, n, a);
		/* slot 0 IS f, and f's type IS type 0, so this recurses until the
		 * depth bound -- which must be a trap and not a host stack overflow */
		ck(r.trap == WASM_TRAP_EXHAUSTED,
		   "unbounded recursion is `call stack exhausted`, not a host crash");
		ck(r.trap != WASM_TRAP_INDIRECT_TYPE,
		   "call_indirect matches types STRUCTURALLY, not by type index");
	}
	{   /* the refusals that are OURS are not the module's fault */
		ck(!wasm_trap_is_module_fault(WASM_TRAP_UNIMPLEMENTED),
		   "UNIMPLEMENTED is not counted as the module's fault");
		ck(!wasm_trap_is_module_fault(WASM_TRAP_NOMEM),
		   "NOMEM is apparatus, not a verdict");
		ck(strstr(wasm_trapstr(WASM_TRAP_UNIMPLEMENTED), "not implemented") != 0,
		   "an unimplemented opcode has a name that says so");
	}
	{   /* AND THE POLICY IS ENFORCED ONE LAYER UP, which is the strong form:
	     * a post-MVP opcode never reaches the interpreter at all, because the
	     * decoder refuses it by name.  i32.trunc_sat_f32_s is 0xFC 0x00. */
		static const unsigned char body[] = { 0x20, 0x00, 0xFC, 0x00, 0x0B };
		unsigned char mod[4096];
		struct ran r = call(mod, build(mod, "f>i", -1, -1, 0, 0, body, sizeof body), 0);
		ck(r.loaderr == WASM_E_UNSUPPORTED,
		   "a post-MVP opcode is refused at load, by name, and never executed");
		ck(!wasm_err_is_rejection(WASM_E_UNSUPPORTED),
		   "and that refusal is not counted as the module being malformed");
	}
	{   /* memory.grow: the page count is the answer, -1 is the refusal, and
	     * the memory that comes back is zeroed. */
		static const unsigned char body[] = {
			0x20, 0x00, 0x40, 0x00, 0x0B   /* local.get 0; memory.grow */
		};
		unsigned char mod[4096];
		unsigned n = build(mod, "i>i", 1, -1, 0, 0, body, sizeof body);
		union wasm_val a[1];
		struct ran r;
		a[0] = VI(1);
		r = call(mod, n, a);
		ck(r.trap == 0 && r.ret.i32 == 1, "memory.grow returns the OLD page count");
		a[0] = VI(0x10000000u);
		r = call(mod, n, a);
		ck(r.trap == 0 && r.ret.i32 == 0xFFFFFFFFu, "an impossible grow returns -1, it does not trap");
		ck(r.in && wasm_apparatus_count(r.in) == 0,
		   "and refusing it did not consume the arena (apparatus stays 0)");
	}
	{   /* a data segment past the end of memory makes the module
	     * UNINSTANTIABLE, and that is a trap the embedder catches */
		unsigned char mod[4096];
		static const unsigned char body[] = { 0x0B };
		unsigned n = build(mod, ">", 1, -1, 0, 0, body, sizeof body);
		/* append a data section putting 4 bytes at 65534 */
		static const unsigned char data[] = { 1, 0, 0x41, 0xFE, 0xFF, 0x03, 0x0B, 4, 1, 2, 3, 4 };
		mod[n++] = 11; mod[n++] = sizeof data;
		memcpy(mod + n, data, sizeof data); n += sizeof data;
		{
			struct ran r = call(mod, n, 0);
			ck(r.loaderr == 0, "a data segment past the end still VALIDATES");
			ck(r.trap == WASM_TRAP_MEM_OOB,
			   "and fails at instantiation, as a trap the host catches");
		}
	}
}

/* ---------------------------------------------------------------- part 3
 * The spec suite's execution corpus. */

struct spectest_state {
	unsigned char mem[2u * 65536u];
	uint32_t mpages, mmax;
	uint32_t table[20];
	uint32_t tsize, tmax;
};
static struct spectest_state g_spec;

static int host_ignore(void *ctx, const union wasm_val *a, union wasm_val *r)
{ (void)ctx; (void)a; (void)r; return WASM_TRAP_NONE; }

static struct wasm_hostimport g_imports[24];
static unsigned g_nimports;

static void addfn(const char *name)
{
	g_imports[g_nimports].module = "spectest";
	g_imports[g_nimports].name = name;
	g_imports[g_nimports].kind = WASM_IMP_FUNC;
	g_imports[g_nimports].fn = host_ignore;
	g_nimports++;
}
static void addglobal(const char *name, uint8_t vt, uint64_t bits)
{
	g_imports[g_nimports].module = "spectest";
	g_imports[g_nimports].name = name;
	g_imports[g_nimports].kind = WASM_IMP_GLOBAL;
	g_imports[g_nimports].gtype = vt;
	g_imports[g_nimports].gmut = 0;
	g_imports[g_nimports].gval.bits = 0;
	g_imports[g_nimports].gval.i64 = bits;
	g_nimports++;
}

static void spectest_init(void)
{
	unsigned i;
	memset(&g_spec, 0, sizeof g_spec);
	g_spec.mpages = 1; g_spec.mmax = 2;
	g_spec.tsize = 10; g_spec.tmax = 20;
	for (i = 0; i < 20; i++) g_spec.table[i] = 0xFFFFFFFFu;
	memset(g_imports, 0, sizeof g_imports);
	g_nimports = 0;
	addfn("print"); addfn("print_i32"); addfn("print_i64");
	addfn("print_f32"); addfn("print_f64");
	addfn("print_i32_f32"); addfn("print_f64_f64");
	addglobal("global_i32", 0x7F, 666u);
	addglobal("global_i64", 0x7E, 666ull);
	addglobal("global_f32", 0x7D, 0x4426A666ull);          /* 666.6f */
	addglobal("global_f64", 0x7C, 0x4084D4CCCCCCCCCDull);  /* 666.6  */
	g_imports[g_nimports].module = "spectest";
	g_imports[g_nimports].name = "memory";
	g_imports[g_nimports].kind = WASM_IMP_MEMORY;
	g_imports[g_nimports].mem = g_spec.mem;
	g_imports[g_nimports].mpages = &g_spec.mpages;
	g_imports[g_nimports].mmaxpages = &g_spec.mmax;
	g_nimports++;
	g_imports[g_nimports].module = "spectest";
	g_imports[g_nimports].name = "table";
	g_imports[g_nimports].kind = WASM_IMP_TABLE;
	g_imports[g_nimports].table = g_spec.table;
	g_imports[g_nimports].tsize = &g_spec.tsize;
	g_imports[g_nimports].tmax = &g_spec.tmax;
	g_nimports++;
}

static unsigned char *slurp(const char *path, unsigned *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	long n;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return NULL; }
	b = malloc((size_t)n + 1);
	if (!b) { fclose(f); return NULL; }
	if (n && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
	fclose(f);
	*len = (unsigned)n;
	return b;
}

/* "i32:123" / "f32:nanc" -> a value plus a wildcard flag */
enum { W_EXACT = 0, W_NANC = 1, W_NANA = 2 };
struct expv { uint64_t bits; uint8_t vt, wild; };

static int parse_val(const char *s, struct expv *v)
{
	const char *c = strchr(s, ':');
	if (!c) return 0;
	if      (!strncmp(s, "i32", 3)) v->vt = 0x7F;
	else if (!strncmp(s, "i64", 3)) v->vt = 0x7E;
	else if (!strncmp(s, "f32", 3)) v->vt = 0x7D;
	else if (!strncmp(s, "f64", 3)) v->vt = 0x7C;
	else return 0;
	c++;
	v->wild = W_EXACT; v->bits = 0;
	if (!strncmp(c, "nanc", 4)) { v->wild = W_NANC; return 1; }
	if (!strncmp(c, "nana", 4)) { v->wild = W_NANA; return 1; }
	if (*c == '-') v->bits = (uint64_t)strtoll(c, NULL, 10);
	else           v->bits = strtoull(c, NULL, 10);
	if (v->vt == 0x7F || v->vt == 0x7D) v->bits &= 0xFFFFFFFFull;
	return 1;
}

static int parse_list(char *s, struct expv *out, unsigned max, unsigned *n)
{
	char *p = s;
	*n = 0;
	if (!*p) return 1;
	for (;;) {
		char *comma = strchr(p, ',');
		if (comma) *comma = 0;
		if (*n >= max) return 0;
		if (!parse_val(p, &out[(*n)++])) return 0;
		if (!comma) break;
		p = comma + 1;
	}
	return 1;
}

static int val_matches(const struct expv *e, union wasm_val got)
{
	uint64_t g = got.bits;
	if (e->vt == 0x7F || e->vt == 0x7D) g &= 0xFFFFFFFFull;
	switch (e->wild) {
	case W_NANC:
		if (e->vt == 0x7D) return (g & 0x7FFFFFFFull) == 0x7FC00000ull;
		return (g & 0x7FFFFFFFFFFFFFFFull) == 0x7FF8000000000000ull;
	case W_NANA:
		/* "any NaN with the quiet bit set" -- the specification genuinely
		 * leaves the payload open here, and demanding an exact one would
		 * be asserting something that is not true. */
		if (e->vt == 0x7D)
			return (g & 0x7FFFFFFFull) > 0x7F800000ull && (g & 0x00400000ull) != 0;
		return (g & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull &&
		       (g & 0x0008000000000000ull) != 0;
	default:
		return g == e->bits;
	}
}

static unsigned unhex(const char *h, char *out, unsigned max)
{
	unsigned n = 0;
	while (h[0] && h[1] && n < max) {
		int hi = h[0] <= '9' ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
		int lo = h[1] <= '9' ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
		out[n++] = (char)((hi << 4) | lo);
		h += 2;
	}
	return n;
}

struct exectally {
	unsigned run, pass, wrong, no_trap, misnamed, apparatus, noexport;
	unsigned oos_val;    /* the assertion itself uses a post-MVP value type */
	unsigned oos_mod;    /* its module was refused as post-MVP, by name */
	unsigned rejected;   /* its module was refused for an MVP reason */
	unsigned unlink;     /* its module imports one registered earlier */
};

static void run_suite(const char *dir)
{
	char path[1024], line[4096];
	FILE *rf;
	struct exectally T;
	static struct wasm_module mod;
	struct wasm_arena arena;
	struct wasm_instance *in = NULL;
	unsigned char *modbytes = NULL;
	int modstate = 0;          /* 0 ok, 1 out-of-scope, 2 unlinkable, 3 other */
	unsigned nshown = 0, modules = 0;
	const char *verbose = getenv("WASM_VERBOSE");

	memset(&T, 0, sizeof T);
	snprintf(path, sizeof path, "%s/runs.tsv", dir);
	rf = fopen(path, "r");
	if (!rf) {
		printf("\nwasm-exec: SKIPPED -- %s is absent.\n", path);
		printf("           The spec suite is the only independent source of\n");
		printf("           assert_return / assert_trap assertions in this tree.\n");
		printf("           Settle it with:   make wasm-fetch\n");
		return;
	}
	printf("\nspec suite execution corpus: %s\n", dir);

	while (fgets(line, sizeof line, rf)) {
		char *kind, *fld, *args, *exp, *org, *tab;
		if (line[0] == '#') continue;
		kind = line;
		tab = strchr(kind, '\t'); if (!tab) continue; *tab = 0; fld = tab + 1;
		tab = strchr(fld, '\t');  if (!tab) continue; *tab = 0; args = tab + 1;
		tab = strchr(args, '\t'); if (!tab) continue; *tab = 0; exp = tab + 1;
		tab = strchr(exp, '\t');  if (!tab) continue; *tab = 0; org = tab + 1;
		tab = strchr(org, '\n');  if (tab) *tab = 0;

		if (!strcmp(kind, "mod") || !strcmp(kind, "modtrap")) {
			unsigned n;
			int e;
			modules++;
			in = NULL;
			modstate = 3;
			snprintf(path, sizeof path, "%s/%s", dir, fld);
			/* THE MODULE BYTES OUTLIVE THE LOAD, and the first version of
			 * this loop freed them right after wasm_load returned.  A
			 * `struct wasm_module` is a set of SPANS INTO THE CALLER'S
			 * BUFFER -- every function body, every constant expression and
			 * every export name is an offset, which is the property that
			 * lets the decoder allocate almost nothing.  Freeing it read
			 * as the interpreter failing: 707 modules "failed to
			 * instantiate" with `unexpected end` and 41 with the boundary
			 * cross-check, and standalone every one of them was fine.
			 * Suspect the apparatus first. */
			free(modbytes);
			modbytes = slurp(path, &n);
			if (!modbytes) continue;
			/* One arena per module, so the instance and everything the
			 * decoder built for it die together and a stale pointer from
			 * the previous module cannot survive into this one. */
			wasm_arena_init(&arena, g_arena, ARENA_BYTES);
			spectest_init();
			e = wasm_load(modbytes, n, &mod, &arena);
			if (e == WASM_E_UNSUPPORTED) { modstate = 1; continue; }
			if (e == WASM_E_NOMEM || e == WASM_E_LIMIT) { modstate = 5; T.apparatus++; continue; }
			if (e) {
				/* We rejected it for an MVP reason.  Kept apart from
				 * out-of-scope because the two say different things: one
				 * is a proposal we do not implement, the other is a
				 * verdict this library reached, and the set of those is
				 * already ratcheted by tests/wasm-spec.baseline in the
				 * validator gate.  Folding them together would hide a
				 * regression there behind a number that only ever grows
				 * for good reasons. */
				modstate = 4;
				if (verbose) printf("  module rejected  %-28s %s\n", org, wasm_errstr(e));
				continue;
			}
			e = wasm_instantiate(&in, &mod, &arena, g_imports, g_nimports);
			if (e == WASM_TRAP_UNLINKABLE) { modstate = 2; in = NULL; continue; }
			if (e == WASM_TRAP_NOMEM) { modstate = 5; in = NULL; T.apparatus++; continue; }
			if (!strcmp(kind, "modtrap")) {
				T.run++;
				if (e && wasm_trap_is_module_fault(e)) T.pass++;
				else { T.wrong++;
					if (verbose || nshown++ < 25)
						printf("  UNINSTANTIABLE EXPECTED  %s: got `%s`\n", org, wasm_trapstr(e)); }
				modstate = 5; in = NULL;
				continue;
			}
			if (e) { modstate = 5; in = NULL; continue; }
			modstate = 0;
			continue;
		}
		if (!strcmp(kind, "skip")) { T.oos_val++; continue; }

		/* every remaining row acts on the current instance */
		if (modstate == 1) { T.oos_mod++; continue; }
		if (modstate == 2) { T.unlink++; continue; }
		if (modstate == 4) { T.rejected++; continue; }
		if (modstate != 0 || !in) { T.apparatus++; continue; }

		{
			char name[512];
			unsigned nlen = unhex(fld, name, sizeof name);
			struct expv av[16], ev[8];
			unsigned na = 0, ne = 0, i;
			union wasm_val args_v[16], rets[8];
			unsigned idx;
			int e;

			if (!strcmp(kind, "get")) {
				T.run++;
				if (!parse_list(exp, ev, 8, &ne) ||
				    !wasm_export_index(in, name, nlen, WASM_EXT_GLOBAL, &idx)) {
					T.noexport++; continue;
				}
				if (ne == 1 && val_matches(&ev[0], wasm_global_get(in, idx))) T.pass++;
				else { T.wrong++;
					if (verbose || nshown++ < 25) printf("  WRONG GLOBAL %s\n", org); }
				continue;
			}

			if (!parse_list(args, av, 16, &na) || !parse_list(exp, ev, 8, &ne)) {
				T.oos_val++; continue;
			}
			T.run++;
			if (!wasm_export_index(in, name, nlen, WASM_EXT_FUNC, &idx)) {
				T.noexport++; T.run--; continue;
			}
			for (i = 0; i < na; i++) { args_v[i].bits = 0; args_v[i].i64 = av[i].bits; }
			for (i = 0; i < 8; i++) rets[i].bits = 0;
			e = wasm_invoke(in, idx, args_v, rets);

			if (e == WASM_TRAP_NOMEM || (in && wasm_apparatus_count(in))) {
				T.apparatus++; T.run--; continue;
			}
			if (!strcmp(kind, "ret") || !strcmp(kind, "act")) {
				if (e) {
					T.no_trap++;   /* trapped where it should have returned */
					if (verbose || nshown++ < 25)
						printf("  UNEXPECTED TRAP  %-28s %s\n", org, wasm_trapstr(e));
					continue;
				}
				for (i = 0; i < ne; i++)
					if (!val_matches(&ev[i], rets[i])) break;
				if (i == ne) T.pass++;
				else {
					T.wrong++;
					if (verbose || nshown++ < 25)
						printf("  WRONG VALUE      %-28s got 0x%llx want 0x%llx\n",
						       org, (unsigned long long)rets[i].bits,
						       (unsigned long long)ev[i].bits);
				}
			} else {   /* trap / exh */
				if (!e) {
					T.no_trap++;
					if (verbose || nshown++ < 25)
						printf("  DID NOT TRAP     %-28s expected `%s`\n", org, exp);
				} else if (!wasm_trap_is_module_fault(e)) {
					/* WE refused rather than the module trapping --
					 * UNIMPLEMENTED, DESYNC or an unlinkable host
					 * call.  That is never a pass. */
					T.apparatus++;
					if (verbose || nshown++ < 25)
						printf("  OUR REFUSAL      %-28s %s\n", org, wasm_trapstr(e));
				} else if (exp[0] && strcmp(wasm_trapstr(e), exp)) {
					T.misnamed++;
					if (verbose || nshown++ < 25)
						printf("  TRAP MISNAMED    %-28s got `%s` want `%s`\n",
						       org, wasm_trapstr(e), exp);
				} else {
					T.pass++;
				}
			}
		}
	}
	fclose(rf);
	free(modbytes);

	printf("\nmodules instantiated for execution: %u\n", modules);
	printf("%-14s %8s\n", "outcome", "count");
	printf("%-14s %8u\n", "PASSED",        T.pass);
	printf("%-14s %8u   <- wrong value returned\n", "wrong",    T.wrong);
	printf("%-14s %8u   <- trapped when it should not, or the reverse\n", "trap-mismatch", T.no_trap);
	printf("%-14s %8u   <- trapped, with a DIFFERENT trap than the suite named\n", "trap-misnamed", T.misnamed);
	printf("%-14s %8u   <- the ASSERTION uses a post-MVP value type, or names\n"
	       "%-14s %8s      an earlier module; NOT a pass and NOT a failure\n",
	       "oos-assertion", T.oos_val, "", "");
	printf("%-14s %8u   <- its MODULE is post-MVP, refused by name\n", "oos-module", T.oos_mod);
	printf("%-14s %8u   <- its module was refused for an MVP reason; the set of\n"
	       "%-14s %8s      those is ratcheted by tests/wasm-spec.baseline\n",
	       "mod-rejected", T.rejected, "", "");
	printf("%-14s %8u   <- imports a module registered earlier in the .wast\n", "unlinkable", T.unlink);
	printf("%-14s %8u   <- the export named does not exist here\n", "no-export", T.noexport);
	printf("%-14s %8u   <- OUR arena or OUR bound; any value here fails the gate\n", "APPARATUS", T.apparatus);
	printf("\nspec assertions: %u of %u PASSED\n", T.pass, T.run);

	if (T.wrong || T.no_trap || T.misnamed || T.apparatus || T.noexport) g_fail++;
}

int main(int argc, char **argv)
{
	g_arena = malloc(ARENA_BYTES);
	if (!g_arena) { printf("wasm-exec: cannot allocate the arena\n"); return 2; }

	part1_traps();
	part2_traps_are_clean();

	if (argc > 1) run_suite(argv[1]);
	else {
		printf("\nwasm-exec: spec corpus SKIPPED -- no directory given.\n");
		printf("           Settle it with:   make wasm-fetch\n");
	}

	printf("\nbuilt-in: %d checks, %s\n", g_checks, g_fail ? "FAIL" : "ok");
	return g_fail ? 1 : 0;
}
