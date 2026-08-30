/* WebAssembly MVP validator: the stack-machine type check, plus the
 * module-level conditions.  This file also owns THE ONLY description in the
 * tree of what immediates each opcode carries -- see wasm_int.h for why that
 * is deliberate.
 *
 * THE POLYMORPHIC STACK IS THE PART THAT IS EASY TO GET SUBTLY WRONG, and
 * getting it wrong is silent: after `unreachable`, `br`, `br_table` or
 * `return`, the operand stack is not "empty", it is "anything the rest of
 * this block wants, until the enclosing `end`".  The spec's own validation
 * algorithm (appendix, "Validation Algorithm") models that with a per-frame
 * height plus an `unreachable` flag and a distinguished Unknown type, and
 * this is that algorithm rather than a paraphrase of it.  An implementation
 * that instead clears the stack accepts `(func (result i32) unreachable)` --
 * correct -- and also accepts `(func (result i32) (block (result i32)
 * unreachable) drop)` type errors that never fire, which is the failure
 * shape unreached-invalid.wast exists to catch: 121 assert_invalid cases
 * every one of which is dead code.
 *
 * WHAT THIS FILE REFUSES BY NAME rather than approximating: sign-extension
 * operators (0xC0-0xC4), the 0xFC/0xFD/0xFE prefixes, reference types,
 * multi-value block types, more than one table, more than one memory.  All
 * WASM_E_UNSUPPORTED.  CLAUDE.md rule 3: a WebAssembly that instantiates a
 * module incorrectly is worse than no WebAssembly, because a page feature
 * tests the constructor and then trusts the result.
 */

#include "wasm_int.h"

/* SINGLE-LETTER MACROS, and they are #undef'd at the end of this file.
 * They exist because the opcode table below is 23 rows of operand and result
 * types and reads as arithmetic with them and as noise without.  They are
 * contained because a one-letter macro that escapes its file is a landmine:
 * c/apps/browser/js_wasm.c #includes this .c textually (see its header for the
 * argument), and `I` promptly ate a local variable called `I` in a function
 * three hundred lines away -- reported as "expected identifier or '('", with
 * the macro expansion as the only clue.  Anything added here gets an #undef
 * there. */
#define I WASM_VT_I32
#define J WASM_VT_I64
#define F WASM_VT_F32
#define D WASM_VT_F64
#define U WASM_VT_UNKNOWN

enum { OP_BLOCK = 0x02, OP_LOOP = 0x03, OP_IF = 0x04, OP_ELSE = 0x05, OP_END = 0x0B };

struct ctrl {
	uint8_t op;
	uint8_t out;          /* 0 = no result, else a valtype */
	uint8_t unreachable;
	uint32_t height;
};

struct wasm_tc {
	const struct wasm_module *m;
	const struct wasm_functype *ft;
	const struct wasm_code *code;
	uint8_t *vals;   uint32_t vh, vcap;
	struct ctrl *ctrls; uint32_t ch, ccap;
	int err;
};

/* ---- numeric opcodes: one row per opcode, 0x45..0xBF ------------------
 * {a, b, res}: a is the first operand, b the second (0 for a unary op), res
 * the result.  Written out rather than derived, because the interesting
 * mistakes here (i64.eqz returning i64, f32.demote taking f32) all produce a
 * table that still looks plausible. */
