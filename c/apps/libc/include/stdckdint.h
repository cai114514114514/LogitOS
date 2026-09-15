#ifndef _STDCKDINT_H
#define _STDCKDINT_H

/* C23 checked arithmetic.  Clang's overflow builtins perform the operation in
 * the destination type and return true exactly when the mathematical result
 * is not representable; unlike hand-written bounds tests they also cover all
 * signed/unsigned mixtures without evaluating an argument twice. */
#define ckd_add(result, a, b) __builtin_add_overflow((a), (b), (result))
#define ckd_sub(result, a, b) __builtin_sub_overflow((a), (b), (result))
#define ckd_mul(result, a, b) __builtin_mul_overflow((a), (b), (result))

#endif /* _STDCKDINT_H */
