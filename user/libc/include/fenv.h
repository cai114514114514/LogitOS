#ifndef _FENV_H
#define _FENV_H
/* Rounding control mapped to SSE MXCSR (see user/libc/src/fenv.c). Values match
 * the MXCSR RC field shifted to the fenv API's expectations. */
#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0c00

#define FE_INVALID    0x01
#define FE_DENORMAL   0x02
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT 0x3f

int fesetround(int);
int fegetround(void);
int feclearexcept(int);
int fetestexcept(int);
int feraiseexcept(int);

#endif
