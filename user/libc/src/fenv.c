/* Rounding control via SSE MXCSR (QuickJS's dtoa/atof toggle the mode). The
 * MXCSR rounding-control field is bits 13-14. */
#include <fenv.h>

static unsigned get_mxcsr(void) { unsigned v; __asm__ volatile ("stmxcsr %0" : "=m"(v)); return v; }
static void set_mxcsr(unsigned v) { __asm__ volatile ("ldmxcsr %0" : : "m"(v)); }

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
    unsigned m = get_mxcsr();
    m = (m & ~(3u << 13)) | rc_bits(round);
    set_mxcsr(m);
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
int feclearexcept(int e) { (void)e; return 0; }
int fetestexcept(int e)  { (void)e; return 0; }
int feraiseexcept(int e) { (void)e; return 0; }
