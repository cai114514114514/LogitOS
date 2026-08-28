#include "mldsa.h"
#include "rng.h"

/* The randomised entry points, split into their own translation unit for the
 * same reason mlkem_rand.c is split from mlkem.c: mldsa.c must link into a
 * HOST test that has no kernel RNG, and the KAT/differential gates that
 * matter can only run against the derandomised _internal forms and the
 * deterministic (rnd = 0^32) mldsa_sign()/mldsa_verify() pair. Keeping the RNG
 * dependency out of mldsa.c means the tested object and the shipped object
 * are the same code, not the same code minus an #ifdef.
 *
 * Not gated on rng_strong() -- same reasoning as mlkem_rand.c: whatever calls
 * this is responsible for having already refused to proceed on a weak RNG: a
 * second check here would be dead code or a second policy in a second place.
 * (Nothing calls this yet -- see mldsa.h's header on scope.)
 */

void mldsa_keygen(const mldsa_params *p, uint8_t *pk, uint8_t *sk)
{
    uint8_t xi[32];
    kernel_random_bytes(xi, 32);
    mldsa_keygen_internal(p, xi, pk, sk);
    for (volatile uint8_t *v = xi; v < xi + 32; v++) *v = 0;
}

/* HEDGED signing (FIPS 204 Algorithm 2, the `rnd <- {0,1}^256` branch, not the
 * `rnd <- 0^256` deterministic one -- see mldsa_sign() in mldsa.c for that).
 * Hedged and deterministic signatures over the same (sk, message) are
 * completely different byte strings; that is intentional, not a bug -- see
 * mldsa.h. */
int mldsa_sign_hedged(const mldsa_params *p, const uint8_t *sk,
                       const uint8_t *msg, size_t mlen,
                       const uint8_t *ctx, size_t ctxlen, uint8_t *sig)
{
    uint8_t rnd[32];
    kernel_random_bytes(rnd, 32);
    int rc = mldsa_sign(p, sk, msg, mlen, ctx, ctxlen, rnd, sig);
    for (volatile uint8_t *v = rnd; v < rnd + 32; v++) *v = 0;
    return rc;
}
