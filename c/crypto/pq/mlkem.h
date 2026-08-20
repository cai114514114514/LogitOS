#ifndef LOGIT_PQ_MLKEM_H
#define LOGIT_PQ_MLKEM_H

#include <stdint.h>

/* ML-KEM-768 (FIPS 203), the parameter set browsers actually negotiate inside
 * X25519MLKEM768. Only the 768 set is here: 512 and 1024 are the same code with
 * different k/du/dv, but nothing in this tree offers them, and a parameter set
 * that is compiled and never negotiated is a second code path the gate does not
 * cover. Adding one later is three constants and a struct, not a rewrite.
 *
 * Sizes, derived (build/tlsx/gen_compress.py) rather than remembered:
 *   ek = 384k + 32          = 1184     k = 3
 *   dk = 768k + 96          = 2400
 *   ct = 32(du*k + dv)      = 1088     du = 10, dv = 4
 *   ss                      = 32
 */
#define MLKEM768_EK      1184
#define MLKEM768_DK      2400
#define MLKEM768_CT      1088
#define MLKEM768_SS        32

/* KeyGen from the two 32-byte seeds d and z (FIPS 203 Alg.16,
 * ML-KEM.KeyGen_internal). The seeds are a parameter rather than drawn inside
 * because that is what makes the function testable against a KAT at all -- a
 * KeyGen that calls the RNG itself can only ever be compared with itself.
 * tls.c draws them from rand_bytes(); see mlkem_keygen(). */
void mlkem768_keygen_derand(const uint8_t d[32], const uint8_t z[32],
                            uint8_t ek[MLKEM768_EK], uint8_t dk[MLKEM768_DK]);

/* Encaps from the 32-byte message m (FIPS 203 Alg.17, Encaps_internal).
 * Returns 0, or -1 if ek fails the modulus check of FIPS 203 7.2 -- an ek whose
 * coefficients are not all < q is not a valid encapsulation key, and accepting
 * one would make ByteEncode(ByteDecode(ek)) != ek, i.e. two distinct encodings
 * of one key. Rejecting is the spec's requirement, not a hardening choice. */
int  mlkem768_encaps_derand(const uint8_t ek[MLKEM768_EK], const uint8_t m[32],
                            uint8_t ct[MLKEM768_CT], uint8_t ss[MLKEM768_SS]);

/* Decaps (FIPS 203 Alg.18, Decaps_internal).
 *
 * THIS FUNCTION HAS NO FAILURE RETURN, AND THAT IS THE WHOLE POINT. ML-KEM uses
 * IMPLICIT REJECTION: a ciphertext that does not re-encrypt to itself yields
 * the pseudorandom secret J(z || ct) instead of an error. A version that
 * returned an error code here would be a decryption oracle -- the attacker
 * learns whether their mauled ciphertext decrypted correctly, which is exactly
 * the distinguisher the Fujisaki-Okamoto transform exists to deny them. The
 * caller cannot tell success from failure and must not try; a wrong ct simply
 * produces a shared secret the peer does not have, and the handshake fails
 * later at the Finished MAC, telling the attacker nothing.
 *
 * Constant time in ct and dk: the comparison and the select are branch-free. */
void mlkem768_decaps(const uint8_t dk[MLKEM768_DK], const uint8_t ct[MLKEM768_CT],
                     uint8_t ss[MLKEM768_SS]);

/* Convenience wrappers that draw their own randomness from the kernel RNG.
 * Kernel/TLS builds only -- the host test drives the _derand forms so that
 * every case is reproducible and comparable with openssl. */
void mlkem768_keygen(uint8_t ek[MLKEM768_EK], uint8_t dk[MLKEM768_DK]);
int  mlkem768_encaps(const uint8_t ek[MLKEM768_EK],
                     uint8_t ct[MLKEM768_CT], uint8_t ss[MLKEM768_SS]);

#endif
