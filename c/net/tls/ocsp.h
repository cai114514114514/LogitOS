#ifndef LOGIT_OCSP_H
#define LOGIT_OCSP_H

#include <stdint.h>
#include "x509.h"

/* OCSP stapling (RFC 6960 + RFC 6961 / RFC 8446 4.4.2.1).
 *
 * THE HOLE THIS CLOSES. Until now this stack verified that a certificate chains
 * to a trusted root and that its dates are current, and then trusted it. A
 * certificate whose key had been stolen and which the CA had REVOKED an hour
 * ago verified exactly as well as a good one. That is the one live security
 * defect in an otherwise strict verifier, because revocation is the only
 * mechanism that acts faster than a certificate's expiry.
 *
 * WHAT IS AND IS NOT DONE HERE:
 *   - We ASK for a stapled response (status_request in the ClientHello) and
 *     VERIFY the one the server hands back.
 *   - We do NOT make an online OCSP request. Deliberately: an online check is a
 *     plaintext HTTP round trip to a third party on every connection, which
 *     leaks the browsing history it is protecting, adds a hard dependency on a
 *     responder being up, and -- because clients soft-fail when it is not --
 *     buys an attacker who can drop packets exactly nothing. Stapling has none
 *     of those properties: the response is signed, so a hostile server cannot
 *     forge one, and it costs no extra round trip.
 *
 * THE POLICY, said plainly, because "we check revocation now" would be a
 * misleading summary:
 *   - No staple                 -> the handshake proceeds. Most of the web does
 *                                  not staple. Hard-failing here would break
 *                                  more sites than it protects, and a server
 *                                  that wants to hide a revocation can simply
 *                                  not staple -- so hard-fail buys nothing
 *                                  against the attacker it is aimed at either.
 *   - Staple says REVOKED       -> the handshake FAILS. This is the case that
 *                                  did not exist before.
 *   - Staple present but bad    -> the handshake FAILS: unparseable, wrong
 *                                  CertID, expired, unsigned, signed by the
 *                                  wrong key, or status "unknown". A response
 *                                  we cannot check is not a response; treating
 *                                  it as "no staple" would let an attacker
 *                                  downgrade a revoked answer to a missing one
 *                                  by corrupting one byte.
 *
 * That asymmetry -- absent is fine, present-and-wrong is fatal -- is the whole
 * design, and it is what makes stapling worth having without an online fetch.
 */

#define OCSP_OK           0
#define OCSP_E_PARSE     -1   /* DER did not decode, or a field was missing */
#define OCSP_E_STATUS    -2   /* OCSPResponse.responseStatus != successful */
#define OCSP_E_TYPE      -3   /* responseType is not id-pkix-ocsp-basic */
#define OCSP_E_CERTID    -4   /* no SingleResponse is about this certificate */
#define OCSP_E_REVOKED   -5   /* the certificate is REVOKED */
#define OCSP_E_UNKNOWN   -6   /* the responder says it does not know this cert */
#define OCSP_E_STALE     -7   /* thisUpdate in the future, or nextUpdate passed */
#define OCSP_E_SIG       -8   /* signature over ResponseData did not verify */
#define OCSP_E_SIGNER    -9   /* nobody entitled to sign for this issuer did */

/* Verify a stapled DER OCSPResponse about `leaf`, issued under `issuer`.
 * `now` is unix seconds (the same clock x509_verify_chain uses).
 *
 * Returns OCSP_OK only when a SingleResponse matching the leaf's CertID says
 * `good`, is current, and is signed either by the issuer itself or by a
 * delegated responder certificate carried in the response that the issuer
 * signed AND that carries id-kp-OCSPSigning.
 *
 * `skew` is the tolerance in seconds applied to thisUpdate/nextUpdate. It is a
 * parameter and not a constant because this machine's clock is a CMOS RTC read
 * once at boot; 300 is what the caller passes. */
int ocsp_check(const uint8_t *der, int len,
               const struct cert *leaf, const struct cert *issuer,
               int64_t now, int64_t skew);

/* A name for the result, for the one kprintf that reports it. */
const char *ocsp_strerror(int rc);

/* Test hook: where the SIGNED region (tbsResponseData) is inside the DER.
 * See the long comment above the definition for why the tamper sweep needs
 * this rather than simply flipping every byte in the file. */
int ocsp_signed_span(const uint8_t *der, int len, int *off, int *span);

#endif /* LOGIT_OCSP_H */
