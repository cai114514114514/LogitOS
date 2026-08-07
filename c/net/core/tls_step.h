#ifndef LOGIT_TLS_STEP_H
#define LOGIT_TLS_STEP_H

#include <stdint.h>
#include "tls.h"
#include "logit_abi.h"      /* SOCK_F_ALPN_* */

/* WHAT THE SOCKET LAYER ASSUMES ABOUT TLS, AND THE ONE ADAPTER IT NEEDS.
 *
 * c/net/tls/tls.c is written by another author. The stepped entry points it now
 * exposes -- tls_start / tls_step / tls_send / tls_recv / tls_alpn / tls_close --
 * are declared in tls.h and are what sock.c drives. This header exists to write
 * down the four properties sock.c RELIES on, because they are not things a
 * signature can state, and to convert the one place where the two sides describe
 * the same thing differently.
 *
 *  1. tls_step() NEVER BLOCKS AND NEVER CALLS net_poll(). It is driven from
 *     inside net_poll(); a step that pumped net_poll would re-enter the RX path
 *     with connections half-advanced. tls.h promises this ("nothing here ever
 *     sleeps, pumps the network, or spins").
 *
 *  2. tls_recv() DOES NOT BLOCK EITHER. sock_recv() calls it straight from the
 *     int 0x80 gate, where IF is 0 and no tick will ever arrive, so anything
 *     that waited there would hang the caller forever.
 *
 *  3. WRITES MAY BE SHORT. tls_send() returning less than asked (or 0) is normal
 *     -- the TCP layer has a single outstanding segment -- and sock_pump()
 *     retries the tail on the next poll. Only -1 means dead.
 *
 *  4. TLS SITS ON THE TCP CONNECTION, NOT ON THE SOCKET. tls_start() takes the
 *     TCP id, so the record layer keeps talking to tcp_recv/tcp_send_nb
 *     directly. Layering it on sock_send/sock_recv would have been circular:
 *     sock drives the handshake, so sock cannot also be underneath it.
 *
 * ONE KNOWN COUPLING, LEFT ALONE ON PURPOSE: tls.h caps concurrent sessions at
 * TLS_MAX_SESSIONS 8 and explains it as "matches TCP's NCONN". NCONN is 32 now
 * (see tcp.c), so TLS is the tighter limit -- eight simultaneous handshakes
 * rather than sixteen sockets' worth. That is still eight times what the
 * blocking path allowed, and raising it is the TLS author's call, not ours. */

/* The three stepped entry points, RE-DECLARED WEAK.
 *
 * tls.h declares them strongly, but tls.c is being written in parallel with this
 * file and there are windows in which the header has them and the object does
 * not. A weak reference turns that from "the kernel does not link" into "TLS
 * sockets report SOCK_E_TLS", which is the difference between this layer being
 * blocked on someone else's edit and not. sock.c checks the pointers before
 * calling. Once the TLS side is settled these three lines can go and the
 * declarations in tls.h become the only ones. */
int tls_start(int tcp_id, const char *host, const char *alpn, int64_t now) __attribute__((weak));
int tls_step(int id) __attribute__((weak));
int tls_alpn(int id, char *out, int max) __attribute__((weak));

/* Decrypted bytes buffered and ready, without consuming them. OPTIONAL -- it
 * does not exist yet, so the declaration is weak and resolves to 0. Where it is
 * absent, sock_poll() reports readability from the TCP layer instead, which is
 * right except in the single case where a whole record has been decrypted into
 * the TLS buffer while the socket's wire buffer went empty. */
int tls_pending(int sess) __attribute__((weak));

/* SOCK_F_ALPN_* bits -> the comma-separated list tls_start() wants.
 *
 * The two sides describe ALPN differently on purpose. The syscall takes a
 * bitmask because a flat integer is one less user pointer for the kernel to
 * copy and validate on every connection, and the set an app can ask for is
 * exactly {h2, http/1.1}. TLS takes a string because it writes the list into
 * the extension. This is the seam, and it is three lines. h2 comes first when
 * both are set: ALPN preference is the order in the list, not the order of the
 * bits. NULL (not "") omits the extension entirely, which is what a socket
 * opened with no ALPN bits must do. */
static inline const char *tls_alpn_list(int sock_flags)
{
    int h2 = (sock_flags & SOCK_F_ALPN_H2) != 0;
    int h1 = (sock_flags & SOCK_F_ALPN_HTTP11) != 0;
    if (h2 && h1) return "h2,http/1.1";
    if (h2)       return "h2";
    if (h1)       return "http/1.1";
    return (const char *)0;
}

#endif /* LOGIT_TLS_STEP_H */
