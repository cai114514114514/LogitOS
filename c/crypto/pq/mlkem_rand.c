#include "mlkem.h"
#include "rng.h"

/* The two randomised entry points, split into their own translation unit for
 * one reason: mlkem.c must link into a HOST test that has no kernel RNG, and
 * the gate that matters (byte-for-byte agreement with openssl) can only be run
 * on the derandomised forms. Keeping the RNG dependency out of mlkem.c means
 * the tested object and the shipped object are the same code, rather than the
 * same code minus an #ifdef -- which is the arrangement where a bug hides in
 * the half the test cannot reach.
 *
 * Not gated on rng_strong(). tls.c already refuses to start a handshake at all
 * without a strong RNG (see the -cpu max note in CLAUDE.md), so a second check
 * here would be either dead code or a different policy in a second place.
 */

void mlkem768_keygen(uint8_t ek[MLKEM768_EK], uint8_t dk[MLKEM768_DK])
{
    uint8_t d[32], z[32];
    kernel_random_bytes(d, 32);
    kernel_random_bytes(z, 32);
    mlkem768_keygen_derand(d, z, ek, dk);
    /* d and z reconstruct the entire private key, so they are wiped here and
     * not merely left to go out of scope. Volatile write: -O2 would otherwise
     * drop a dead store to an automatic. */
    for (volatile uint8_t *p = d; p < d + 32; p++) *p = 0;
    for (volatile uint8_t *p = z; p < z + 32; p++) *p = 0;
}

int mlkem768_encaps(const uint8_t ek[MLKEM768_EK],
                    uint8_t ct[MLKEM768_CT], uint8_t ss[MLKEM768_SS])
{
    uint8_t m[32];
    kernel_random_bytes(m, 32);
    int rc = mlkem768_encaps_derand(ek, m, ct, ss);
    for (volatile uint8_t *p = m; p < m + 32; p++) *p = 0;
    return rc;
}