struct numrow { uint8_t a, b, res; };
static const struct numrow g_num[0xC0 - 0x45] = {
	/* 45 */ {I,0,I},
	/* 46..4F i32 cmp  */ {I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},
	/* 50 i64.eqz */ {J,0,I},
	/* 51..5A i64 cmp  */ {J,J,I},{J,J,I},{J,J,I},{J,J,I},{J,J,I},{J,J,I},{J,J,I},{J,J,I},{J,J,I},{J,J,I},
	/* 5B..60 f32 cmp  */ {F,F,I},{F,F,I},{F,F,I},{F,F,I},{F,F,I},{F,F,I},
	/* 61..66 f64 cmp  */ {D,D,I},{D,D,I},{D,D,I},{D,D,I},{D,D,I},{D,D,I},
	/* 67..69 i32 un   */ {I,0,I},{I,0,I},{I,0,I},
	/* 6A..78 i32 bin  */ {I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},
	                      {I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},{I,I,I},
	/* 79..7B i64 un   */ {J,0,J},{J,0,J},{J,0,J},
	/* 7C..8A i64 bin  */ {J,J,J},{J,J,J},{J,J,J},{J,J,J},{J,J,J},{J,J,J},{J,J,J},{J,J,J},
	                      {J,J,J},{J,J,J},{J,J,J},{J,J,J},{J,J,J},{J,J,J},{J,J,J},
	/* 8B..91 f32 un   */ {F,0,F},{F,0,F},{F,0,F},{F,0,F},{F,0,F},{F,0,F},{F,0,F},
	/* 92..98 f32 bin  */ {F,F,F},{F,F,F},{F,F,F},{F,F,F},{F,F,F},{F,F,F},{F,F,F},
	/* 99..9F f64 un   */ {D,0,D},{D,0,D},{D,0,D},{D,0,D},{D,0,D},{D,0,D},{D,0,D},
	/* A0..A6 f64 bin  */ {D,D,D},{D,D,D},{D,D,D},{D,D,D},{D,D,D},{D,D,D},{D,D,D},
	/* A7 i32.wrap_i64 */ {J,0,I},
	/* A8..AB i32.trunc*/ {F,0,I},{F,0,I},{D,0,I},{D,0,I},
	/* AC..AD i64.ext  */ {I,0,J},{I,0,J},
	/* AE..B1 i64.trunc*/ {F,0,J},{F,0,J},{D,0,J},{D,0,J},
	/* B2..B5 f32.conv */ {I,0,F},{I,0,F},{J,0,F},{J,0,F},
	/* B6 f32.demote   */ {D,0,F},
	/* B7..BA f64.conv */ {I,0,D},{I,0,D},{J,0,D},{J,0,D},
	/* BB f64.promote  */ {F,0,D},
	/* BC..BF reinterp */ {F,0,I},{D,0,J},{I,0,F},{J,0,D},
};

/* ---- memory opcodes 0x28..0x3E ---------------------------------------
 * {natural alignment as log2, value type, 1 = load}.  "alignment must not be
 * larger than natural" is an INVALID condition, not a malformed one, which is
 * why it is checked here and not in the decoder. */
struct memrow { uint8_t align, vt, isload; };
static const struct memrow g_mem[0x3F - 0x28] = {
	/* 28 i32.load    */ {2,I,1},
	/* 29 i64.load    */ {3,J,1},
	/* 2A f32.load    */ {2,F,1},
	/* 2B f64.load    */ {3,D,1},
	/* 2C i32.load8_s */ {0,I,1}, /* 2D */ {0,I,1},
	/* 2E i32.load16_s*/ {1,I,1}, /* 2F */ {1,I,1},
	/* 30 i64.load8_s */ {0,J,1}, /* 31 */ {0,J,1},
	/* 32 i64.load16_s*/ {1,J,1}, /* 33 */ {1,J,1},
	/* 34 i64.load32_s*/ {2,J,1}, /* 35 */ {2,J,1},
	/* 36 i32.store   */ {2,I,0},
	/* 37 i64.store   */ {3,J,0},
	/* 38 f32.store   */ {2,F,0},
	/* 39 f64.store   */ {3,D,0},
	/* 3A i32.store8  */ {0,I,0},
	/* 3B i32.store16 */ {1,I,0},
	/* 3C i64.store8  */ {0,J,0},
	/* 3D i64.store16 */ {1,J,0},
	/* 3E i64.store32 */ {2,J,0},
};

/* ---- stack helpers ---------------------------------------------------- */

static void fail(struct wasm_tc *tc, int e) { if (!tc->err) tc->err = e; }

static void push_val(struct wasm_tc *tc, uint8_t t)
{
	if (tc->err) return;
	/* "no result" is not a value.  This must NOT also swallow Unknown -- see
	 * the note on WASM_VT_UNKNOWN in wasm.h; when the two shared the value 0
	 * a polymorphic `select` pushed nothing and the leftover-operand check at
	 * `end` silently stopped firing. */
	if (t == WASM_VT_NORESULT) return;
	if (tc->vh >= tc->vcap) { fail(tc, WASM_E_LIMIT); return; }
	tc->vals[tc->vh++] = t;
}

static uint8_t pop_val(struct wasm_tc *tc)
{
	const struct ctrl *f;
	if (tc->err) return U;
	if (tc->ch == 0) { fail(tc, WASM_E_TYPE); return U; }
	f = &tc->ctrls[tc->ch - 1];
	if (tc->vh <= f->height) {
		if (f->unreachable) return U;
		fail(tc, WASM_E_TYPE);
		return U;
	}
	return tc->vals[--tc->vh];
}

