#ifndef LOGIT_PKGSIG_H
#define LOGIT_PKGSIG_H

#include <stdint.h>

/* A signed container, and the trust root under it.
 *
 * WHAT THIS IS AND IS NOT. It is not a package manager: no dependency solving,
 * no install scripts, no repository index, no versions. It is the ONE thing a
 * package manager would have to stand on and that this machine did not have --
 * the ability to check that a file it downloaded is the file someone with a
 * known key meant to send. Everything above it (what to fetch, where to put it,
 * what to do if it is already there) is policy and can be built later without
 * touching this; nothing above it is safe to build first.
 *
 * THE FORMAT (.lpk). All integers little-endian. 224-byte fixed header, then
 * the payload:
 *
 *     0   8   magic       "LOGITPKG"
 *     8   4   version     1
 *    12   4   flags       0
 *    16   8   payload_len
 *    24   4   name_len    <= 64
 *    28   4   reserved    0
 *    32  64   name        the payload's intended file name, NUL-padded
 *    96  32   payload_sha256
 *   128  32   signer      Ed25519 public key
 *   160  64   sig         Ed25519 over SHA-256(DOMAIN || header[0..159])
 *   224  ..   payload
 *
 * The signature covers the header up to (not including) itself, and the header
 * carries the payload's length and digest -- so signing 160 bytes authenticates
 * the whole package, and a signer never has to stream a 200 MiB payload through
 * the curve. The cost is stated rather than hidden: the binding from signature
 * to payload is SHA-256's collision resistance, so a signer who can be made to
 * sign an attacker-chosen digest is a signer who has signed everything that
 * collides with it. That is the standard sign-the-manifest trade and it is why
 * the digest is SHA-256 and not something shorter.
 *
 * DOMAIN is "LOGITOS-lpk-v1" and it is not decoration: without it, an Ed25519
 * signature made by the same key over any other 160-byte structure -- a future
 * update manifest, a TLS client-certificate challenge -- could be replayed as a
 * package signature. The domain string is inside the hash, so a signature is
 * bound to the thing it was made for.
 *
 * WHERE THE TRUST ROOT LIVES -- and this is the question that decides whether
 * any of the above means anything. It is COMPILED IN: tools/pkgroots/<name>.pub ->
 * tools/genpkgroots.py -> c/crypto/trust/pkgroots.inc, which pkgsig.c
 * #includes, exactly like the 130-root CA bundle. It is NOT read from a file.
 *
 *   Because a file on this machine's disk is not a trust root. LogitFS has no
 *   enforced permissions that survive a reboot -- CLAUDE.md says outright that
 *   vfs_meta's modes and owners live in RAM and are lost -- so /etc/trust/keys
 *   would be writable by exactly the unprivileged code the signature is
 *   supposed to constrain. A verifier whose root can be replaced by its
 *   attacker is a verifier that always says yes, slowly.
 *
 *   The consequence, said plainly: adding a trusted signer means rebuilding the
 *   ISO. That is a real limitation and it is the correct one for today. When
 *   this machine grows enforced ownership on a mounted filesystem, a
 *   root-owned /etc/trust becomes possible and the change is local to
 *   pkg_root_count/pkg_root_key below.
 *
 *   That last sentence is now ENFORCED rather than asserted, and it was not
 *   before: lpk_verify used to walk the compiled-in array itself, four lines
 *   under the accessors, while /bin/pkgverify --roots used the accessors. With
 *   a single built-in root the two agree and nothing is visibly wrong; change
 *   the accessors as this paragraph invites and the machine's answer to "who
 *   signed this?" and its answer to "who do you trust?" start coming from
 *   different tables, with the UI reporting the one that did not decide.
 *   pkgsig.c now reaches the trust set only through the three functions below,
 *   and `#pragma GCC poison` on the array makes a relapse a compile error.
 *
 * The Makefile gives pkgsig.o an explicit dependency on pkgroots.inc for the
 * same reason roots.o has one on roots_bundle.inc: regenerating the include
 * without it silently keeps the old keys in the kernel. */

#define LPK_HDR_LEN   224
#define LPK_SIGNED_LEN 160          /* header bytes the signature covers */
#define LPK_NAME_MAX   64
#define LPK_VERSION    1

/* What a caller gets out of a verified package. Pointers reference the input
 * buffer, which must outlive this struct. */
struct lpk {
    const uint8_t *payload; uint64_t payload_len;
    char name[LPK_NAME_MAX + 1];    /* NUL-terminated */
    uint8_t signer[32];
    int root_index;                 /* which built-in key signed it, or -1 */
};

/* Verify `buf[0..len)` as a package. Returns 0 and fills `out` when the
 * container is well formed, the payload matches its digest, AND the signature
 * verifies under a key in the built-in root set. Otherwise one of the LPK_E_*
 * below and `out` is untouched.
 *
 * `trust_any` is for the tools and the unit test ONLY: with it set, a valid
 * self-signature by any key is accepted and root_index comes back -1. It is a
 * separate argument rather than a flag inside the file so that a package can
 * never talk the verifier into trusting itself. */
int lpk_verify(const uint8_t *buf, uint64_t len, struct lpk *out, int trust_any);

/* Build a package header for `payload` signed by `seed` (the 32-byte Ed25519
 * private key). Writes LPK_HDR_LEN bytes to `hdr`. The caller concatenates
 * hdr + payload. Returns 0, or -1 on a name longer than LPK_NAME_MAX.
 *
 * Present in the kernel-linkable file (not only in tools/lpk.py) because a
 * machine that can verify but not sign cannot produce anything another machine
 * would accept, and because the unit test's round trip has to exercise the same
 * bytes the verifier reads. */
int lpk_sign(uint8_t *hdr, const char *name,
             const uint8_t *payload, uint64_t payload_len,
             const uint8_t seed[32]);

#define LPK_OK          0
#define LPK_E_SHORT    -1   /* fewer bytes than a header, or a truncated payload */
#define LPK_E_MAGIC    -2
#define LPK_E_VERSION  -3
#define LPK_E_FIELD    -4   /* a length or reserved field is out of range */
#define LPK_E_DIGEST   -5   /* payload does not match payload_sha256 */
#define LPK_E_SIG      -6   /* Ed25519 verification failed */
#define LPK_E_UNTRUSTED -7  /* signature is good, signer is not a built-in root */

/* The built-in signer set, for diagnostics and for a UI that wants to say who
 * is trusted. `pkg_root_key` returns a pointer to 32 bytes, or NULL. */
int          pkg_root_count(void);
const uint8_t *pkg_root_key(int i);
const char  *pkg_root_name(int i);

#endif /* LOGIT_PKGSIG_H */
