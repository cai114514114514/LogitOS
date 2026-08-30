/* Internal shared surface between wasm_parse.c and wasm_valid.c.
 *
 * ONE JAR, ONE DOOR.  The immediate layout of every opcode is spelled EXACTLY
 * ONCE, in wasm_valid.c's walk(), and both passes go through it: the decode
 * pass calls it with a NULL type-check context (syntax only, which is what
 * the spec calls "malformed"), the validate pass calls it with one (typing,
 * which is what the spec calls "invalid").  A second copy of "br_table takes
 * a vector of u32 then one more u32" is the shape CLAUDE.md's rule 3 is
 * about: two spellings of one constant agree on the wrong value about as
 * often as the right one.
 */
#ifndef C_LIB_WASM_WASM_INT_H_
#define C_LIB_WASM_WASM_INT_H_

#include "wasm.h"

struct rd {
	const uint8_t *p;
	uint32_t n;      /* end offset, exclusive, in module coordinates */
	uint32_t i;      /* cursor, in module coordinates */
};

/* All of these return a WASM_E_* code and leave the cursor unspecified on
 * failure.  Every bound is written `off > n || size > n - off` and never
 * `off + size > n` -- modelf.c's rule, and its reason: they are the same
 * expression until the sum overflows and then they are opposites. */
int wasm_rd_u8(struct rd *r, uint8_t *out);
int wasm_rd_u32(struct rd *r, uint32_t *out);        /* uN, N=32 */
int wasm_rd_s32(struct rd *r, int32_t *out);         /* sN, N=32 */
int wasm_rd_s33(struct rd *r, int64_t *out);         /* sN, N=33 -- blocktype */
int wasm_rd_s64(struct rd *r, int64_t *out);         /* sN, N=64 */
int wasm_rd_skip(struct rd *r, uint32_t nbytes);
int wasm_rd_name(struct rd *r, struct wasm_span *out);
int wasm_rd_valtype(struct rd *r, uint8_t *out);

/* number of bytes still readable */
static inline uint32_t wasm_rd_left(const struct rd *r) { return r->n - r->i; }

/* An observer of the walk.  `insn` is called once per instruction, at the
 * moment the opcode byte has been read and BEFORE its immediates are consumed,
 * with `pos` the offset of the opcode byte itself.
 *
 * THIS EXISTS SO THE INTERPRETER DOES NOT BECOME A THIRD DOOR ON THE SAME JAR.
 * wasm_exec.c has to know two structural things about a body -- where each
 * block's matching `else`/`end` is, and where every instruction begins -- and
 * both are derivable from the order in which this callback fires.  Deriving
 * them with a second scanner would put "how long is a br_table" in two files,
 * which is the shape CLAUDE.md rule 3 is about.  The interpreter still reads
 * its own immediates while executing (it must), but it checks every cursor
 * position against the boundary set this walk produced, so a disagreement
 * between the two is a named trap instead of a plausible wrong answer. */
struct wasm_sink {
	void (*insn)(void *ctx, uint32_t pos, uint8_t op);
	void *ctx;
};

/* The walker.  m may be NULL only when tc is NULL.  See wasm_valid.c.
 * `sk` may be NULL; it is orthogonal to tc. */
struct wasm_tc;
int wasm_walk(struct rd *r, struct wasm_tc *tc, struct wasm_sink *sk);

/* syntax-only walk of one expression, cursor left just past its closing
 * `end`.  Used by the decoder to find a const-expr's extent. */
int wasm_expr_extent(struct rd *r);

#endif
