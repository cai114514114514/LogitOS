/* The float-suffixed math functions that need no transcendental code.
 *
 * WHY ONLY THESE. <math.h> has always DECLARED sqrtf/fabsf/floorf/truncf/
 * roundf/copysignf/scalbnf; nothing DEFINED them, because third_party/libm is a
 * double-only subset of musl. Any program that called one got a link error at
 * the very end of its build. The ones below are exactly those that can be
 * implemented from IEEE bit manipulation and the SSE sqrt instruction -- no
 * libm dependency -- which matters because /bin/as links mini-libc WITHOUT
 * libm, so a reference to sin() from this file would break that link.
 *
 * The transcendental float forms (sinf, cosf, expf, logf, powf, ...) are
 * therefore still declared and still undefined. That is the honest state: a
 * program using them fails at link time with a named missing symbol, rather
 * than silently getting a double-rounded answer from a wrapper. */
#include <math.h>
#include <stdint.h>

static float bits_to_f(uint32_t u) { union { uint32_t u; float f; } v = { u }; return v.f; }
static uint32_t f_to_bits(float f) { union { float f; uint32_t u; } v = { f }; return v.u; }

float sqrtf(float x) { float r; __asm__ ("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }
float fabsf(float x) { return bits_to_f(f_to_bits(x) & 0x7fffffffu); }
float copysignf(float x, float y)
{ return bits_to_f((f_to_bits(x) & 0x7fffffffu) | (f_to_bits(y) & 0x80000000u)); }

/* Exact integer-part extraction by bit surgery: the only way to get floorf
 * right for values whose exponent puts the binary point inside the mantissa,
 * and the only way to keep -0.0 and the infinities correct. */
static float trunc_bits(float x, int mode)   /* 0 trunc, 1 floor, 2 ceil, 3 round-half-away */
{
    uint32_t u = f_to_bits(x);
    int e = (int)((u >> 23) & 0xff) - 127;
    uint32_t sign = u & 0x80000000u;
    if (e >= 23) return x;                    /* already integral (or inf/nan) */
    if (e < 0) {                              /* |x| < 1 */
        float zero = bits_to_f(sign);
        if (mode == 1) return sign ? (x == zero ? zero : -1.0f) : zero;
        if (mode == 2) return sign ? zero : (x == zero ? zero : 1.0f);
        if (mode == 3) return (e == -1) ? bits_to_f(sign | f_to_bits(1.0f)) : zero;
        return zero;
    }
    uint32_t mask = (1u << (23 - e)) - 1u;
    if ((u & mask) == 0) return x;            /* exactly integral */
    uint32_t t = u & ~mask;
    float ti = bits_to_f(t);
    switch (mode) {
    case 1: return sign ? ti - 1.0f : ti;
    case 2: return sign ? ti : ti + 1.0f;
    case 3: { float frac = fabsf(x) - fabsf(ti);
              return frac >= 0.5f ? (sign ? ti - 1.0f : ti + 1.0f) : ti; }
    default: return ti;
    }
}
float truncf(float x) { return trunc_bits(x, 0); }
float floorf(float x) { return trunc_bits(x, 1); }
float ceilf(float x)  { return trunc_bits(x, 2); }
float roundf(float x) { return trunc_bits(x, 3); }
/* rintf/nearbyintf honour the current rounding mode; cvtss2si+cvtsi2ss does
 * exactly that in one instruction pair, and leaves inf/nan alone. */
float rintf(float x)
{
    uint32_t u = f_to_bits(x);
    int e = (int)((u >> 23) & 0xff) - 127;
    if (e >= 23) return x;
    int32_t i; __asm__ ("cvtss2si %1, %0" : "=r"(i) : "x"(x));
    float r; __asm__ ("cvtsi2ss %1, %0" : "=x"(r) : "r"(i));
    return copysignf(r, x);
}
float nearbyintf(float x) { return rintf(x); }
long  lrintf(float x)  { long i; __asm__ ("cvtss2si %1, %0" : "=r"(i) : "x"(x)); return i; }
long long llrintf(float x) { long long i; __asm__ ("cvtss2si %1, %0" : "=r"(i) : "x"(x)); return i; }
long  lroundf(float x) { return (long)roundf(x); }
long long llroundf(float x) { return (long long)roundf(x); }

float fminf(float a, float b) { if (a != a) return b; if (b != b) return a; return a < b ? a : b; }
float fmaxf(float a, float b) { if (a != a) return b; if (b != b) return a; return a > b ? a : b; }
float fdimf(float a, float b) { if (a != a || b != b) return a + b; return a > b ? a - b : 0.0f; }

float scalbnf(float x, int n)
{
    /* Applied in steps so that a huge n cannot make the intermediate overflow
     * before the final underflow (or vice versa) -- the classic scalbn bug. */
    if (n > 127) { x *= 1.7014118346046923e38f; n -= 127;
                   if (n > 127) { x *= 1.7014118346046923e38f; n -= 127; if (n > 127) n = 127; } }
    else if (n < -126) { x *= 1.1754943508222875e-38f * 0x1p23f; n += 126 - 23;
                         if (n < -126) { x *= 1.1754943508222875e-38f * 0x1p23f; n += 126 - 23;
                                         if (n < -126) n = -126; } }
    return x * bits_to_f((uint32_t)(127 + n) << 23);
}
float ldexpf(float x, int n) { return scalbnf(x, n); }
float frexpf(float x, int *e)
{
    uint32_t u = f_to_bits(x);
    int ee = (int)((u >> 23) & 0xff);
    if (ee == 0) {                       /* zero or subnormal */
        if (x == 0) { *e = 0; return x; }
        x = frexpf(x * 0x1p64f, e); *e -= 64; return x;
    }
    if (ee == 0xff) { *e = 0; return x; }   /* inf / nan */
    *e = ee - 126;
    return bits_to_f((u & 0x807fffffu) | 0x3f000000u);
}
float modff(float x, float *ip)
{
    float i = truncf(x);
    *ip = i;
    if (x != x || (fabsf(x) == fabsf(x) && f_to_bits(fabsf(x)) >= 0x7f800000u))
        return copysignf(0.0f, x);          /* inf: fractional part is +-0 */
    return x - i;
}
float fmodf(float x, float y)
{
    if (y == 0 || x != x || y != y) return (x * y) / (x * y);   /* NaN */
    if (fabsf(x) == fabsf(x) && f_to_bits(fabsf(x)) >= 0x7f800000u) return (x - x) / (x - x);
    float r = x;
    if (fabsf(y) >= fabsf(r)) { if (fabsf(y) == fabsf(r)) return copysignf(0.0f, x); return r; }
    /* Repeated scaled subtraction: exact, because each step is an exact
     * subtraction of a scaled y from a value of comparable magnitude. */
    int ex, ey;
    frexpf(r, &ex); frexpf(y, &ey);
    for (int k = ex - ey; k >= 0; k--) {
        float t = scalbnf(fabsf(y), k);
        if (t <= fabsf(r)) r = copysignf(fabsf(r) - t, r);
    }
    return r;
}
float hypotf(float a, float b)
{
    /* Computed in double: every float pair fits a double with room for the
     * squares, so there is no overflow to guard against and one rounding.
     * The square root is the SSE instruction, not __builtin_sqrt -- clang
     * lowers that builtin to a call to libm's sqrt(), and /bin/as links this
     * library WITHOUT libm. */
    double x = (double)a, y = (double)b, s = x * x + y * y, r;
    __asm__ ("sqrtsd %1, %0" : "=x"(r) : "x"(s));
    return (float)r;
}
float nanf(const char *tag) { (void)tag; return __builtin_nanf(""); }
