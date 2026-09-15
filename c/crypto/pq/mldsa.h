#ifndef LOGIT_PQ_MLDSA_H
#define LOGIT_PQ_MLDSA_H

#include <stdint.h>
#include <stddef.h>

/* ML-DSA (FIPS 204), all three parameter sets: ML-DSA-44, ML-DSA-65, ML-DSA-87.
 * This is the standardised PQ SIGNATURE counterpart to the ML-KEM-768 already
 * in c/crypto/pq/mlkem.c -- ML-KEM is the key-exchange half of the NIST PQC
 * suite, ML-DSA is the signature half, and until this file the tree had one
 * and not the other. It is A LIBRARY PRIMITIVE ONLY: nothing in c/crypto/trust,
 * c/net/tls, c/kernel/exec/load/aex.c or the login path calls it, and it must stay
 * that way -- wiring a new signature scheme into a trust path is a decision
 * with its own review, not a side effect of shipping the primitive. See
 * CLAUDE.md's crypto-extension workflow note if that decision ever gets made.
 *
 * THREE PARAMETER SETS, NOT ONE HARDCODED TO LOOK LIKE THREE. Every size and
 * every sampling width (eta, gamma1, gamma2's bit-packing width) differs across
 * 44/65/87, and a single hardcoded set that happens to pass one KAT while
 * claiming to support all three is worse than shipping one and refusing the
 * other two by name -- so the engine below is parameterised at RUNTIME by
 * `mldsa_params` and every one of 44/65/87 is a real, independently gated,
 * instance of the same code, not a macro-duplicated copy (which would be a
 * second place for the same bug to be fixed once and missed twice).
 *
 * DERANDOMISED CORE, RANDOMISED WRAPPER -- same split as mlkem.h/mlkem_rand.c,
 * for the same reason: this file must link into a HOST test with no kernel RNG,
 * and the KAT/differential gates can only run against the derandomised entry
 * points. mldsa_rand.c (not this file) is where kernel_random_bytes appears.
 *
 * SIZES (FIPS 204 Table 2 / Section 4), pk = 32 + 32*k*10, sk = 2*32 + 64 +
 * 32*l*bits_s + 32*k*bits_s + 416*k, sig = c_tilde_bytes + 32*l*bits_z + omega + k:
 *
 *              k  l  eta  tau  gamma1   gamma2      omega  c~   pk    sk    sig
 *   ML-DSA-44  4  4  2    39   2^17     (q-1)/88     80    32  1312  2560  2420
 *   ML-DSA-65  6  5  4    49   2^19     (q-1)/32     55    48  1952  4032  3309
 *   ML-DSA-87  8  7  2    60   2^19     (q-1)/32     75    64  2592  4896  4627
 */

#define MLDSA44_PK  1312
#define MLDSA44_SK  2560
#define MLDSA44_SIG 2420

#define MLDSA65_PK  1952
#define MLDSA65_SK  4032
#define MLDSA65_SIG 3309

#define MLDSA87_PK  2592
#define MLDSA87_SK  4896
#define MLDSA87_SIG 4627

/* Bound on k and l across all three sets, for callers sizing a scratch buffer
 * without depending on which parameter set they picked at runtime. */
#define MLDSA_K_MAX 8
#define MLDSA_L_MAX 7
#define MLDSA_N     256

typedef struct {
    const char *name;
    int k, l;              /* matrix A is k x l */
    int eta;                /* secret-vector coefficient bound: 2 or 4 */
    int tau;                 /* number of +-1's in the challenge polynomial c */
    int beta;                 /* tau * eta -- the rejection-loop bound offset */
    int gamma1;                /* 2^17 or 2^19 -- mask-vector coefficient bound */
    int gamma2;                  /* (q-1)/88 or (q-1)/32 -- low-order rounding range */
    int omega;                    /* max number of 1's in the hint vector */
    int c_tilde_bytes;              /* challenge-hash length: 32, 48 or 64 */
    int pk_bytes, sk_bytes, sig_bytes;
} mldsa_params;

extern const mldsa_params mldsa44_params;
extern const mldsa_params mldsa65_params;
extern const mldsa_params mldsa87_params;

/* ML-DSA.KeyGen_internal, FIPS 204 Algorithm 6. xi is the 32-byte seed; pk/sk
 * buffers must be p->pk_bytes / p->sk_bytes. Deterministic in xi -- this is
 * what makes it KAT-testable; mldsa_keygen() in mldsa_rand.c draws xi from the
 * RNG. */
