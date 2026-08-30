/* WebAssembly MVP interpreter.  See wasm_exec.h for why this is an
 * interpreter rather than a port, and why a trap is a value here.
 *
 * FIVE PLACES THIS IS ROUTINELY GOTTEN WRONG, and the reason they are called
 * out rather than left to the reader is that every one of them is a SILENT
 * WRONG ANSWER instead of a crash -- the module runs, produces a number, and
 * the number is not the one the specification names:
 *
 *   1. Integer division and remainder.  Division by zero traps.  INT_MIN / -1
 *      TRAPS for div_s and returns ZERO for rem_s.  The natural C expression
 *      is undefined behaviour for both and, on x86, raises SIGFPE for both --
 *      so "just write a / b" turns a wasm trap into a host fault for one of
 *      them and a host fault into a wrong wasm answer for the other.
 *   2. The shift count is taken MODULO the width, always, including for
 *      rotates.  A C shift by >= the width is undefined, so the mask is not
 *      an optimisation that the hardware happens to do -- it is the semantics.
 *   3. Float-to-integer truncation TRAPS on NaN and on out-of-range.  It does
 *      NOT clamp.  The saturating forms are a separate post-MVP proposal
 *      (0xFC 0x00..0x07) and the decoder refuses them by name.  The range
 *      test has to be written on the FLOAT side with exactly representable
 *      bounds; casting first and checking after is a check that runs on the
 *      already-undefined result.
 *   4. NaN and the sign of zero in f32/f64 min/max.  min(+0,-0) is -0 and
 *      max(+0,-0) is +0, neither of which `a < b ? a : b` produces; and a NaN
 *      operand makes the result a NaN rather than the other operand, which is
 *      the opposite of C's fmin.
 *   5. br_table: the last entry is the DEFAULT, not a target, and every label
 *      depth is counted from the innermost enclosing block outwards.
 *
 * Each of them is a named case in the built-in half of tests/unit/
 * wasm_exec_test.c as well as being covered by the spec suite, because a
 * property whose only witness is a downloaded corpus stops being tested the
 * day the download fails.
 *
 * THE INSTRUCTION-BOUNDARY CROSS-CHECK.  wasm_valid.c owns the only
 * description of what immediates each opcode carries, and the interpreter has
 * to read those immediates too -- two doors on one jar, the shape CLAUDE.md
 * rule 3 is about.  It cannot be avoided (executing requires reading), so it
 * is made WATCHABLE instead: the pre-pass walks each body through wasm_walk's
 * sink and records a bit per byte marking where instructions begin, and the
 * interpreter checks its cursor against that set before every dispatch.  A
 * disagreement about the length of an immediate is then WASM_TRAP_DESYNC
 * naming the opcode, instead of an opcode byte read out of the middle of a
 * LEB128 and executed.  WASM_NEGCTL_IMM injects exactly that mistake so the
 * check can be watched firing.
 */

#include "wasm_int.h"
#include "wasm_exec.h"

/* An implementation bound, stated rather than discovered: the largest linear
 * memory this interpreter will build.  A module asking for more as its MINIMUM
 * fails instantiation with WASM_TRAP_NOMEM, which is apparatus and is loud; a
 * memory.grow past it returns -1, which is what the specification permits a
 * grow to do for any reason at all.  Distinguishing those two is the whole
 * point -- "we said no because the module's own maximum said no" and "we said
 * no because we ran out" must never print the same. */
#ifndef WASM_MEM_PAGES_CAP
#define WASM_MEM_PAGES_CAP 512u          /* 32 MiB */
#endif
#define WASM_PAGE 65536u

#define NOFUNC 0xFFFFFFFFu
#define NOPOS  0xFFFFFFFFu

/* ---- freestanding scaffolding ----------------------------------------- */

static void zero(void *p, uint32_t n)
{
	unsigned char *q = (unsigned char *)p;
	uint32_t i;
	for (i = 0; i < n; i++) q[i] = 0;
}

union f32bits { float f; uint32_t u; };
union f64bits { double d; uint64_t u; };

static float    u2f(uint32_t u) { union f32bits b; b.u = u; return b.f; }
static uint32_t f2u(float f)    { union f32bits b; b.f = f; return b.u; }
static double   u2d(uint64_t u) { union f64bits b; b.u = u; return b.d; }
static uint64_t d2u(double d)   { union f64bits b; b.d = d; return b.u; }

/* The canonical NaN of each width.  The spec permits any NaN as the result of
 * an operation with a NaN operand, and requires the canonical one when no
 * operand was a NaN (inf - inf, 0/0, sqrt of a negative).  Hardware arithmetic
 * already does both; these are for the operations written out longhand
 * below -- min, max and the rounding functions. */
#define F32_CANON_NAN 0x7FC00000u
#define F64_CANON_NAN 0x7FF8000000000000ull