static uint8_t pop_expect(struct wasm_tc *tc, uint8_t want)
{
	uint8_t got = pop_val(tc);
	if (tc->err) return U;
	if (got == U) return want;
	if (want == U) return got;
	if (got != want) { fail(tc, WASM_E_TYPE); return U; }
	return got;
}

static void push_ctrl(struct wasm_tc *tc, uint8_t op, uint8_t out)
{
	if (tc->err) return;
	if (tc->ch >= tc->ccap) { fail(tc, WASM_E_LIMIT); return; }
	tc->ctrls[tc->ch].op = op;
	tc->ctrls[tc->ch].out = out;
	tc->ctrls[tc->ch].unreachable = 0;
	tc->ctrls[tc->ch].height = tc->vh;
	tc->ch++;
}

static struct ctrl pop_ctrl(struct wasm_tc *tc)
{
	struct ctrl f;
	f.op = 0; f.out = 0; f.unreachable = 0; f.height = 0;
	if (tc->err) return f;
	if (tc->ch == 0) { fail(tc, WASM_E_TYPE); return f; }
	f = tc->ctrls[tc->ch - 1];
	if (f.out) pop_expect(tc, f.out);
	if (tc->err) return f;
#ifndef WASM_NEGCTL_STACK
	/* Leftover operands at the end of a block are a type error.  Deleting
	 * this one line is the NEGATIVE CONTROL for the type checker: every
	 * arithmetic and branch rule still fires, every valid module still
	 * validates, and a large slice of the suite's assert_invalid corpus is
	 * silently accepted.  make test-wasm-negctl watches it happen. */
	if (tc->vh != f.height) { fail(tc, WASM_E_TYPE); return f; }
#endif
	tc->ch--;
	return f;
}

static void mark_unreachable(struct wasm_tc *tc)
{
	struct ctrl *f;
	if (tc->err) return;
	if (tc->ch == 0) { fail(tc, WASM_E_TYPE); return; }
	f = &tc->ctrls[tc->ch - 1];
	tc->vh = f->height;
	f->unreachable = 1;
}

/* label_types: a `loop` label takes the loop's PARAMETERS, which in MVP are
 * always none.  Getting this backwards makes every backward branch demand the
 * loop's result type and is the classic MVP validator bug. */
static uint8_t label_out(const struct ctrl *f)
{
	return f->op == OP_LOOP ? (uint8_t)0 : f->out;
}

/* ---- module lookups ---------------------------------------------------- */

static int local_type(struct wasm_tc *tc, uint32_t idx, uint8_t *out)
{
	const struct wasm_functype *ft = tc->ft;
	uint32_t j;
	if (idx < ft->nparams) { *out = ft->params[idx]; return WASM_OK; }
	idx -= ft->nparams;
	for (j = 0; j < tc->code->ndecl; j++) {
		if (idx < tc->code->decl[j].count) { *out = tc->code->decl[j].type; return WASM_OK; }
		idx -= tc->code->decl[j].count;
	}
	return WASM_E_INDEX;
}

/* pop a call's parameters (last first) and push its results */
static void apply_functype(struct wasm_tc *tc, const struct wasm_functype *ft)
{
	uint32_t k;
	for (k = ft->nparams; k > 0; k--) pop_expect(tc, ft->params[k - 1]);
	for (k = 0; k < ft->nresults; k++) push_val(tc, ft->results[k]);
}

/* ---- blocktype --------------------------------------------------------
 * MVP: 0x40 (empty) or a single valtype.  The encoding is s33, and a
 * NON-NEGATIVE value is a type index -- that is multi-value, a real proposal,
 * so it is refused by name.  Reading this as a plain byte instead of an s33
 * is a silent wrong answer on a padded encoding. */
static int rd_blocktype(struct rd *r, uint8_t *out)
{
	int64_t v;
	int e = wasm_rd_s33(r, &v);
	if (e) return e;
	if (v >= 0) return WASM_E_UNSUPPORTED;          /* multi-value */
	switch ((int32_t)v) {
	case -0x40: *out = 0;   return WASM_OK;
	case -0x01: *out = I;   return WASM_OK;
	case -0x02: *out = J;   return WASM_OK;
	case -0x03: *out = F;   return WASM_OK;
	case -0x04: *out = D;   return WASM_OK;
	case -0x05:                                     /* v128 */
	case -0x10: case -0x11:                         /* funcref / externref */
		return WASM_E_UNSUPPORTED;
	default: return WASM_E_VALTYPE;
	}
}

