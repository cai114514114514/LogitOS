#ifndef LOGIT_TLS_PROF_H
#define LOGIT_TLS_PROF_H

#include <stdint.h>

/* ============================================================================
 * TLS phase timing -- the spans, and nothing else.
 *
 * WHY THIS FILE EXISTS AT ALL
 * ---------------------------
 * `browser_rt.c` states, as settled fact, that a TLS handshake on an emulated
 * CPU "costs SECONDS", and every scheduling decision in the fetch layer is
 * built on that sentence. Nobody had ever measured which part of the handshake
 * those seconds were in. A handshake is a network round trip, an ECDH, a DER
 * parse, between one and five signature verifications, a key schedule and some
 * bulk AEAD -- and those differ from each other by three orders of magnitude,
 * so "TLS is slow" names no code. This header is what turns it into a number
 * per phase.
 *
 * IT IS NOT A SECOND PROFILER. The instrument is `kprof` (c/kernel/core/kprof.c):
 * KPROF_BEGIN/END, read back with `cat /dev/kprof`. All this header does is
 * decide whether the TLS sources can see it, which they cannot always:
 *
 *   - In the KERNEL (-ffreestanding, so __STDC_HOSTED__ == 0) kprof.o is linked
 *     in and the spans are real.
 *   - In a HOST unit test -- tests/unit/run-tls-interop.sh and x509_fuzz build
 *     the same tls.c/tls12.c/x509.c natively, and their include path already
 *     contains c/kernel/core -- kprof.h is *findable* but kprof.o is not
 *     linked. Including it there would break the link of the two tests that
 *     guard this code's correctness, to add timing to a process whose timing
 *     means nothing. So the spans compile to nothing on a hosted build.
 *
 * The __has_include is the second half of the same problem: kprof landed in the
 * working tree before it landed in a commit, and a commit of THIS file that
 * assumed kprof.h exists would not build from a clean clone of itself. With
 * both guards the file compiles identically with or without the profiler, which
 * is the property that makes it safe to commit into a contended tree.
 *
 * WHAT THE SPANS ARE NAMED, AND WHY THAT SET
 * ------------------------------------------
 * One span per phase that could plausibly be the seconds, chosen so the totals
 * sum to something close to the handshake and each one names code you could go
 * and change:
 *
 *   tls_handshake      tls_start() .. established or failed. The denominator.
 *   tls_netwait        net_poll()+net_idle() inside the blocking connect: the
 *                      round trips, i.e. the part no crypto change can touch.
 *   tls_kx_keygen      our ephemeral share (x25519_base / ecdh_keygen)
 *   tls_kx_shared      the ECDH proper (x25519 / ecdh_shared)
 *   tls_sched13        the TLS 1.3 HKDF ladder
 *   tls12_prf          TLS 1.2's P_hash key schedule
 *   tls_cert_parse     x509_parse over the whole flight
 *   tls_chain_verify   x509_verify_chain: link signatures + the anchor search
 *   tls_chain_sig        one child-signed-by-issuer check (nested)
 *   tls_root_search      the trust-store scan (nested) -- see x509.c
 *   tls_certverify     1.3 CertificateVerify / 1.2 ServerKeyExchange signature
 *   tls_hs_aead        record open/seal during the handshake
 *   tls_app_aead       record open/seal for application data. Separate from the
 *                      handshake's because this is the one bulk-crypto number,
 *                      and it is what the AES-NI-versus-C question is about.
 *
 * Nested spans are fine and intended: kprof keeps the start timestamp in a
 * local, so tls_root_search inside tls_chain_verify inside tls_handshake all
 * accumulate correctly and the report reads as a decomposition.
 * ========================================================================== */

#if !defined(__STDC_HOSTED__) || __STDC_HOSTED__ == 0
#  if defined(__has_include)
#    if __has_include("kprof.h")
#      include "kprof.h"
#      define TLS_PROF_KPROF 1
#    endif
#  endif
#endif

#ifdef TLS_PROF_KPROF

#define TLSPROF_BEGIN(name)  KPROF_BEGIN(name)
#define TLSPROF_END(name)    KPROF_END(name)

/* The cross-function form, for a span that starts in tls_start() and ends in
 * whichever of four places the handshake finishes at. The slot is a file-static
 * rather than the macro's function-static because begin and end are not in the
 * same function; the start timestamp lives in the session, which is where the
 * rest of the handshake's state already is. */
typedef struct { int slot; uint64_t t0; } tls_prof_span;

static inline void tlsprof_open(tls_prof_span *sp, int *slot, const char *name)
{ sp->slot = *slot; sp->t0 = kprof_span_begin(slot, name); sp->slot = *slot; }

static inline void tlsprof_close(tls_prof_span *sp)
{ kprof_span_end(sp->slot, sp->t0); sp->t0 = 0; }

#else  /* no profiler in this build: every span is nothing at all */

typedef struct { int slot; uint64_t t0; } tls_prof_span;

#define TLSPROF_BEGIN(name)  ((void)0)
#define TLSPROF_END(name)    ((void)0)

static inline void tlsprof_open(tls_prof_span *sp, int *slot, const char *name)
{ (void)slot; (void)name; sp->slot = -1; sp->t0 = 0; }

static inline void tlsprof_close(tls_prof_span *sp)
{ (void)sp; }

#endif

#endif /* LOGIT_TLS_PROF_H */
