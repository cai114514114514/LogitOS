#ifndef _FENV_H
#define _FENV_H
/* Rounding and exception control mapped to SSE MXCSR (see src/fenv.c).
 *
 * The x87 unit is NOT consulted: every floating-point value in this userland is
 * a double in an XMM register (the kernel enables SSE at boot and the Makefile
 * builds with -msse2), so MXCSR is the whole floating-point environment that
 * matters here. A program using long double via x87 will not see its exceptions
 * reflected in fetestexcept -- named here rather than discovered later. */

#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0c00

/* MXCSR exception-flag bit positions (bits 0..5), which is also the order the
 * standard names them in. */
#define FE_INVALID    0x01
#define FE_DENORMAL   0x02
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT 0x3f

typedef unsigned int fexcept_t;
typedef struct { unsigned int __mxcsr; } fenv_t;

extern const fenv_t __fe_dfl_env;
#define FE_DFL_ENV (&__fe_dfl_env)

int fesetround(int);
int fegetround(void);
int feclearexcept(int);
int fetestexcept(int);
int feraiseexcept(int);
int fegetexceptflag(fexcept_t *, int);
int fesetexceptflag(const fexcept_t *, int);
int fegetenv(fenv_t *);
int fesetenv(const fenv_t *);
int feholdexcept(fenv_t *);
int feupdateenv(const fenv_t *);

#endif