/* ---- the walker -------------------------------------------------------
 * tc == NULL: syntax only (what the spec calls "malformed").
 * tc != NULL: full type check (what the spec calls "invalid").
 * ONE function, so the two passes cannot disagree about what a br_table is. */
int wasm_walk(struct rd *r, struct wasm_tc *tc, struct wasm_sink *sk)
{
	uint32_t depth = 1;    /* the implicit outermost frame */
	int e;

	if (tc) push_ctrl(tc, OP_BLOCK, tc->ft->nresults ? tc->ft->results[0] : 0);

	for (;;) {
		uint8_t op;
		uint32_t pos = r->i;
		if (tc && tc->err) return tc->err;
		e = wasm_rd_u8(r, &op);
		if (e) return e;
		if (sk) sk->insn(sk->ctx, pos, op);

		switch (op) {

		case 0x00:  /* unreachable */
			if (tc) mark_unreachable(tc);
			break;
		case 0x01:  /* nop */
			break;

		case OP_BLOCK: case OP_LOOP: case OP_IF: {
			uint8_t bt;
			e = rd_blocktype(r, &bt); if (e) return e;
			depth++;
			if (tc) {
				if (op == OP_IF) pop_expect(tc, I);
				push_ctrl(tc, op, bt);
			}
			break;
		}
		case OP_ELSE: {
			if (tc) {
				struct ctrl f = pop_ctrl(tc);
				if (tc->err) return tc->err;
				if (f.op != OP_IF) { fail(tc, WASM_E_TYPE); return tc->err; }
				push_ctrl(tc, OP_ELSE, f.out);
			}
			break;
		}
		case OP_END: {
			if (tc) {
				struct ctrl f = pop_ctrl(tc);
				if (tc->err) return tc->err;
				/* an `if` with no `else` may not produce a result */
				if (f.op == OP_IF && f.out) { fail(tc, WASM_E_TYPE); return tc->err; }
				push_val(tc, f.out);
			}
			depth--;
			if (depth == 0) return tc ? tc->err : WASM_OK;
			break;
		}

		case 0x0C: case 0x0D: {   /* br, br_if */
			uint32_t l;
			e = wasm_rd_u32(r, &l); if (e) return e;
			if (tc) {
				uint8_t t;
				if (l >= tc->ch) { fail(tc, WASM_E_INDEX); return tc->err; }
				t = label_out(&tc->ctrls[tc->ch - 1 - l]);
				if (op == 0x0D) {
					pop_expect(tc, I);
					if (t) { pop_expect(tc, t); push_val(tc, t); }
				} else {
					if (t) pop_expect(tc, t);
					mark_unreachable(tc);
				}
			}
			break;
		}
		case 0x0E: {   /* br_table */
			uint32_t n, k, l;
			uint8_t t = 0;
			int have = 0;
			e = wasm_rd_u32(r, &n); if (e) return e;
			if (n > wasm_rd_left(r)) return WASM_E_END;
			for (k = 0; k <= n; k++) {          /* n targets plus the default */
				e = wasm_rd_u32(r, &l); if (e) return e;
				if (!tc) continue;
				if (l >= tc->ch) { fail(tc, WASM_E_INDEX); return tc->err; }
				{
					uint8_t ti = label_out(&tc->ctrls[tc->ch - 1 - l]);
					/* MVP arity is 0 or 1, so "same arity" and "same
					 * type" are one condition. */
					if (!have) { t = ti; have = 1; }
					else if (ti != t) { fail(tc, WASM_E_TYPE); return tc->err; }
				}
			}
			if (tc) {
				pop_expect(tc, I);
				if (t) pop_expect(tc, t);
				mark_unreachable(tc);
			}
			break;
		}
		case 0x0F:   /* return */
			if (tc) {
				if (tc->ft->nresults) pop_expect(tc, tc->ft->results[0]);
				mark_unreachable(tc);
			}
			break;

		case 0x10: {   /* call */
			uint32_t f;
			e = wasm_rd_u32(r, &f); if (e) return e;
			if (tc) {
				if (f >= tc->m->total_funcs) { fail(tc, WASM_E_INDEX); return tc->err; }
				if (tc->m->functype_of[f] >= tc->m->ntypes) { fail(tc, WASM_E_INDEX); return tc->err; }
				apply_functype(tc, &tc->m->types[tc->m->functype_of[f]]);
			}
			break;
		}
		case 0x11: {   /* call_indirect */
			uint32_t ty; uint8_t z;
			e = wasm_rd_u32(r, &ty); if (e) return e;
			e = wasm_rd_u8(r, &z);   if (e) return e;
			/* MVP spells the table as a single zero byte.  Reference types
			 * makes it a table index; accepting a non-zero value here would
			 * silently call through the wrong table. */
			if (z != 0x00) return WASM_E_RESERVED;
			if (tc) {
				if (tc->m->total_tables == 0) { fail(tc, WASM_E_INDEX); return tc->err; }
				if (ty >= tc->m->ntypes) { fail(tc, WASM_E_INDEX); return tc->err; }
				pop_expect(tc, I);
				apply_functype(tc, &tc->m->types[ty]);
			}
			break;
		}

		case 0x1A:   /* drop */
			if (tc) pop_val(tc);
			break;
		case 0x1B: { /* select */
			if (tc) {
				uint8_t t1, t2;
				pop_expect(tc, I);
				t1 = pop_val(tc);
				t2 = pop_val(tc);
				if (tc->err) return tc->err;
				if (t1 != U && t2 != U && t1 != t2) { fail(tc, WASM_E_TYPE); return tc->err; }
				push_val(tc, t1 == U ? t2 : t1);
			}
			break;
		}
		case 0x1C:   /* typed select: reference types */
			return WASM_E_UNSUPPORTED;

		case 0x20: case 0x21: case 0x22: {   /* local.get/set/tee */
			uint32_t l;
			e = wasm_rd_u32(r, &l); if (e) return e;
			if (tc) {
				uint8_t t;
				if (local_type(tc, l, &t)) { fail(tc, WASM_E_INDEX); return tc->err; }
				if (op == 0x20) push_val(tc, t);
				else if (op == 0x21) pop_expect(tc, t);
				else { pop_expect(tc, t); push_val(tc, t); }
			}
			break;
		}
		case 0x23: case 0x24: {   /* global.get/set */
			uint32_t g;
			e = wasm_rd_u32(r, &g); if (e) return e;
			if (tc) {
				if (g >= tc->m->total_globals) { fail(tc, WASM_E_INDEX); return tc->err; }
				if (op == 0x23) push_val(tc, tc->m->globaltype_of[g].valtype);
				else {
					if (!tc->m->globaltype_of[g].mut) { fail(tc, WASM_E_MUT_GLOBAL); return tc->err; }
					pop_expect(tc, tc->m->globaltype_of[g].valtype);
				}
			}
			break;
		}

		case 0x3F: case 0x40: {   /* memory.size / memory.grow */
			uint8_t z;
			e = wasm_rd_u8(r, &z); if (e) return e;
			if (z != 0x00) return WASM_E_RESERVED;
			if (tc) {
				if (tc->m->total_mems == 0) { fail(tc, WASM_E_INDEX); return tc->err; }
				if (op == 0x40) pop_expect(tc, I);
				push_val(tc, I);
			}
			break;
		}

		case 0x41: { int32_t v; e = wasm_rd_s32(r, &v); if (e) return e; if (tc) push_val(tc, I); break; }
		case 0x42: { int64_t v; e = wasm_rd_s64(r, &v); if (e) return e; if (tc) push_val(tc, J); break; }
		case 0x43: { e = wasm_rd_skip(r, 4); if (e) return e; if (tc) push_val(tc, F); break; }
		case 0x44: { e = wasm_rd_skip(r, 8); if (e) return e; if (tc) push_val(tc, D); break; }

		case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4:
			return WASM_E_UNSUPPORTED;    /* sign-extension operators */
		case 0xD0: case 0xD1: case 0xD2:
			return WASM_E_UNSUPPORTED;    /* ref.null / ref.is_null / ref.func */
		case 0xFC: case 0xFD: case 0xFE:
			/* saturating truncation + bulk memory / SIMD / atomics */
			return WASM_E_UNSUPPORTED;

		default:
			if (op >= 0x28 && op <= 0x3E) {          /* loads and stores */
				const struct memrow *mr = &g_mem[op - 0x28];
				uint32_t align, off;
				e = wasm_rd_u32(r, &align); if (e) return e;
				e = wasm_rd_u32(r, &off);   if (e) return e;
				if (tc) {
					if (align > mr->align) { fail(tc, WASM_E_ALIGN); return tc->err; }
					if (tc->m->total_mems == 0) { fail(tc, WASM_E_INDEX); return tc->err; }
					if (mr->isload) { pop_expect(tc, I); push_val(tc, mr->vt); }
					else { pop_expect(tc, mr->vt); pop_expect(tc, I); }
				}
				break;
			}
			if (op >= 0x45 && op <= 0xBF) {
				const struct numrow *nr = &g_num[op - 0x45];
				if (tc) {
					if (nr->b) { pop_expect(tc, nr->b); pop_expect(tc, nr->a); }
					else pop_expect(tc, nr->a);
					push_val(tc, nr->res);
				}
				break;
			}
			return WASM_E_OPCODE;
		}
	}
}

