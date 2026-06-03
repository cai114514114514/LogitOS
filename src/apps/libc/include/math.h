#ifndef _MATH_H
#define _MATH_H

/* Freestanding math.h for QuickJS + musl libm. Classification via clang
 * builtins; the rest are declared here and implemented by musl (third_party/libm)
 * or user/libc. */

typedef double double_t;
typedef float  float_t;

#define NAN        (__builtin_nanf(""))
#define INFINITY   (__builtin_inff())
#define HUGE_VAL   (__builtin_inf())
#define HUGE_VALF  (__builtin_inff())

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN 2147483647

#define fpclassify(x) __builtin_fpclassify(FP_NAN,FP_INFINITE,FP_NORMAL,FP_SUBNORMAL,FP_ZERO,(x))
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x)  __builtin_signbit(x)
#define isgreater(x,y)      __builtin_isgreater(x,y)
#define isless(x,y)         __builtin_isless(x,y)
#define isunordered(x,y)    __builtin_isunordered(x,y)

#define M_PI   3.14159265358979323846
#define M_E    2.7182818284590452354
#define M_LN2  0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_LOG2E 1.4426950408889634074
#define M_SQRT2 1.41421356237309504880

double acos(double); double acosh(double); double asin(double); double asinh(double);
double atan(double); double atan2(double,double); double atanh(double);
double cbrt(double); double ceil(double); double copysign(double,double);
double cos(double); double cosh(double); double erf(double); double erfc(double);
double exp(double); double exp2(double); double expm1(double);
double fabs(double); double fdim(double,double); double floor(double);
double fma(double,double,double); double fmax(double,double); double fmin(double,double);
double fmod(double,double); double frexp(double,int*); double hypot(double,double);
int    ilogb(double); double ldexp(double,int); double lgamma(double);
double log(double); double log10(double); double log1p(double); double log2(double);
double logb(double); long long llrint(double); long llround(double);
long   lrint(double); long lround(double); double modf(double,double*);
double nan(const char*); double nearbyint(double); double nextafter(double,double);
double pow(double,double); double remainder(double,double); double remquo(double,double,int*);
double rint(double); double round(double); double scalbln(double,long);
double scalbn(double,int); double sin(double); double sinh(double); double sqrt(double);
double tan(double); double tanh(double); double tgamma(double); double trunc(double);
double significand(double); double drem(double,double);
double j0(double); double j1(double); double y0(double); double y1(double);

/* float versions a few musl files reference internally */
float sqrtf(float); float fabsf(float); float scalbnf(float,int); float copysignf(float,float);
float floorf(float); float truncf(float); float roundf(float);

#endif
