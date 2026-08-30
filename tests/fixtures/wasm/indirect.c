/* Function pointers, which is the only way to make clang emit the two things
 * the other three fixtures never produce: an ELEMENT SECTION and
 * `call_indirect`.  Those are a distinct path on both sides of this library --
 * the decoder has to read a table type, an offset const-expr and a vector of
 * function indices, and the validator has to check the type index, refuse the
 * MVP reserved byte when it is non-zero, and require that a table exists at
 * all.  None of that is exercised by arith/control/memory.
 *
 * Signed narrow loads are here for the same reason: memory.c stores and reads
 * back through unsigned types, so i32.load8_s / i32.load16_s / i64.load32_s
 * -- three rows of the validator's memory table, each with its own natural
 * alignment -- had no producer in this corpus. */
typedef unsigned u32;
typedef signed char s8;
typedef short s16;
typedef long long s64;

static u32 add1(u32 x) { return x + 1; }
static u32 dbl(u32 x)  { return x * 2; }
static u32 neg(u32 x)  { return 0u - x; }
static u32 sq(u32 x)   { return x * x; }

/* A table clang must materialise as an elem segment, and a call through it. */
static u32 (*const ops[4])(u32) = { add1, dbl, neg, sq };

u32 dispatch(u32 which, u32 x) { return ops[which & 3](x); }

/* An indirect call whose callee is chosen at run time and whose type is not
 * the same as `ops`', so the module carries two distinct functypes reachable
 * only through call_indirect. */
typedef u32 (*binop)(u32, u32);
static u32 sum(u32 a, u32 b)  { return a + b; }
static u32 diff(u32 a, u32 b) { return a - b; }
u32 apply2(int pick, u32 a, u32 b) { binop f = pick ? sum : diff; return f(a, b); }

/* NOT `static`, and there is a writer: with a static array that nothing
 * stores into, clang proves every element is zero and folds the whole of
 * signed_loads to a constant -- the fixture then compiles, validates, and
 * exercises none of what it is here for.  Checked with wasm-objdump rather
 * than assumed; the first draft of this file emitted exactly one i32.load. */
s8 buf[64];
void poke(u32 i, int v) { buf[i & 31] = (s8)v; }

s64 signed_loads(u32 i) {
	s8  *b = buf + (i & 31);
	s16 *h = (s16 *)(buf + ((i * 2) & 30));
	int *w = (int *)(buf + ((i * 4) & 28));
	return (s64)*b + (s64)*h + (s64)*w;
}
