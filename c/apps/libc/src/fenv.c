/* Floating-point environment, mapped onto SSE's MXCSR.
 *
 * The exception queries used to be stubs returning 0 -- which is a lie with
 * consequences: code that computes, then asks fetestexcept(FE_OVERFLOW) to
 * decide whether the result is usable, was told "no overflow" every time. MXCSR
 * has had the real sticky flags all along (bits 0..5, in the same order as the
 * FE_* names), so the implementation is a read and a mask. */
#include <fenv.h>

static unsigned get_mxcsr(void) { unsigned v; __asm__ volatile ("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(unsigned v) { __asm__ volatile ("ldmxcsr %0" : : "m"(v)); }

/* Default environment: round to nearest, all exceptions masked, flags clear --
 * the state boot/long.asm leaves MXCSR in. */
const fenv_t __fe_dfl_env = { 0x1f80 };

static unsigned rc_bits(int round)
{
    switch (round) {
    case FE_DOWNWARD:   return 1u << 13;
    case FE_UPWARD:     return 2u << 13;
    case FE_TOWARDZERO: return 3u << 13;
    default:            return 0;            /* FE_TONEAREST */
    }
}

int fesetround(int round)
{
    if (round != FE_TONEAREST && round != FE_DOWNWARD &&
        round != FE_UPWARD && round != FE_TOWARDZERO) return 1;
    set_mxcsr((get_mxcsr() & ~(3u << 13)) | rc_bits(round));
    return 0;
}
int fegetround(void)
{
    switch ((get_mxcsr() >> 13) & 3u) {
    case 1: return FE_DOWNWARD;
    case 2: return FE_UPWARD;
    case 3: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
    }
}

int feclearexcept(int e) { set_mxcsr(get_mxcsr() & ~((unsigned)e & FE_ALL_EXCEPT)); return 0; }
int fetestexcept(int e)  { return (int)(get_mxcsr() & (unsigned)e & FE_ALL_EXCEPT); }

/* Raising is done by setting the sticky flag. The exceptions are all masked
 * here, so this cannot trap -- which is the observable difference from a
 * hardware-raised exception on a system that unmasks them, and the reason this
 * is a flag write rather than an arithmetic trick. */
int feraiseexcept(int e) { set_mxcsr(get_mxcsr() | ((unsigned)e & FE_ALL_EXCEPT)); return 0; }

int fegetexceptflag(fexcept_t *out, int e)
{ if (!out) return 1; *out = get_mxcsr() & (unsigned)e & FE_ALL_EXCEPT; return 0; }
int fesetexceptflag(const fexcept_t *in, int e)
{
    if (!in) return 1;
    unsigned m = get_mxcsr() & ~((unsigned)e & FE_ALL_EXCEPT);
    set_mxcsr(m | (*in & (unsigned)e & FE_ALL_EXCEPT));
    return 0;
}

int fegetenv(fenv_t *env) { if (!env) return 1; env->__mxcsr = get_mxcsr(); return 0; }
int fesetenv(const fenv_t *env) { if (!env) return 1; set_mxcsr(env->__mxcsr); return 0; }
int feholdexcept(fenv_t *env)
{
    if (!env) return 1;
    env->__mxcsr = get_mxcsr();
    set_mxcsr((env->__mxcsr & ~(unsigned)FE_ALL_EXCEPT) | 0x1f80u);  /* clear flags, mask all */
    return 0;
}
int feupdateenv(const fenv_t *env)
{
    if (!env) return 1;
    unsigned raised = get_mxcsr() & FE_ALL_EXCEPT;
    set_mxcsr(env->__mxcsr);
    return feraiseexcept((int)raised);
}