int wasm_expr_extent(struct rd *r) { return wasm_walk(r, 0, 0); }

/* ---- constant expressions ---------------------------------------------
 * MVP: exactly one of i32/i64/f32/f64.const or global.get of an IMPORTED
 * IMMUTABLE global, then `end`.  A defined global is not available -- at the
 * time an initialiser runs, the globals after it do not exist yet, and the
 * spec closes that by restricting the index space rather than by ordering. */
static int const_expr(const struct wasm_module *m, struct wasm_span s, uint8_t want)
{
	struct rd r;
	uint8_t op, got = 0;
	int e;
	r.p = m->bytes; r.i = s.off; r.n = s.off + s.len;

	e = wasm_rd_u8(&r, &op); if (e) return e;
	switch (op) {
	case 0x41: { int32_t v; e = wasm_rd_s32(&r, &v); if (e) return e; got = I; break; }
	case 0x42: { int64_t v; e = wasm_rd_s64(&r, &v); if (e) return e; got = J; break; }
	case 0x43: { e = wasm_rd_skip(&r, 4); if (e) return e; got = F; break; }
	case 0x44: { e = wasm_rd_skip(&r, 8); if (e) return e; got = D; break; }
	case 0x23: {
		uint32_t g;
		e = wasm_rd_u32(&r, &g); if (e) return e;
		if (g >= m->nimp_globals) return WASM_E_INDEX;      /* unknown global */
		if (m->globaltype_of[g].mut) return WASM_E_CONSTEXPR;
		got = m->globaltype_of[g].valtype;
		break;
	}
	case 0x0B:
		return WASM_E_TYPE;                                  /* produces nothing */
	default:
		return WASM_E_CONSTEXPR;
	}
	e = wasm_rd_u8(&r, &op); if (e) return e;
	if (op != OP_END) return WASM_E_CONSTEXPR;
	if (r.i != r.n) return WASM_E_JUNK;
	if (got != want) return WASM_E_TYPE;
	return WASM_OK;
}

