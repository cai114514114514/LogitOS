#ifndef LOGIT_AEXSIG_H
#define LOGIT_AEXSIG_H

#include <stdint.h>

/* Ed25519 signing for .aex containers.
 *
 * SAME SCHEME, SAME KEYS, DIFFERENT DOMAIN. This reuses c/crypto/trust/
 * pkgsig.c's construction (sign a small manifest carrying a length and a
 * SHA-256 digest, never the payload itself) and its SAME compiled-in trust
 * roots -- pkg_root_count()/pkg_root_key()/pkg_root_name(), the exact
 * accessors lpk_verify uses, so ".lpk answers who is trusted" and ".aex
 * answers who is trusted" can never read two different tables. See
 * pkgsig.h's long comment for WHY the roots are compiled in rather than read
 * from a file (LogitFS has no enforced, reboot-surviving ownership); that
 * argument is unchanged here.
 *
 * WHAT VERIFYING ONE OF THESE DOES NOT MEAN. c/kernel/exec/aex.c decided, and
 * argues at the TLV case for AEX_T_SIG and again above where it calls
 * aex_sig_verify(), to LOG the verdict and load the program ANYWAY --
 * unsigned, invalid, untrusted or verified all execute. That is a deliberate
 * product decision (no key-distribution story, no revocation, no policy for
 * "refuse" yet), not a limitation of the four bytes read here. A caller that
 * branches on AEX_SIG_OK to allow or deny something IS treating this as a
 * security boundary and SECURITY.md's frame says not to: "Do not treat its
 * ... process isolation ... as a security boundary for hostile workloads."
 * Read aex.c before writing that caller.
 *
 * WHY A SEPARATE DOMAIN, AND WHY IT MUST NEVER BE "LOGITOS-lpk-v1". Both
 * formats are signed by the SAME Ed25519 keys (the same compiled-in roots
 * trust both an .lpk and an .aex signature), Ed25519 signs whatever 32-byte
 * hash it is handed, and here -- exactly as in pkgsig.c -- that hash is
 * SHA-256(DOMAIN || small manifest). If the two formats shared one domain
 * string, a valid .lpk signature over a package whose payload happens to be
 * an ELF image would ALSO verify as a valid .aex signature over that same
 * ELF, because every other byte the two hash (a length, a SHA-256 digest) is
 * the identical shape in both formats. The domain string is the ONLY thing
 * that tells them apart, which is why getting this one line wrong produces a
 * scheme that passes every test written against it in isolation and is
 * forgeable in practice by anyone who can get one .lpk signed by a root.
 * tests/unit/aexsig_test.c signs a real .lpk header and feeds its raw
 * signer+signature bytes to aex_sig_verify() to prove this does not verify.
 *
 * THE MANIFEST: 8 bytes of elf_size (LE) + 32 bytes of SHA-256(ELF image).
 * Mirrors lpk_sign's payload_len + payload_sha256 -- a signer never streams
 * a 64 MiB program through the curve, it signs 40 bytes, and the binding
 * from signature to program is SHA-256's collision resistance, exactly the
 * trade pkgsig.h argues for its own manifest. */

#define AEX_SIG_DOMAIN   "LOGITOS-aex-v1"
#define AEX_SIG_LEN      96      /* signer pubkey[32] || Ed25519 signature[64] --
                                  * the AEX_T_SIG TLV record's value, whole    */

/* h = SHA-256(AEX_SIG_DOMAIN || elf_size (8 bytes LE) || elf_sha256[32]).
 * Exposed so a host signer and the kernel verifier derive the identical 40
 * bytes from the identical two facts about the image -- its length and its
 * digest -- rather than each re-deriving this layout from a comment. */
void aex_sig_hash(uint8_t h[32], uint64_t elf_size, const uint8_t elf_sha256[32]);

/* Build the 96-byte TLV value for `seed`'s key: pub || Ed25519_sign(h). */
void aex_sig_sign(uint8_t out[AEX_SIG_LEN], uint64_t elf_size,
                   const uint8_t elf_sha256[32], const uint8_t seed[32]);

/* Verdicts, worst to best on purpose: `if (v >= AEX_SIG_OK)` is the strict
 * check, `if (v > AEX_SIG_ABSENT)` is "at least a mathematically valid
 * signature, from whoever". NONE of the four is a decision to refuse
 * anything -- see the file header. */
#define AEX_SIG_ABSENT     0   /* no AEX_T_SIG record was present             */
#define AEX_SIG_INVALID    1   /* a record was present and did NOT verify --
                                * wrong key material, wrong domain, a payload
                                * or length that does not match what was signed */
#define AEX_SIG_UNTRUSTED  2   /* the Ed25519 signature verifies, but the
                                * signer is not one of the compiled-in roots  */
#define AEX_SIG_OK         3   /* verifies, and the signer IS a compiled-in
                                * root -- root_index_out names which one      */

/* Checks the 96-byte record against (elf_size, elf_sha256) and, only if the
 * Ed25519 signature itself is valid, walks pkg_root_count()/pkg_root_key().
 * Returns one of the four verdicts above; `root_index_out` (may be NULL) is
 * set to a valid pkg_root_name() index only for AEX_SIG_OK, else to -1. */
int aex_sig_verify(const uint8_t sigrec[AEX_SIG_LEN], uint64_t elf_size,
                    const uint8_t elf_sha256[32], int *root_index_out);

#endif /* LOGIT_AEXSIG_H */