static int f32_isnan(float f) { return (f2u(f) & 0x7FFFFFFFu) > 0x7F800000u; }
static int f64_isnan(double d) { return (d2u(d) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }

/* ---- rounding, longhand ------------------------------------------------
 * No libm: this library is freestanding.  __builtin_sqrt{,f} is the one
 * exception and is a single hardware instruction (build -fno-math-errno, or
 * clang emits a libcall for the errno path -- measured, it does so for
 * --target=x86_64-elf).  Everything else here is written out, and the reason
 * it is written out rather than reached for is the SIGN OF ZERO: trunc(-0.5)
 * is -0.0, and `(float)(int)x` gives +0.0.  A rounding function that loses
 * that is wrong in a way no test of magnitudes can see. */

static float f32_trunc(float x)
{
	uint32_t u = f2u(x), sign = u & 0x80000000u;
	float t;
	/* A NaN in must be a QUIET NaN out.  The specification's result set for
	 * these operators is every NaN whose payload is at least the canonical
	 * one -- which is exactly the NaNs with the quiet bit set -- so passing
	 * a SIGNALLING NaN straight through is not conformant, and it is the
	 * failure the suite catches with `nan:arithmetic` (f32.wast:2456 and
	 * seven siblings, one per rounding operator per width).  Hardware
	 * arithmetic quiets for free, which is why only the operators written
	 * out longhand need this line. */
	if (f32_isnan(x)) return u2f(F32_CANON_NAN);
	if (!((u & 0x7FFFFFFFu) < 0x4B000000u)) return x;   /* |x| >= 2^23, or inf */
	t = (float)(int32_t)x;                              /* exact: |x| < 2^23 */
	return u2f((f2u(t) & 0x7FFFFFFFu) | sign);
}

static double f64_trunc(double x)
{
	uint64_t u = d2u(x), sign = u & 0x8000000000000000ull;
	double t;
	if (f64_isnan(x)) return u2d(F64_CANON_NAN);   /* see f32_trunc */
	if (!((u & 0x7FFFFFFFFFFFFFFFull) < 0x4330000000000000ull)) return x;  /* |x| >= 2^52 */
	t = (double)(int64_t)x;
	return u2d((d2u(t) & 0x7FFFFFFFFFFFFFFFull) | sign);
}

static float  f32_floor(float x)  { float t = f32_trunc(x); return (t > x) ? t - 1.0f : t; }
static double f64_floor(double x) { double t = f64_trunc(x); return (t > x) ? t - 1.0 : t; }
static float  f32_ceil(float x)   { float t = f32_trunc(x); return (t < x) ? t + 1.0f : t; }
static double f64_ceil(double x)  { double t = f64_trunc(x); return (t < x) ? t + 1.0 : t; }

/* roundTiesToEven.  The tie case is the whole content of this function: 2.5
 * rounds to 2 and 3.5 rounds to 4, so "add a half and truncate" is wrong on
 * every other integer, and it is wrong in a way that looks right in a spot
 * check because half the cases agree. */
static float f32_nearest(float x)
{
	float t, d;
	if (f32_isnan(x)) return u2f(F32_CANON_NAN);
	t = f32_trunc(x);
	d = x - t;
	if (d > 0.5f) return t + 1.0f;
	if (d < -0.5f) return t - 1.0f;
	if (d == 0.5f || d == -0.5f) {
		int64_t i = (int64_t)t;
		if (i & 1) return (d > 0.0f) ? t + 1.0f : t - 1.0f;
	}
	return t;
}

static double f64_nearest(double x)
{
	double t, d;
	if (f64_isnan(x)) return u2d(F64_CANON_NAN);
	t = f64_trunc(x);
	d = x - t;
	if (d > 0.5) return t + 1.0;
	if (d < -0.5) return t - 1.0;
	if (d == 0.5 || d == -0.5) {
		int64_t i = (int64_t)t;
		if (i & 1) return (d > 0.0) ? t + 1.0 : t - 1.0;
	}
	return t;
}

/* trap #4.  Written from the specification's four clauses rather than from
 * `a < b ? a : b`, which gets the zero case AND the NaN case backwards. */
static float f32_min(float a, float b)
{
	if (f32_isnan(a) || f32_isnan(b)) return u2f(F32_CANON_NAN);
	if (a == 0.0f && b == 0.0f) return u2f((f2u(a) | f2u(b)) & 0x80000000u ? 0x80000000u : 0u);
	return a < b ? a : b;
}
static float f32_max(float a, float b)
{
	if (f32_isnan(a) || f32_isnan(b)) return u2f(F32_CANON_NAN);
	if (a == 0.0f && b == 0.0f) return u2f((f2u(a) & f2u(b)) & 0x80000000u ? 0x80000000u : 0u);
	return a > b ? a : b;
}
static double f64_min(double a, double b)
{
	if (f64_isnan(a) || f64_isnan(b)) return u2d(F64_CANON_NAN);
	if (a == 0.0 && b == 0.0)
		return u2d((d2u(a) | d2u(b)) & 0x8000000000000000ull ? 0x8000000000000000ull : 0ull);
	return a < b ? a : b;
}
static double f64_max(double a, double b)
{
	if (f64_isnan(a) || f64_isnan(b)) return u2d(F64_CANON_NAN);
	if (a == 0.0 && b == 0.0)
		return u2d((d2u(a) & d2u(b)) & 0x8000000000000000ull ? 0x8000000000000000ull : 0ull);
	return a > b ? a : b;
}

static uint32_t clz32(uint32_t v) { uint32_t n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static uint32_t ctz32(uint32_t v) { uint32_t n = 0; if (!v) return 32; while (!(v & 1u)) { v >>= 1; n++; } return n; }
static uint32_t pop32(uint32_t v) { uint32_t n = 0; while (v) { n += v & 1u; v >>= 1; } return n; }
static uint64_t clz64(uint64_t v) { uint64_t n = 0; if (!v) return 64; while (!(v & 0x8000000000000000ull)) { v <<= 1; n++; } return n; }
static uint64_t ctz64(uint64_t v) { uint64_t n = 0; if (!v) return 64; while (!(v & 1ull)) { v >>= 1; n++; } return n; }
static uint64_t pop64(uint64_t v) { uint64_t n = 0; while (v) { n += v & 1ull; v >>= 1; } return n; }

/* trap #2: the count is masked, and a rotate by zero must not become a shift
 * by the full width (which is undefined in C and, on x86, a no-op that
 * silently returns the wrong half). */
static uint32_t rotl32(uint32_t v, uint32_t r) { r &= 31; return r ? ((v << r) | (v >> (32 - r))) : v; }
static uint32_t rotr32(uint32_t v, uint32_t r) { r &= 31; return r ? ((v >> r) | (v << (32 - r))) : v; }
static uint64_t rotl64(uint64_t v, uint64_t r) { r &= 63; return r ? ((v << r) | (v >> (64 - r))) : v; }
static uint64_t rotr64(uint64_t v, uint64_t r) { r &= 63; return r ? ((v >> r) | (v << (64 - r))) : v; }

/* ---- the instance ------------------------------------------------------ */

struct ctrlent { uint32_t start, els, end; };

struct fninfo {
	struct ctrlent *ents;
	uint32_t nents;
	uint8_t *bmap;              /* one bit per body byte: an instruction starts here */
	uint32_t off, len;          /* the expression's extent in module bytes */
};

struct rframe {
	uint32_t cont;              /* where a branch to this label resumes */
	uint32_t vheight;           /* operand stack height on entry */
	uint32_t ent;               /* index into fninfo.ents, or NOPOS */
	uint8_t  arity;             /* values the label carries (0 for a loop) */
	uint8_t  is_loop;
};

struct wasm_instance {
	const struct wasm_module *m;
	struct wasm_arena *a;

	uint8_t *mem; uint32_t mem_pages, mem_maxpages; uint8_t mem_present;
	uint32_t *mempages_ext, *memmax_ext;      /* set when the memory is the HOST's */
	uint8_t **membase_ext;                    /* ditto: where to write a moved base */

	uint32_t *table; uint32_t table_size, table_max; uint8_t table_present;
	uint32_t *tsize_ext, *tmax_ext;

	union wasm_val *globals;                  /* total_globals entries */

	const struct wasm_hostimport **hostfn;    /* nimp_funcs entries */

	struct fninfo *fn;                        /* ncode entries */

	union wasm_val *stackraw, *stack;
	uint32_t sp, scap;
	struct rframe *ctrl; uint32_t ctop, ccap;
	uint32_t depth;

	uint32_t apparatus;
	uint8_t  trap_op;
};

const char *wasm_trapstr(int c)
{
	switch (c) {
	case WASM_TRAP_NONE:          return "ok";
	case WASM_TRAP_UNREACHABLE:   return "unreachable";
	case WASM_TRAP_MEM_OOB:       return "out of bounds memory access";
	case WASM_TRAP_TABLE_OOB:     return "undefined element";
	case WASM_TRAP_UNINIT_ELEM:   return "uninitialized element";
	case WASM_TRAP_INDIRECT_TYPE: return "indirect call type mismatch";
	case WASM_TRAP_DIV_ZERO:      return "integer divide by zero";
	case WASM_TRAP_INT_OVERFLOW:  return "integer overflow";
	case WASM_TRAP_BAD_CONVERSION:return "invalid conversion to integer";
	case WASM_TRAP_EXHAUSTED:     return "call stack exhausted";
	case WASM_TRAP_HOST:          return "host function trapped";
	case WASM_TRAP_UNIMPLEMENTED: return "REFUSED: opcode not implemented";
	case WASM_TRAP_UNLINKABLE:    return "REFUSED: unknown import";
	case WASM_TRAP_DESYNC:        return "REFUSED: instruction boundary cross-check failed";
	case WASM_TRAP_NOMEM:         return "APPARATUS: interpreter arena exhausted";
	default:                      return wasm_errstr(c);
	}
}

int wasm_trap_is_module_fault(int c)
{
	return c >= WASM_TRAP_UNREACHABLE && c <= WASM_TRAP_HOST;
}

uint32_t wasm_apparatus_count(const struct wasm_instance *in) { return in->apparatus; }
uint8_t  wasm_trap_opcode(const struct wasm_instance *in) { return in->trap_op; }

/* ---- the pre-pass ------------------------------------------------------
 * Two walks of each body through wasm_walk's sink: one to count the control
 * instructions and the bitmap size, one to fill them.  Nothing here decodes an
 * immediate; the walk does that, once, in the file that owns the description. */

struct prepass {
	struct fninfo *fi;
	struct ctrlent *ents;
	uint32_t nents, cap;
	uint32_t *stack;           /* indices into ents */
	uint32_t depth, dcap;
	uint8_t *bmap;
	uint32_t base, len;
	int overflow;
};

static void pp_insn(void *ctx, uint32_t pos, uint8_t op)
{
	struct prepass *p = (struct prepass *)ctx;
	uint32_t rel = pos - p->base;

	if (p->bmap && rel < p->len) p->bmap[rel >> 3] |= (uint8_t)(1u << (rel & 7));

	if (op == 0x02 || op == 0x03 || op == 0x04) {          /* block loop if */
		if (p->ents) {
			if (p->nents >= p->cap || p->depth >= p->dcap) { p->overflow = 1; return; }
			p->ents[p->nents].start = pos;
			p->ents[p->nents].els = NOPOS;
			p->ents[p->nents].end = NOPOS;
			p->stack[p->depth++] = p->nents;
		}
		p->nents++;
	} else if (op == 0x05) {                               /* else */
		if (p->ents && p->depth) p->ents[p->stack[p->depth - 1]].els = pos;
	} else if (op == 0x0B) {                               /* end */
		if (p->ents && p->depth) p->ents[p->stack[--p->depth]].end = pos;
	}
}

/* binary search: the entries are recorded in a forward walk, so `start` is
 * strictly increasing. */
static uint32_t ent_at(const struct fninfo *fi, uint32_t pos)
{
	uint32_t lo = 0, hi = fi->nents;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (fi->ents[mid].start == pos) return mid;
		if (fi->ents[mid].start < pos) lo = mid + 1; else hi = mid;
	}
	return NOPOS;
}

static int build_fninfo(struct wasm_instance *in, uint32_t k)
{
	const struct wasm_code *c = &in->m->code[k];
	struct fninfo *fi = &in->fn[k];
	struct prepass p;
	struct rd r;
	int e;

	fi->off = c->expr.off;
	fi->len = c->expr.len;

	zero(&p, sizeof p);
	p.base = fi->off; p.len = fi->len;
	r.p = in->m->bytes; r.i = fi->off; r.n = fi->off + fi->len;
	e = wasm_walk(&r, 0, &(struct wasm_sink){ pp_insn, &p });
	if (e) return e;

	fi->nents = p.nents;
	fi->ents = wasm_arena_alloc(in->a, p.nents ? p.nents : 1, sizeof *fi->ents);
	if (!fi->ents) return WASM_TRAP_NOMEM;
	fi->bmap = wasm_arena_alloc(in->a, (fi->len + 7) / 8 + 1, 1);
	if (!fi->bmap) return WASM_TRAP_NOMEM;

	zero(&p, sizeof p);
	p.base = fi->off; p.len = fi->len;
	p.ents = fi->ents; p.cap = fi->nents;
	p.bmap = fi->bmap;
	p.stack = wasm_arena_alloc(in->a, fi->nents ? fi->nents : 1, sizeof *p.stack);
	if (!p.stack) return WASM_TRAP_NOMEM;
	p.dcap = fi->nents;
	r.p = in->m->bytes; r.i = fi->off; r.n = fi->off + fi->len;
	e = wasm_walk(&r, 0, &(struct wasm_sink){ pp_insn, &p });
	if (e) return e;
	if (p.overflow || p.nents != fi->nents) return WASM_TRAP_DESYNC;
	return WASM_TRAP_NONE;
}

/* ---- linear memory ----------------------------------------------------- */

static uint64_t mem_bytes(const struct wasm_instance *in)
{
	return (uint64_t)in->mem_pages * WASM_PAGE;
}

uint8_t *wasm_mem_bytes(const struct wasm_instance *in, uint32_t *n)
{
	if (n) *n = (uint32_t)mem_bytes(in);
	return in->mem;
}

/* Grow by `delta` pages.  Returns the OLD page count, or 0xFFFFFFFF.
 *
 * The arena is a bump allocator, so growth is allocate-and-copy rather than
 * realloc.  That wastes the old block, which is fine for the lifetime of an
 * instance and is the honest cost of having exactly one allocator.  What is
 * NOT fine is confusing the two reasons a grow can fail, so they are counted
 * apart: past the module's own maximum is the module's answer, past our arena
 * is ours and increments the apparatus count the gate refuses to ignore. */
static uint32_t mem_grow(struct wasm_instance *in, uint32_t delta)
{
	uint32_t old = in->mem_pages;
	uint64_t np = (uint64_t)old + delta;
	uint8_t *nb;
	uint64_t i, ob;

	if (!in->mem_present) return NOFUNC;
	if (delta == 0) return old;
	if (np > in->mem_maxpages || np > 65536u) return NOFUNC;
	if (np > WASM_MEM_PAGES_CAP) return NOFUNC;   /* a STATED bound; see the top */

	nb = wasm_arena_alloc(in->a, (uint32_t)np, WASM_PAGE);
	if (!nb) { in->apparatus++; return NOFUNC; }
	ob = mem_bytes(in);
	for (i = 0; i < ob; i++) nb[i] = in->mem[i];
	in->mem = nb;
	in->mem_pages = (uint32_t)np;
	if (in->mempages_ext) *in->mempages_ext = in->mem_pages;
	/* The base moved, so tell the host BEFORE the page count is believed.
	 * Updating *mempages_ext without this is the defect wasm_exec.h's `memp`
	 * comment describes: the right new length over the old bytes. */
#ifndef WASM_NEGCTL_MEMP
	if (in->membase_ext) *in->membase_ext = nb;
#endif
	/* NEGATIVE CONTROL (WASM_NEGCTL_MEMP): drop the write-back and keep the
	 * page-count update above.  That is precisely the state this file was in
	 * on 2026-08-30, and the shape is the reason it survived: the host is told
	 * the memory is now larger and is left pointing at the smaller, older
	 * block, so every length is right and the bytes are wrong. */
	return old;
}

/* Bounds written as a SUBTRACTION against what is left, never as `ea + w > n`
 * -- c/kernel/module/modelf.c's rule, and here `ea` is 64-bit precisely so
 * that base + offset cannot wrap before the comparison happens. */
static int mem_range_ok(const struct wasm_instance *in, uint64_t ea, uint32_t w)
{
	uint64_t n = mem_bytes(in);
	if (!in->mem_present) return 0;
	if ((uint64_t)w > n) return 0;
	return ea <= n - w;
}

/* ---- function types ----------------------------------------------------
 * STRUCTURAL equality, not index equality.  A module may declare the same
 * signature twice, and call_indirect's check is against the TYPE, so
 * comparing type indices refuses a call the specification requires to
 * succeed -- and does so only for modules that happen to have a duplicate
 * type, which is why it survives casual testing. */
static int functype_eq(const struct wasm_functype *a, const struct wasm_functype *b)
{
	uint32_t i;
	if (a == b) return 1;
	if (a->nparams != b->nparams || a->nresults != b->nresults) return 0;
	for (i = 0; i < a->nparams; i++) if (a->params[i] != b->params[i]) return 0;
	for (i = 0; i < a->nresults; i++) if (a->results[i] != b->results[i]) return 0;
	return 1;
}

const struct wasm_functype *wasm_func_signature(const struct wasm_instance *in, uint32_t f)
{
	if (f >= in->m->total_funcs) return 0;
	return &in->m->types[in->m->functype_of[f]];
}

union wasm_val wasm_global_get(const struct wasm_instance *in, uint32_t g)
{
	union wasm_val v;
	v.bits = 0;
	if (g < in->m->total_globals) v = in->globals[g];
	return v;
}

/* ---- accessors the JS API needs, and nothing inside this file uses -------
 *
 * WebAssembly.Table.prototype.get/set and WebAssembly.Global.prototype.value
 * are reads and writes of state this file already owns; without these the
 * binding would have to keep a SECOND copy of the table and the globals
 * alongside the interpreter's, and CLAUDE.md's "one jar, two doors" is the
 * whole history of what that costs.  They are deliberately thin: every one is
 * a bounds check and a move, and each returns a status rather than clamping,
 * because a silently-clamped index is the shape this tree keeps paying for. */

uint32_t wasm_table_size(const struct wasm_instance *in)
{
	return in->table_present ? in->table_size : 0;
}

int wasm_table_get(const struct wasm_instance *in, uint32_t i, uint32_t *fidx)
{
	if (!in->table_present || i >= in->table_size) return -1;
	if (fidx) *fidx = in->table[i];       /* WASM_NOFUNC means the slot is null */
	return 0;
}

int wasm_table_set(struct wasm_instance *in, uint32_t i, uint32_t fidx)
{
	if (!in->table_present || i >= in->table_size) return -1;
	/* NOFUNC (a null slot) is always allowed; anything else must name a real
	 * function, or an indirect call through it would index off the end of
	 * the function space. */
	if (fidx != NOFUNC && fidx >= in->m->total_funcs) return -1;
	in->table[i] = fidx;
	return 0;
}

int wasm_global_set(struct wasm_instance *in, uint32_t g, union wasm_val v)
{
	if (g >= in->m->total_globals) return -1;
	/* Mutability is NOT checked here and that is on purpose: it is a property
	 * of the module's global TYPE, which the caller can already read, and the
	 * JS API must throw a TypeError for an immutable global rather than
	 * report a numeric failure.  Enforcing it in both places is two doors on
	 * one jar; this is the door that does not get to decide. */
	in->globals[g] = v;
	return 0;
}

uint32_t wasm_mem_pages(const struct wasm_instance *in)
{
	return in->mem_present ? in->mem_pages : 0;
}

/* Grow through the interpreter's own path rather than reallocating behind it.
 * A JS `memory.grow(n)` on a memory that an instance is using must move the
 * same bytes the `memory.grow` OPCODE would move, update the same page count,
 * and write back through the same `memp` -- doing it host-side would leave the
 * instance holding the pre-grow base, which is the exact defect wasm_exec.h's
 * `memp` comment describes, only in the other direction. */
int wasm_mem_grow(struct wasm_instance *in, uint32_t delta, uint32_t *oldpages)
{
	uint32_t old;
	if (!in->mem_present) return -1;
	old = mem_grow(in, delta);
	if (old == NOFUNC) return -1;
	if (oldpages) *oldpages = old;
	return 0;
}

/* ---- constant expressions --------------------------------------------- */

static int const_eval(struct wasm_instance *in, struct wasm_span s, union wasm_val *out)
{
	struct rd r;
	uint8_t op;
	int e;
	r.p = in->m->bytes; r.i = s.off; r.n = s.off + s.len;
	out->bits = 0;
	e = wasm_rd_u8(&r, &op);
	if (e) return e;
	switch (op) {
	case 0x41: { int32_t v; e = wasm_rd_s32(&r, &v); if (e) return e; out->i32 = (uint32_t)v; break; }
	case 0x42: { int64_t v; e = wasm_rd_s64(&r, &v); if (e) return e; out->i64 = (uint64_t)v; break; }
	case 0x43: { uint32_t u = 0, i;
		     for (i = 0; i < 4; i++) { uint8_t b; e = wasm_rd_u8(&r, &b); if (e) return e; u |= (uint32_t)b << (8 * i); }
		     out->i32 = u; break; }
	case 0x44: { uint64_t u = 0, i;
		     for (i = 0; i < 8; i++) { uint8_t b; e = wasm_rd_u8(&r, &b); if (e) return e; u |= (uint64_t)b << (8 * i); }
		     out->i64 = u; break; }
	case 0x23: { uint32_t g; e = wasm_rd_u32(&r, &g); if (e) return e;
		     if (g >= in->m->total_globals) return WASM_E_INDEX;
		     *out = in->globals[g]; break; }
	default:
		/* wasm_validate has already refused everything else, so reaching
		 * here means the two disagree.  Named, not guessed at. */
		return WASM_TRAP_DESYNC;
	}
	return WASM_TRAP_NONE;
}

/* ---- imports ----------------------------------------------------------- */

static int name_eq(const struct wasm_module *m, struct wasm_span s, const char *z)
{
	uint32_t i;
	for (i = 0; i < s.len; i++) {
		if (z[i] == 0 || (uint8_t)z[i] != m->bytes[s.off + i]) return 0;
	}
	return z[s.len] == 0;
}

static const struct wasm_hostimport *
find_import(const struct wasm_module *m, const struct wasm_import *im,
            const struct wasm_hostimport *imps, uint32_t nimps, uint8_t kind)
{
	uint32_t i;
	for (i = 0; i < nimps; i++) {
		if (imps[i].kind != kind) continue;
		if (!name_eq(m, im->module_name, imps[i].module)) continue;
		if (!name_eq(m, im->field_name, imps[i].name)) continue;
		return &imps[i];
	}
	return 0;
}

/* ---- instantiation ----------------------------------------------------- */

#define STACK_SLOTS 65536u
#define CTRL_SLOTS  8192u
#define STACK_GUARD 8u          /* see below */

static int call_any(struct wasm_instance *in, uint32_t fidx);

int wasm_instantiate(struct wasm_instance **out, const struct wasm_module *m,
                     struct wasm_arena *a,
                     const struct wasm_hostimport *imps, uint32_t nimps)
{
	struct wasm_instance *in;
	uint32_t i, nf = 0, ng = 0;
	int e;

	*out = 0;
	in = wasm_arena_alloc(a, 1, sizeof *in);
	if (!in) return WASM_TRAP_NOMEM;
	zero(in, sizeof *in);
	in->m = m;
	in->a = a;

	in->globals = wasm_arena_alloc(a, m->total_globals ? m->total_globals : 1, sizeof *in->globals);
	if (!in->globals) return WASM_TRAP_NOMEM;
	in->hostfn = wasm_arena_alloc(a, m->nimp_funcs ? m->nimp_funcs : 1, sizeof *in->hostfn);
	if (!in->hostfn) return WASM_TRAP_NOMEM;

	/* --- resolve every import, or refuse by name --- */
	for (i = 0; i < m->nimports; i++) {
		const struct wasm_import *im = &m->imports[i];
		const struct wasm_hostimport *h;
		switch (im->kind) {
		case WASM_EXT_FUNC:
			h = find_import(m, im, imps, nimps, WASM_IMP_FUNC);
			if (!h) return WASM_TRAP_UNLINKABLE;
			in->hostfn[nf++] = h;
			break;
		case WASM_EXT_GLOBAL:
			h = find_import(m, im, imps, nimps, WASM_IMP_GLOBAL);
			if (!h) return WASM_TRAP_UNLINKABLE;
			if (h->gtype != im->gt.valtype || h->gmut != im->gt.mut)
				return WASM_TRAP_UNLINKABLE;
			in->globals[ng++] = h->gval;
			break;
		case WASM_EXT_MEM:
			h = find_import(m, im, imps, nimps, WASM_IMP_MEMORY);
			if (!h) return WASM_TRAP_UNLINKABLE;
			/* Shared, not copied: the host and the module must see the
			 * same bytes, and a memory that were copied in would stop
			 * being the same memory at the first store. */
			in->mem = h->mem;
			in->mem_pages = *h->mpages;
			in->mem_maxpages = *h->mmaxpages;
			in->mempages_ext = h->mpages;
			in->memmax_ext = h->mmaxpages;
			in->membase_ext = h->memp;   /* optional; see wasm_exec.h */
			in->mem_present = 1;
			if (in->mem_pages < im->mem.min) return WASM_TRAP_UNLINKABLE;
			break;
		case WASM_EXT_TABLE:
			h = find_import(m, im, imps, nimps, WASM_IMP_TABLE);
			if (!h) return WASM_TRAP_UNLINKABLE;
			in->table = h->table;
			in->table_size = *h->tsize;
			in->table_max = *h->tmax;
			in->tsize_ext = h->tsize;
			in->tmax_ext = h->tmax;
			in->table_present = 1;
			if (in->table_size < im->tt.lim.min) return WASM_TRAP_UNLINKABLE;
			break;
		default:
			return WASM_TRAP_UNLINKABLE;
		}
	}

	/* --- defined memory --- */
	for (i = 0; i < m->nmems; i++) {
		if (m->mems[i].min > WASM_MEM_PAGES_CAP) {
			/* Apparatus: the module is legal and we cannot build it.  Loud,
			 * and never counted as the module's fault. */
			in->apparatus++;
			return WASM_TRAP_NOMEM;
		}
		in->mem_pages = m->mems[i].min;
		in->mem_maxpages = m->mems[i].has_max ? m->mems[i].max : 65536u;
		in->mem = wasm_arena_alloc(a, in->mem_pages ? in->mem_pages : 1, WASM_PAGE);
		if (!in->mem) { in->apparatus++; return WASM_TRAP_NOMEM; }
		in->mem_present = 1;
	}

	/* --- defined table --- */
	for (i = 0; i < m->ntables; i++) {
		uint32_t k;
		in->table_size = m->tables[i].lim.min;
		in->table_max = m->tables[i].lim.has_max ? m->tables[i].lim.max : 0xFFFFFFFFu;
		in->table = wasm_arena_alloc(a, in->table_size ? in->table_size : 1, sizeof *in->table);
		if (!in->table) { in->apparatus++; return WASM_TRAP_NOMEM; }
		for (k = 0; k < in->table_size; k++) in->table[k] = NOFUNC;
		in->table_present = 1;
	}

	/* --- defined globals, in order: an initialiser may read an imported
	 *     global and never a later one, which wasm_validate has enforced --- */
	for (i = 0; i < m->nglobals; i++) {
		e = const_eval(in, m->globals[i].init, &in->globals[ng]);
		if (e) return e;
		ng++;
	}

	/* --- the per-function control map --- */
	in->fn = wasm_arena_alloc(a, m->ncode ? m->ncode : 1, sizeof *in->fn);
	if (!in->fn) return WASM_TRAP_NOMEM;
	for (i = 0; i < m->ncode; i++) {
		e = build_fninfo(in, i);
		if (e) return e;
	}

	/* --- the interpreter's own stacks.  STACK_GUARD slots of headroom in
	 *     front: the validator proves the operand stack is balanced, but a
	 *     bug in THIS file must not be able to read before the array while it
	 *     is being caught, so the underflow check below has somewhere safe to
	 *     have already looked. --- */
	in->stackraw = wasm_arena_alloc(a, STACK_SLOTS + 2 * STACK_GUARD, sizeof *in->stackraw);
	if (!in->stackraw) return WASM_TRAP_NOMEM;
	in->stack = in->stackraw + STACK_GUARD;
	in->scap = STACK_SLOTS;
	in->ctrl = wasm_arena_alloc(a, CTRL_SLOTS, sizeof *in->ctrl);
	if (!in->ctrl) return WASM_TRAP_NOMEM;
	in->ccap = CTRL_SLOTS;

	/* --- element and data segments: EVERY bound checked before ANY write,
	 *     so a module that fails to instantiate has not half-written the
	 *     memory of the instance the embedder is about to throw away --- */
	for (i = 0; i < m->nelems; i++) {
		union wasm_val off;
		e = const_eval(in, m->elems[i].offset, &off);
		if (e) return e;
		if (!in->table_present) return WASM_TRAP_TABLE_OOB;
		if ((uint64_t)off.i32 + m->elems[i].nfunc > in->table_size)
			return WASM_TRAP_TABLE_OOB;
	}
	for (i = 0; i < m->ndatas; i++) {
		union wasm_val off;
		e = const_eval(in, m->datas[i].offset, &off);
		if (e) return e;
		if (!mem_range_ok(in, off.i32, m->datas[i].bytes.len))
			return WASM_TRAP_MEM_OOB;
	}
	for (i = 0; i < m->nelems; i++) {
		union wasm_val off;
		struct rd fr;
		uint32_t j;
		(void)const_eval(in, m->elems[i].offset, &off);
		fr.p = m->elems[i].funcs_raw; fr.i = 0; fr.n = m->elems[i].funcs_raw_len;
		for (j = 0; j < m->elems[i].nfunc; j++) {
			uint32_t f;
			if (wasm_rd_u32(&fr, &f)) return WASM_TRAP_DESYNC;
			in->table[off.i32 + j] = f;
		}
	}
	for (i = 0; i < m->ndatas; i++) {
		union wasm_val off;
		uint32_t j;
		(void)const_eval(in, m->datas[i].offset, &off);
		for (j = 0; j < m->datas[i].bytes.len; j++)
			in->mem[off.i32 + j] = m->bytes[m->datas[i].bytes.off + j];
	}

	*out = in;

	/* --- start --- */
	if (m->has_start) {
		in->sp = 0; in->ctop = 0; in->depth = 0;
		e = call_any(in, m->start);
		if (e) return e;
	}
	return WASM_TRAP_NONE;
}

int wasm_export_index(const struct wasm_instance *in, const char *name,
                      uint32_t namelen, uint8_t kind, uint32_t *idx)
{
	uint32_t i, k;
	for (i = 0; i < in->m->nexports; i++) {
		const struct wasm_export *x = &in->m->exports[i];
		if (x->kind != kind || x->name.len != namelen) continue;
		for (k = 0; k < namelen; k++)
			if (in->m->bytes[x->name.off + k] != (uint8_t)name[k]) break;
		if (k == namelen) { *idx = x->index; return 1; }
	}
	return 0;
}

/* ---- the interpreter --------------------------------------------------- */

#define PUSHV(v) (in->stack[in->sp++] = (v))
#define POPV()   (in->stack[--in->sp])

static int bit_at(const uint8_t *b, uint32_t i) { return (b[i >> 3] >> (i & 7)) & 1; }

/* {width in bytes, sign-extend, is-load, result is 32-bit}.  This is
 * SEMANTICS, not immediate layout -- wasm_valid.c's g_mem carries the natural
 * alignment and the value type and this carries the access width; the two are
 * related by width == 1 << natural-align, which is what makes them checkable
 * against each other rather than two copies of one fact. */
struct memop { uint8_t w, sext, load, is32; };
static const struct memop g_memop[0x3F - 0x28] = {
	/* 28 */ {4,0,1,1}, /* 29 */ {8,0,1,0}, /* 2A */ {4,0,1,1}, /* 2B */ {8,0,1,0},
	/* 2C */ {1,1,1,1}, /* 2D */ {1,0,1,1}, /* 2E */ {2,1,1,1}, /* 2F */ {2,0,1,1},
	/* 30 */ {1,1,1,0}, /* 31 */ {1,0,1,0}, /* 32 */ {2,1,1,0}, /* 33 */ {2,0,1,0},
	/* 34 */ {4,1,1,0}, /* 35 */ {4,0,1,0},
	/* 36 */ {4,0,0,1}, /* 37 */ {8,0,0,0}, /* 38 */ {4,0,0,1}, /* 39 */ {8,0,0,0},
	/* 3A */ {1,0,0,1}, /* 3B */ {2,0,0,1},
	/* 3C */ {1,0,0,0}, /* 3D */ {2,0,0,0}, /* 3E */ {4,0,0,0},
};

static uint64_t sext(uint64_t v, uint32_t bytes)
{
	uint32_t bits = bytes * 8;
	if (bits >= 64) return v;
	if (v & (1ull << (bits - 1))) v |= ~0ull << bits;
	return v;
}

/* trap #3, both halves, and the bound of each is written out per opcode
 * rather than shared, because the two ends are NOT symmetric and a macro that
 * made them look symmetric would be the bug.
 *
 * The test runs on the FLOAT, against exactly representable limits: casting
 * first and checking after runs the check on a value the C standard has
 * already declined to define.  The low end is inclusive where the limit is
 * representable (-2^31 and -2^63 are exact in both formats) and exclusive
 * where it is not -- for the unsigned forms the boundary is -1.0, and every
 * float in (-1, 0] truncates to 0 and is legal, so `x > -1.0` is the whole
 * rule and any nearby literal excludes real values.  The high end is always
 * exclusive at a power of two, which is exact in both formats.
 *
 * NaN is tested first and separately so that "invalid conversion to integer"
 * and "integer overflow" stay two different answers -- the suite asserts each
 * by name, and a single combined check passes both assertions for the wrong
 * reason exactly half the time. */
#define TRUNC(TY, PRED, CAST)                                               \
	do {                                                                \
		TY x = v;                                                   \
		if (x != x) { tr = WASM_TRAP_BAD_CONVERSION; goto trap; }    \
		if (!(PRED)) { tr = WASM_TRAP_INT_OVERFLOW; goto trap; }     \
		CAST;                                                       \
	} while (0)

/* Execute one DEFINED function.  Parameters are already on the operand stack. */
static int run_body(struct wasm_instance *in, uint32_t fidx)
{
	const struct wasm_module *m = in->m;
	const struct wasm_functype *ft = &m->types[m->functype_of[fidx]];
	uint32_t k = fidx - m->nimp_funcs;
	const struct wasm_code *code = &m->code[k];
	const struct fninfo *fi = &in->fn[k];
	uint32_t lbase = in->sp - ft->nparams;
	uint32_t cbase = in->ctop;
	uint32_t fbase;
	struct rd r;
	int tr = WASM_TRAP_NONE;
	uint32_t i;
	/* The branch target lives at function scope because br, br_if and
	 * br_table all end in the same code.  Declaring it inside the br case
	 * and jumping in from br_table would be a jump past an initialiser: the
	 * label would be reached with the OTHER case's variable in scope, which
	 * compiles and reads correctly and branches to whatever was last there. */
	uint32_t br_l = 0;

	if (in->sp + code->nlocals + 2 > in->scap) return WASM_TRAP_EXHAUSTED;
	for (i = 0; i < code->nlocals; i++) in->stack[in->sp++].bits = 0;
	fbase = in->sp;

	if (in->ctop >= in->ccap) return WASM_TRAP_EXHAUSTED;
	in->ctrl[in->ctop].cont = fi->off + fi->len;
	in->ctrl[in->ctop].vheight = fbase;
	in->ctrl[in->ctop].ent = NOPOS;
	in->ctrl[in->ctop].arity = (uint8_t)ft->nresults;
	in->ctrl[in->ctop].is_loop = 0;
	in->ctop++;

	r.p = m->bytes;
	r.n = fi->off + fi->len;
	r.i = fi->off;

	for (;;) {
		uint8_t op;
		uint32_t ip = r.i;

		/* the cross-check: see the header comment */
		if (ip < fi->off || ip >= fi->off + fi->len ||
		    !bit_at(fi->bmap, ip - fi->off)) {
			in->trap_op = ip < r.n ? m->bytes[ip] : 0;
			return WASM_TRAP_DESYNC;
		}
		if (in->sp < fbase) { in->trap_op = m->bytes[ip]; return WASM_TRAP_DESYNC; }
		if (in->sp + 2 > in->scap) return WASM_TRAP_EXHAUSTED;

		if (wasm_rd_u8(&r, &op)) return WASM_TRAP_DESYNC;

		switch (op) {

		case 0x00:  /* unreachable */
			tr = WASM_TRAP_UNREACHABLE; goto trap;
		case 0x01:  /* nop */
			break;

		case 0x02: case 0x03: case 0x04: {   /* block, loop, if */
			int64_t bt;
			uint32_t ent = ent_at(fi, ip);
			uint8_t arity;
			if (ent == NOPOS) { in->trap_op = op; return WASM_TRAP_DESYNC; }
			if (wasm_rd_s33(&r, &bt)) return WASM_TRAP_DESYNC;
			/* wasm_validate has already refused every blocktype but
			 * `empty` and a single valtype, so arity is a two-case
			 * derivation from the s33 rather than a second copy of the
			 * blocktype table. */
			arity = (bt == -0x40) ? 0 : 1;
			if (op == 0x04) {
				uint32_t c = POPV().i32;
				if (!c) {
					if (fi->ents[ent].els != NOPOS) {
						r.i = fi->ents[ent].els + 1;
					} else {
						r.i = fi->ents[ent].end + 1;
						break;      /* no frame: nothing to close */
					}
				}
			}
			if (in->ctop >= in->ccap) return WASM_TRAP_EXHAUSTED;
			in->ctrl[in->ctop].cont = (op == 0x03) ? r.i : fi->ents[ent].end + 1;
			in->ctrl[in->ctop].vheight = in->sp;
			in->ctrl[in->ctop].ent = ent;
			in->ctrl[in->ctop].arity = (op == 0x03) ? 0 : arity;
			in->ctrl[in->ctop].is_loop = (op == 0x03);
			in->ctop++;
			break;
		}

		case 0x05: {   /* else, reached by falling out of a taken `then` */
			struct rframe *f = &in->ctrl[in->ctop - 1];
			if (f->ent == NOPOS) { in->trap_op = op; return WASM_TRAP_DESYNC; }
			r.i = fi->ents[f->ent].end + 1;
			in->ctop--;
			break;
		}

		case 0x0B:     /* end */
			in->ctop--;
			if (in->ctop == cbase) goto done;
			break;

		case 0x0C: case 0x0D: {   /* br, br_if */
			if (wasm_rd_u32(&r, &br_l)) return WASM_TRAP_DESYNC;
			if (op == 0x0D && POPV().i32 == 0) break;
			goto do_branch;
		}

		case 0x0E: {   /* br_table -- trap #5 */
			uint32_t n, sel, j, l;
			if (wasm_rd_u32(&r, &n)) return WASM_TRAP_DESYNC;
			sel = POPV().i32;
			/* n targets and THEN the default.  The default is not a
			 * target with a special index -- it is the (n+1)th entry --
			 * and a selector at or past n takes it.  Every entry is read
			 * whatever the selector is, because they are LEB128 and the
			 * (n+1)th cannot be found without walking the first n. */
			br_l = 0;
			for (j = 0; j <= n; j++) {
				if (wasm_rd_u32(&r, &l)) return WASM_TRAP_DESYNC;
				if (j == sel || (sel >= n && j == n)) br_l = l;
			}
			goto do_branch;
		}

		case 0x0F:     /* return */
			br_l = in->ctop - 1 - cbase;   /* the outermost label of THIS frame */
			goto do_branch;

		do_branch: {
			struct rframe *f;
			uint32_t idx, ar, j;
			/* label depth counts outwards from the innermost frame of
			 * THIS function; a depth that reaches past cbase would be a
			 * branch into the caller, which wasm_validate refused. */
			if (br_l >= in->ctop - cbase) { in->trap_op = op; return WASM_TRAP_DESYNC; }
			idx = in->ctop - 1 - br_l;
			f = &in->ctrl[idx];
			ar = f->arity;
			for (j = 0; j < ar; j++)
				in->stack[f->vheight + j] = in->stack[in->sp - ar + j];
			in->sp = f->vheight + ar;
			if (idx == cbase) { in->ctop = cbase; goto done; }
			/* A branch to a LOOP goes to its head and the frame stays;
			 * a branch to anything else leaves the block, so the frame
			 * goes with it.  Getting this backwards makes every loop run
			 * once and every block run forever. */
			if (f->is_loop) { in->ctop = idx + 1; r.i = f->cont; }
			else            { in->ctop = idx;     r.i = f->cont; }
			break;
		}

		case 0x10: {   /* call */
			uint32_t f;
			if (wasm_rd_u32(&r, &f)) return WASM_TRAP_DESYNC;
			tr = call_any(in, f);
			if (tr) goto trap;
			break;
		}
		case 0x11: {   /* call_indirect */
			uint32_t ty, idx, f;
			uint8_t z;
			if (wasm_rd_u32(&r, &ty)) return WASM_TRAP_DESYNC;
#ifndef WASM_NEGCTL_IMM
			/* The MVP's reserved table byte.  Not reading it walks the
			 * cursor one byte into the next instruction, which is what
			 * the boundary cross-check exists to catch -- and
			 * WASM_NEGCTL_IMM is exactly this line removed, so the
			 * check can be watched firing. */
			if (wasm_rd_u8(&r, &z)) return WASM_TRAP_DESYNC;
			(void)z;
#endif
			idx = POPV().i32;
			if (!in->table_present || idx >= in->table_size) { tr = WASM_TRAP_TABLE_OOB; goto trap; }
			f = in->table[idx];
			if (f == NOFUNC) { tr = WASM_TRAP_UNINIT_ELEM; goto trap; }
			if (f >= m->total_funcs) { tr = WASM_TRAP_TABLE_OOB; goto trap; }
			if (!functype_eq(&m->types[m->functype_of[f]], &m->types[ty])) {
				tr = WASM_TRAP_INDIRECT_TYPE; goto trap;
			}
			tr = call_any(in, f);
			if (tr) goto trap;
			break;
		}

		case 0x1A:   /* drop */
			in->sp--;
			break;
		case 0x1B: { /* select */
			uint32_t c = POPV().i32;
			union wasm_val b = POPV(), a = POPV();
			PUSHV(c ? a : b);
			break;
		}

		case 0x20: { uint32_t l; if (wasm_rd_u32(&r, &l)) return WASM_TRAP_DESYNC;
			     PUSHV(in->stack[lbase + l]); break; }
		case 0x21: { uint32_t l; if (wasm_rd_u32(&r, &l)) return WASM_TRAP_DESYNC;
			     in->stack[lbase + l] = POPV(); break; }
		case 0x22: { uint32_t l; if (wasm_rd_u32(&r, &l)) return WASM_TRAP_DESYNC;
			     in->stack[lbase + l] = in->stack[in->sp - 1]; break; }
		case 0x23: { uint32_t g; if (wasm_rd_u32(&r, &g)) return WASM_TRAP_DESYNC;
			     PUSHV(in->globals[g]); break; }
		case 0x24: { uint32_t g; if (wasm_rd_u32(&r, &g)) return WASM_TRAP_DESYNC;
			     in->globals[g] = POPV(); break; }

		case 0x3F: { uint8_t z; union wasm_val v;
			     if (wasm_rd_u8(&r, &z)) return WASM_TRAP_DESYNC;
			     (void)z;
			     v.bits = 0; v.i32 = in->mem_present ? in->mem_pages : 0;
			     PUSHV(v); break; }
		case 0x40: { uint8_t z; uint32_t d, old; union wasm_val v;
			     if (wasm_rd_u8(&r, &z)) return WASM_TRAP_DESYNC;
			     (void)z;
			     d = POPV().i32;
			     old = mem_grow(in, d);
			     v.bits = 0; v.i32 = old;   /* NOFUNC == (uint32_t)-1 */
			     PUSHV(v); break; }

		case 0x41: { int32_t v; union wasm_val w;
			     if (wasm_rd_s32(&r, &v)) return WASM_TRAP_DESYNC;
			     w.bits = 0; w.i32 = (uint32_t)v; PUSHV(w); break; }
		case 0x42: { int64_t v; union wasm_val w;
			     if (wasm_rd_s64(&r, &v)) return WASM_TRAP_DESYNC;
			     w.bits = 0; w.i64 = (uint64_t)v; PUSHV(w); break; }
		case 0x43: { uint32_t u = 0, j; union wasm_val w;
			     for (j = 0; j < 4; j++) { uint8_t b; if (wasm_rd_u8(&r, &b)) return WASM_TRAP_DESYNC;
						       u |= (uint32_t)b << (8 * j); }
			     w.bits = 0; w.i32 = u; PUSHV(w); break; }
		case 0x44: { uint64_t u = 0; uint32_t j; union wasm_val w;
			     for (j = 0; j < 8; j++) { uint8_t b; if (wasm_rd_u8(&r, &b)) return WASM_TRAP_DESYNC;
						       u |= (uint64_t)b << (8 * j); }
			     w.bits = 0; w.i64 = u; PUSHV(w); break; }

		default:
			if (op >= 0x28 && op <= 0x3E) {
				const struct memop *mo = &g_memop[op - 0x28];
				uint32_t align, off;
				uint64_t ea, raw = 0;
				uint32_t j;
				union wasm_val v, addr;
				if (wasm_rd_u32(&r, &align)) return WASM_TRAP_DESYNC;
				if (wasm_rd_u32(&r, &off)) return WASM_TRAP_DESYNC;
				(void)align;   /* alignment is a HINT; misalignment never traps */
				if (mo->load) {
					addr = POPV();
					ea = (uint64_t)addr.i32 + off;
					if (!mem_range_ok(in, ea, mo->w)) { tr = WASM_TRAP_MEM_OOB; goto trap; }
					for (j = 0; j < mo->w; j++)
						raw |= (uint64_t)in->mem[ea + j] << (8 * j);
					if (mo->sext) raw = sext(raw, mo->w);
					v.bits = 0;
					if (mo->is32) v.i32 = (uint32_t)raw; else v.i64 = raw;
					PUSHV(v);
				} else {
					v = POPV();
					addr = POPV();
					ea = (uint64_t)addr.i32 + off;
					if (!mem_range_ok(in, ea, mo->w)) { tr = WASM_TRAP_MEM_OOB; goto trap; }
					raw = mo->is32 ? (uint64_t)v.i32 : v.i64;
					for (j = 0; j < mo->w; j++)
						in->mem[ea + j] = (uint8_t)(raw >> (8 * j));
				}
				break;
			}
			if (op >= 0x45 && op <= 0xBF) {
				union wasm_val rv;
				rv.bits = 0;
				tr = WASM_TRAP_NONE;
				switch (op) {

				/* ---- i32 comparisons ---- */
				case 0x45: { uint32_t a = POPV().i32; rv.i32 = (a == 0); break; }
				case 0x46: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = (a == b); break; }
				case 0x47: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = (a != b); break; }
				case 0x48: { int32_t b = (int32_t)POPV().i32, a = (int32_t)POPV().i32; rv.i32 = (a < b); break; }
				case 0x49: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = (a < b); break; }
				case 0x4A: { int32_t b = (int32_t)POPV().i32, a = (int32_t)POPV().i32; rv.i32 = (a > b); break; }
				case 0x4B: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = (a > b); break; }
				case 0x4C: { int32_t b = (int32_t)POPV().i32, a = (int32_t)POPV().i32; rv.i32 = (a <= b); break; }
				case 0x4D: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = (a <= b); break; }
				case 0x4E: { int32_t b = (int32_t)POPV().i32, a = (int32_t)POPV().i32; rv.i32 = (a >= b); break; }
				case 0x4F: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = (a >= b); break; }

				/* ---- i64 comparisons: all produce an i32 ---- */
				case 0x50: { uint64_t a = POPV().i64; rv.i32 = (a == 0); break; }
				case 0x51: { uint64_t b = POPV().i64, a = POPV().i64; rv.i32 = (a == b); break; }
				case 0x52: { uint64_t b = POPV().i64, a = POPV().i64; rv.i32 = (a != b); break; }
				case 0x53: { int64_t b = (int64_t)POPV().i64, a = (int64_t)POPV().i64; rv.i32 = (a < b); break; }
				case 0x54: { uint64_t b = POPV().i64, a = POPV().i64; rv.i32 = (a < b); break; }
				case 0x55: { int64_t b = (int64_t)POPV().i64, a = (int64_t)POPV().i64; rv.i32 = (a > b); break; }
				case 0x56: { uint64_t b = POPV().i64, a = POPV().i64; rv.i32 = (a > b); break; }
				case 0x57: { int64_t b = (int64_t)POPV().i64, a = (int64_t)POPV().i64; rv.i32 = (a <= b); break; }
				case 0x58: { uint64_t b = POPV().i64, a = POPV().i64; rv.i32 = (a <= b); break; }
				case 0x59: { int64_t b = (int64_t)POPV().i64, a = (int64_t)POPV().i64; rv.i32 = (a >= b); break; }
				case 0x5A: { uint64_t b = POPV().i64, a = POPV().i64; rv.i32 = (a >= b); break; }

				/* ---- float comparisons.  C's operators already
				 * answer false for every ordered test against a
				 * NaN and true for !=, which is the specification. */
				case 0x5B: { float b = POPV().f32, a = POPV().f32; rv.i32 = (a == b); break; }
				case 0x5C: { float b = POPV().f32, a = POPV().f32; rv.i32 = (a != b); break; }
				case 0x5D: { float b = POPV().f32, a = POPV().f32; rv.i32 = (a < b); break; }
				case 0x5E: { float b = POPV().f32, a = POPV().f32; rv.i32 = (a > b); break; }
				case 0x5F: { float b = POPV().f32, a = POPV().f32; rv.i32 = (a <= b); break; }
				case 0x60: { float b = POPV().f32, a = POPV().f32; rv.i32 = (a >= b); break; }
				case 0x61: { double b = POPV().f64, a = POPV().f64; rv.i32 = (a == b); break; }
				case 0x62: { double b = POPV().f64, a = POPV().f64; rv.i32 = (a != b); break; }
				case 0x63: { double b = POPV().f64, a = POPV().f64; rv.i32 = (a < b); break; }
				case 0x64: { double b = POPV().f64, a = POPV().f64; rv.i32 = (a > b); break; }
				case 0x65: { double b = POPV().f64, a = POPV().f64; rv.i32 = (a <= b); break; }
				case 0x66: { double b = POPV().f64, a = POPV().f64; rv.i32 = (a >= b); break; }

				/* ---- i32 arithmetic ---- */
				case 0x67: { uint32_t a = POPV().i32; rv.i32 = clz32(a); break; }
				case 0x68: { uint32_t a = POPV().i32; rv.i32 = ctz32(a); break; }
				case 0x69: { uint32_t a = POPV().i32; rv.i32 = pop32(a); break; }
				case 0x6A: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a + b; break; }
				case 0x6B: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a - b; break; }
				case 0x6C: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a * b; break; }
				/* trap #1 */
				case 0x6D: { uint32_t b = POPV().i32, a = POPV().i32;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     if (a == 0x80000000u && b == 0xFFFFFFFFu) { tr = WASM_TRAP_INT_OVERFLOW; goto trap; }
					     rv.i32 = (uint32_t)((int32_t)a / (int32_t)b); break; }
				case 0x6E: { uint32_t b = POPV().i32, a = POPV().i32;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     rv.i32 = a / b; break; }
				case 0x6F: { uint32_t b = POPV().i32, a = POPV().i32;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     /* INT_MIN % -1 is ZERO and is NOT a trap.  On x86
					      * the instruction that computes it faults, so the
					      * special case is load-bearing on both sides. */
					     if (a == 0x80000000u && b == 0xFFFFFFFFu) rv.i32 = 0;
					     else rv.i32 = (uint32_t)((int32_t)a % (int32_t)b);
					     break; }
				case 0x70: { uint32_t b = POPV().i32, a = POPV().i32;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     rv.i32 = a % b; break; }
				case 0x71: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a & b; break; }
				case 0x72: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a | b; break; }
				case 0x73: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a ^ b; break; }
				/* trap #2 */
				case 0x74: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a << (b & 31); break; }
				case 0x75: { uint32_t b = POPV().i32, a = POPV().i32;
					     rv.i32 = (uint32_t)((int32_t)a >> (b & 31)); break; }
				case 0x76: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = a >> (b & 31); break; }
				case 0x77: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = rotl32(a, b); break; }
				case 0x78: { uint32_t b = POPV().i32, a = POPV().i32; rv.i32 = rotr32(a, b); break; }

				/* ---- i64 arithmetic ---- */
				case 0x79: { uint64_t a = POPV().i64; rv.i64 = clz64(a); break; }
				case 0x7A: { uint64_t a = POPV().i64; rv.i64 = ctz64(a); break; }
				case 0x7B: { uint64_t a = POPV().i64; rv.i64 = pop64(a); break; }
				case 0x7C: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a + b; break; }
				case 0x7D: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a - b; break; }
				case 0x7E: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a * b; break; }
				case 0x7F: { uint64_t b = POPV().i64, a = POPV().i64;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     if (a == 0x8000000000000000ull && b == 0xFFFFFFFFFFFFFFFFull) { tr = WASM_TRAP_INT_OVERFLOW; goto trap; }
					     rv.i64 = (uint64_t)((int64_t)a / (int64_t)b); break; }
				case 0x80: { uint64_t b = POPV().i64, a = POPV().i64;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     rv.i64 = a / b; break; }
				case 0x81: { uint64_t b = POPV().i64, a = POPV().i64;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     if (a == 0x8000000000000000ull && b == 0xFFFFFFFFFFFFFFFFull) rv.i64 = 0;
					     else rv.i64 = (uint64_t)((int64_t)a % (int64_t)b);
					     break; }
				case 0x82: { uint64_t b = POPV().i64, a = POPV().i64;
					     if (b == 0) { tr = WASM_TRAP_DIV_ZERO; goto trap; }
					     rv.i64 = a % b; break; }
				case 0x83: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a & b; break; }
				case 0x84: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a | b; break; }
				case 0x85: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a ^ b; break; }
				case 0x86: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a << (b & 63); break; }
				case 0x87: { uint64_t b = POPV().i64, a = POPV().i64;
					     rv.i64 = (uint64_t)((int64_t)a >> (b & 63)); break; }
				case 0x88: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = a >> (b & 63); break; }
				case 0x89: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = rotl64(a, b); break; }
				case 0x8A: { uint64_t b = POPV().i64, a = POPV().i64; rv.i64 = rotr64(a, b); break; }

				/* ---- f32 ---- */
				case 0x8B: { uint32_t a = POPV().i32; rv.i32 = a & 0x7FFFFFFFu; break; }
				case 0x8C: { uint32_t a = POPV().i32; rv.i32 = a ^ 0x80000000u; break; }
				case 0x8D: { float a = POPV().f32; rv.f32 = f32_ceil(a); break; }
				case 0x8E: { float a = POPV().f32; rv.f32 = f32_floor(a); break; }
				case 0x8F: { float a = POPV().f32; rv.f32 = f32_trunc(a); break; }
				case 0x90: { float a = POPV().f32; rv.f32 = f32_nearest(a); break; }
				case 0x91: { float a = POPV().f32; rv.f32 = __builtin_sqrtf(a); break; }
				case 0x92: { float b = POPV().f32, a = POPV().f32; rv.f32 = a + b; break; }
				case 0x93: { float b = POPV().f32, a = POPV().f32; rv.f32 = a - b; break; }
				case 0x94: { float b = POPV().f32, a = POPV().f32; rv.f32 = a * b; break; }
				case 0x95: { float b = POPV().f32, a = POPV().f32; rv.f32 = a / b; break; }
				case 0x96: { float b = POPV().f32, a = POPV().f32; rv.f32 = f32_min(a, b); break; }
				case 0x97: { float b = POPV().f32, a = POPV().f32; rv.f32 = f32_max(a, b); break; }
				case 0x98: { uint32_t b = POPV().i32, a = POPV().i32;
					     rv.i32 = (a & 0x7FFFFFFFu) | (b & 0x80000000u); break; }

				/* ---- f64 ---- */
				case 0x99: { uint64_t a = POPV().i64; rv.i64 = a & 0x7FFFFFFFFFFFFFFFull; break; }
				case 0x9A: { uint64_t a = POPV().i64; rv.i64 = a ^ 0x8000000000000000ull; break; }
				case 0x9B: { double a = POPV().f64; rv.f64 = f64_ceil(a); break; }
				case 0x9C: { double a = POPV().f64; rv.f64 = f64_floor(a); break; }
				case 0x9D: { double a = POPV().f64; rv.f64 = f64_trunc(a); break; }
				case 0x9E: { double a = POPV().f64; rv.f64 = f64_nearest(a); break; }
				case 0x9F: { double a = POPV().f64; rv.f64 = __builtin_sqrt(a); break; }
				case 0xA0: { double b = POPV().f64, a = POPV().f64; rv.f64 = a + b; break; }
				case 0xA1: { double b = POPV().f64, a = POPV().f64; rv.f64 = a - b; break; }
				case 0xA2: { double b = POPV().f64, a = POPV().f64; rv.f64 = a * b; break; }
				case 0xA3: { double b = POPV().f64, a = POPV().f64; rv.f64 = a / b; break; }
				case 0xA4: { double b = POPV().f64, a = POPV().f64; rv.f64 = f64_min(a, b); break; }
				case 0xA5: { double b = POPV().f64, a = POPV().f64; rv.f64 = f64_max(a, b); break; }
				case 0xA6: { uint64_t b = POPV().i64, a = POPV().i64;
					     rv.i64 = (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull); break; }

				/* ---- conversions ---- */
				case 0xA7: { uint64_t a = POPV().i64; rv.i32 = (uint32_t)a; break; }
				case 0xA8: { float v = POPV().f32;
					     TRUNC(float, x >= -2147483648.0f && x < 2147483648.0f,
						   rv.i32 = (uint32_t)(int32_t)x); break; }
				case 0xA9: { float v = POPV().f32;
					     TRUNC(float, x > -1.0f && x < 4294967296.0f,
						   rv.i32 = (uint32_t)x); break; }
				case 0xAA: { double v = POPV().f64;
					     /* an f64 CAN hold -2147483648.5, which truncates
					      * into range, so this low bound is exclusive at
					      * -2^31-1 where the f32 form's is inclusive at
					      * -2^31.  The two look like a typo for each other. */
					     TRUNC(double, x > -2147483649.0 && x < 2147483648.0,
						   rv.i32 = (uint32_t)(int32_t)x); break; }
				case 0xAB: { double v = POPV().f64;
					     TRUNC(double, x > -1.0 && x < 4294967296.0,
						   rv.i32 = (uint32_t)x); break; }
				case 0xAC: { uint32_t a = POPV().i32; rv.i64 = (uint64_t)(int64_t)(int32_t)a; break; }
				case 0xAD: { uint32_t a = POPV().i32; rv.i64 = (uint64_t)a; break; }
				case 0xAE: { float v = POPV().f32;
					     TRUNC(float, x >= -9223372036854775808.0f && x < 9223372036854775808.0f,
						   rv.i64 = (uint64_t)(int64_t)x); break; }
				case 0xAF: { float v = POPV().f32;
					     TRUNC(float, x > -1.0f && x < 18446744073709551616.0f,
						   rv.i64 = (uint64_t)x); break; }
				case 0xB0: { double v = POPV().f64;
					     TRUNC(double, x >= -9223372036854775808.0 && x < 9223372036854775808.0,
						   rv.i64 = (uint64_t)(int64_t)x); break; }
				case 0xB1: { double v = POPV().f64;
					     TRUNC(double, x > -1.0 && x < 18446744073709551616.0,
						   rv.i64 = (uint64_t)x); break; }
				case 0xB2: { uint32_t a = POPV().i32; rv.f32 = (float)(int32_t)a; break; }
				case 0xB3: { uint32_t a = POPV().i32; rv.f32 = (float)a; break; }
				case 0xB4: { uint64_t a = POPV().i64; rv.f32 = (float)(int64_t)a; break; }
				case 0xB5: { uint64_t a = POPV().i64; rv.f32 = (float)a; break; }
				case 0xB6: { double a = POPV().f64; rv.f32 = (float)a; break; }
				case 0xB7: { uint32_t a = POPV().i32; rv.f64 = (double)(int32_t)a; break; }
				case 0xB8: { uint32_t a = POPV().i32; rv.f64 = (double)a; break; }
				case 0xB9: { uint64_t a = POPV().i64; rv.f64 = (double)(int64_t)a; break; }
				case 0xBA: { uint64_t a = POPV().i64; rv.f64 = (double)a; break; }
				case 0xBB: { float a = POPV().f32; rv.f64 = (double)a; break; }
				/* reinterpret: the bits are already in the slot; the only
				 * work is to stop the upper half of a 64-bit slot leaking
				 * into a 32-bit result. */
				case 0xBC: { uint32_t a = POPV().i32; rv.i32 = a; break; }
				case 0xBD: { uint64_t a = POPV().i64; rv.i64 = a; break; }
				case 0xBE: { uint32_t a = POPV().i32; rv.i32 = a; break; }
				case 0xBF: { uint64_t a = POPV().i64; rv.i64 = a; break; }

				default:
					/* Unreachable: g_num covers 0x45..0xBF and the
					 * decoder refused everything else.  Named anyway,
					 * because "cannot happen" is how a silent
					 * fallthrough gets argued for. */
					in->trap_op = op;
					return WASM_TRAP_UNIMPLEMENTED;
				}
				PUSHV(rv);
				break;
			}
			/* Every remaining byte is either a post-MVP opcode the decoder
			 * refuses by name or not an opcode at all.  Reaching it means
			 * the validator let something through; it is a NAMED refusal
			 * carrying the byte, never a value. */
			in->trap_op = op;
			return WASM_TRAP_UNIMPLEMENTED;
		}
	}