/* ---- module-level validation ------------------------------------------ */

static int limits_ok(const struct wasm_limits *l, uint32_t ceiling, int is_mem)
{
	if (is_mem) {
		if (l->min > ceiling) return WASM_E_MEMSIZE;
		if (l->has_max && l->max > ceiling) return WASM_E_MEMSIZE;
	}
	if (l->has_max && l->min > l->max) return WASM_E_LIMITS;
	return WASM_OK;
}

static int span_eq(const struct wasm_module *m, struct wasm_span a, struct wasm_span b)
{
	uint32_t k;
	if (a.len != b.len) return 0;
	for (k = 0; k < a.len; k++)
		if (m->bytes[a.off + k] != m->bytes[b.off + k]) return 0;
	return 1;
}

#define VAL_STACK_CAP 65536u
#define CTRL_STACK_CAP 4096u

int wasm_validate(struct wasm_module *m, struct wasm_arena *a)
{
	struct wasm_tc tc;
	uint32_t i, j;
	int e;

	/* --- types referenced by every function, imported or defined --- */
	for (i = 0; i < m->total_funcs; i++)
		if (m->functype_of[i] >= m->ntypes) return WASM_E_INDEX;

	/* --- tables and memories: MVP allows at most one of each --- */
	if (m->total_tables > 1) return WASM_E_MULTI_TABLE;
	if (m->total_mems > 1) return WASM_E_MULTI_MEMORY;
	for (i = 0; i < m->nimports; i++) {
		const struct wasm_import *im = &m->imports[i];
		if (im->kind == WASM_EXT_TABLE) {
			e = limits_ok(&im->tt.lim, 0xFFFFFFFFu, 0); if (e) return e;
		} else if (im->kind == WASM_EXT_MEM) {
			e = limits_ok(&im->mem, 65536u, 1); if (e) return e;
		}
	}
	for (i = 0; i < m->ntables; i++) {
		e = limits_ok(&m->tables[i].lim, 0xFFFFFFFFu, 0); if (e) return e;
	}
	for (i = 0; i < m->nmems; i++) {
		e = limits_ok(&m->mems[i], 65536u, 1); if (e) return e;
	}

	/* --- globals --- */
	for (i = 0; i < m->nglobals; i++) {
		e = const_expr(m, m->globals[i].init, m->globals[i].gt.valtype);
		if (e) return e;
	}

	/* --- exports: index in range, and names UNIQUE --- */
	for (i = 0; i < m->nexports; i++) {
		const struct wasm_export *x = &m->exports[i];
		switch (x->kind) {
		case WASM_EXT_FUNC:   if (x->index >= m->total_funcs)   return WASM_E_INDEX; break;
		case WASM_EXT_TABLE:  if (x->index >= m->total_tables)  return WASM_E_INDEX; break;
		case WASM_EXT_MEM:    if (x->index >= m->total_mems)    return WASM_E_INDEX; break;
		case WASM_EXT_GLOBAL: if (x->index >= m->total_globals) return WASM_E_INDEX; break;
		default: return WASM_E_IMPORT_KIND;
		}
		for (j = 0; j < i; j++)
			if (span_eq(m, x->name, m->exports[j].name)) return WASM_E_DUP_EXPORT;
	}

	/* --- start --- */
	if (m->has_start) {
		const struct wasm_functype *ft;
		if (m->start >= m->total_funcs) return WASM_E_INDEX;
		ft = &m->types[m->functype_of[m->start]];
		if (ft->nparams != 0 || ft->nresults != 0) return WASM_E_START;
	}

	/* --- element segments --- */
	for (i = 0; i < m->nelems; i++) {
		struct rd fr;
		if (m->elems[i].tableidx >= m->total_tables) return WASM_E_INDEX;
		e = const_expr(m, m->elems[i].offset, I); if (e) return e;
		fr.p = m->elems[i].funcs_raw; fr.i = 0; fr.n = m->elems[i].funcs_raw_len;
		for (j = 0; j < m->elems[i].nfunc; j++) {
			uint32_t f;
			e = wasm_rd_u32(&fr, &f); if (e) return e;
			if (f >= m->total_funcs) return WASM_E_INDEX;
		}
	}

	/* --- data segments --- */
	for (i = 0; i < m->ndatas; i++) {
		if (m->datas[i].memidx >= m->total_mems) return WASM_E_INDEX;
		e = const_expr(m, m->datas[i].offset, I); if (e) return e;
	}

	/* --- every function body --- */
	tc.m = m;
	tc.vals = wasm_arena_alloc(a, VAL_STACK_CAP, 1);
	if (!tc.vals) return WASM_E_NOMEM;
	tc.ctrls = wasm_arena_alloc(a, CTRL_STACK_CAP, sizeof *tc.ctrls);
	if (!tc.ctrls) return WASM_E_NOMEM;
	tc.vcap = VAL_STACK_CAP;
	tc.ccap = CTRL_STACK_CAP;

	for (i = 0; i < m->ncode; i++) {
		struct rd r;
		tc.ft = &m->types[m->funcs[i]];
		tc.code = &m->code[i];
		tc.vh = 0; tc.ch = 0; tc.err = 0;
		r.p = m->bytes;
		r.i = m->code[i].expr.off;
		r.n = m->code[i].expr.off + m->code[i].expr.len;
		e = wasm_walk(&r, &tc, 0);
		if (e) return e;
		if (tc.err) return tc.err;
		/* wasm_walk returns when the outermost frame closes; the closing
		 * `end` must also be the last byte of the body. */
		if (r.i != r.n) return WASM_E_JUNK;
		if (tc.ch != 0) return WASM_E_TYPE;
	}
	return WASM_OK;
}

/* See the note beside the definitions: these are file-local by intent, and
 * this file is textually included by c/apps/browser/js_wasm.c. */
#undef I
#undef J
#undef F
#undef D
#undef U