void mldsa_keygen_internal(const mldsa_params *p, const uint8_t xi[32],
                            uint8_t *pk, uint8_t *sk);

/* ML-DSA.Sign_internal, FIPS 204 Algorithm 7. `mprime` is the ALREADY-FORMATTED
 * message representative M' = IntegerToBytes(0,1) || IntegerToBytes(|ctx|,1) ||
 * ctx || M (see mldsa_sign() below, which builds this). mu = H(tr || M', 64) is
 * computed inside from the unpacked sk's tr. HashML-DSA (the pre-hash variant,
 * which signs mu computed from an already-hashed M) and the IETF-draft
 * external-mu entry point are NOT implemented -- named here rather than left
 * silent: ACVP's sigGen/sigVer groups that carry a `hashAlg` field exercise
 * that variant and this code does not claim to pass them. Pure ML-DSA
 * (`signatureInterface: external`, no `hashAlg`) is what this file gates.
 *
 * rnd is the caller-supplied 32-byte randomiser: all-zero for the DETERMINISTIC
 * variant (FIPS 204 Algorithm 2 line 2's "rnd <- 0^256" branch -- what this
 * file's KAT gate uses, because a deterministic signature is the only kind a
 * known-answer test can pin) or drawn from the RNG for the HEDGED variant
 * (mldsa_rand.c). The two give completely different signatures for the same
 * (sk, message) -- that is intentional, not a bug to reconcile.
 *
 * Returns 0 on success. The rejection loop inside always terminates with
 * overwhelming probability (FIPS 204 3.6.3: expected ~4-7 attempts depending on
 * parameter set); MLDSA_MAX_SIGN_ATTEMPTS below is a hard backstop so a defect
 * that made every attempt reject cannot spin the caller forever, and returns -1
 * if it is ever hit -- which is a sign the ENGINE is broken, not the message. */
#define MLDSA_MAX_SIGN_ATTEMPTS 1000
int mldsa_sign_internal(const mldsa_params *p, const uint8_t *sk,
                         const uint8_t *mprime, size_t mplen,
                         const uint8_t rnd[32], uint8_t *sig);

/* ML-DSA.Verify_internal, FIPS 204 Algorithm 8. Returns 1 if the signature
 * verifies, 0 otherwise -- including every malformed-encoding case (hint bytes
 * not monotonically increasing, offsets exceeding omega, z out of range): FIPS
 * 204 treats a signature that fails to decode as a signature that fails to
 * verify, not a distinct error, and this API does not distinguish them either
 * -- see the note at BitUnpack in mldsa.c on why that distinction would be a
 * second, unaudited, place for "is this signature acceptable" to be decided. */
int mldsa_verify_internal(const mldsa_params *p, const uint8_t *pk,
                           const uint8_t *mprime, size_t mplen,
                           const uint8_t *sig);

/* Pure ML-DSA.Sign / ML-DSA.Verify, FIPS 204 Algorithm 2 / Algorithm 3: builds
 * M' = 0x00 || len(ctx) || ctx || M (ctxlen <= 255, enforced -- returns -1
 * otherwise) and calls the _internal forms above. This is the entry point the
 * ACVP sigGen/sigVer vectors exercise (signatureInterface "external", no
 * hashAlg). M' is materialised into a bounded stack buffer
 * (MLDSA_MPRIME_MAX in mldsa.c, currently 16 KiB) rather than streamed --
 * returns -1 if 2 + ctxlen + mlen exceeds that, which every vector this file
 * ships is well inside. */
int mldsa_sign(const mldsa_params *p, const uint8_t *sk,
               const uint8_t *msg, size_t mlen,
               const uint8_t *ctx, size_t ctxlen,
               const uint8_t rnd[32], uint8_t *sig);

int mldsa_verify(const mldsa_params *p, const uint8_t *pk,
                  const uint8_t *msg, size_t mlen,
                  const uint8_t *ctx, size_t ctxlen,
                  const uint8_t *sig);

/* Randomised entry points -- mldsa_rand.c, draws from the kernel RNG. Not
 * declared where the derandomised forms are documented above because these
 * two are the ones with a kernel dependency; see mldsa_rand.c's header. */
void mldsa_keygen(const mldsa_params *p, uint8_t *pk, uint8_t *sk);
int  mldsa_sign_hedged(const mldsa_params *p, const uint8_t *sk,
                        const uint8_t *msg, size_t mlen,
                        const uint8_t *ctx, size_t ctxlen, uint8_t *sig);

#endif
