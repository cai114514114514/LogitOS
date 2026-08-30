/* Compiled to wasm32 by the Homebrew clang and validated by c/lib/wasm.
 * The point is that these modules were produced by a REAL toolchain rather
 * than by us: their instruction mix, their local declarations and their
 * br_table shapes are somebody else's idea of what a module looks like. */
typedef unsigned u32; typedef unsigned long long u64;
u32 fib(u32 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
u64 mix(u64 a, double b, float c) {
	u64 r = a;
	for (u32 i = 0; i < 8; i++) { r = r * 6364136223846793005ULL + (u64)(b * i) + (u64)c; r ^= r >> 13; }
	return r;
}
double poly(double x) { return x * x - 2.0 * x + 1.0; }
float fconv(int i, long long l, double d) { return (float)i + (float)l + (float)d; }
int trunc_all(float f, double d) { return (int)f + (int)d + (int)(unsigned)f; }