done:
	in->ctop = cbase;
	{
		uint32_t ar = ft->nresults, j;
		for (j = 0; j < ar; j++) in->stack[lbase + j] = in->stack[in->sp - ar + j];
		in->sp = lbase + ar;
	}
	return WASM_TRAP_NONE;

trap:
	in->ctop = cbase;
	return tr;
}

/* Call a function by index: an import goes to the host, a definition to the
 * interpreter.  The recursion is the C stack's, so it is bounded explicitly --
 * an infinite wasm recursion must become WASM_TRAP_EXHAUSTED and not a host
 * stack overflow, which is the difference between a caught error and a dead
 * process. */
static int call_any(struct wasm_instance *in, uint32_t fidx)
{
	const struct wasm_module *m = in->m;
	const struct wasm_functype *ft;
	int e;

	if (fidx >= m->total_funcs) { in->trap_op = 0; return WASM_TRAP_DESYNC; }
	ft = &m->types[m->functype_of[fidx]];

	if (in->depth >= WASM_MAX_CALL_DEPTH) return WASM_TRAP_EXHAUSTED;
	in->depth++;

	if (fidx < m->nimp_funcs) {
		const struct wasm_hostimport *h = in->hostfn[fidx];
		union wasm_val rets[4];
		uint32_t j;
		rets[0].bits = 0;
		e = h->fn ? h->fn(h->ctx, &in->stack[in->sp - ft->nparams], rets)
		          : WASM_TRAP_UNLINKABLE;
		if (!e) {
			in->sp -= ft->nparams;
			for (j = 0; j < ft->nresults; j++) in->stack[in->sp++] = rets[j];
		}
	} else {
		e = run_body(in, fidx);
	}
	in->depth--;
	return e;
}

int wasm_invoke(struct wasm_instance *in, uint32_t fidx,
                const union wasm_val *args, union wasm_val *rets)
{
	const struct wasm_functype *ft;
	uint32_t i;
	int e;

	if (fidx >= in->m->total_funcs) return WASM_TRAP_UNLINKABLE;
	ft = &in->m->types[in->m->functype_of[fidx]];

	in->sp = 0; in->ctop = 0; in->depth = 0;
	if (ft->nparams + 8 > in->scap) return WASM_TRAP_EXHAUSTED;
	for (i = 0; i < ft->nparams; i++) in->stack[in->sp++] = args[i];

	e = call_any(in, fidx);
	if (e) return e;

	for (i = 0; i < ft->nresults; i++) rets[i] = in->stack[i];
	return WASM_TRAP_NONE;
}
