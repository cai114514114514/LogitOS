#ifndef LOGIT_SOCK_H
#define LOGIT_SOCK_H

#include <stdint.h>
#include "logit_abi.h"      /* SOCK_F_* / SOCK_P_* / SOCK_E_* -- one definition,
                             * shared with ring 3 (include/abi) */

/* Non-blocking client sockets: DNS -> TCP -> (optionally) TLS, driven forward a
 * little at a time from net_poll().
 *
 * WHY THIS EXISTS. The old path was SYS_HTTP_GET, which ran the entire fetch --
 * lookup, handshake, request, body -- inline in the syscall, holding the big
 * kernel lock, with g_net_busy telling the window manager not to even poll the
 * network. One request could be in flight in the whole machine, and the desktop
 * was frozen for its duration. A page with forty sub-resources meant forty
 * sequential TLS handshakes behind a dead UI.
 *
 * Nothing here waits. sock_open() returns immediately with the socket in its
 * first state; every call after it either moves bytes that are already in a
 * buffer or reports a state. All the actual progress happens in sock_pump(),
 * called from net_poll(), and it advances EVERY socket on every call -- which is
 * what makes N connections handshake at the same time instead of in a queue.
 *
 * SYS_HTTP_GET and SYS_RES_FETCH are untouched and still work; the two paths
 * share only the TCP connection table. */

/* Longest host name a socket will carry (DNS allows 253; 128 covers every real
 * name and keeps the per-socket record small). Exported so the syscall stub
 * sizes its copy-in buffer from the same number the socket stores into. */
#define SOCK_HOST_MAX 128

/* Open a connection to host:port. `flags` is SOCK_F_*: TLS on/off plus the ALPN
 * protocols to offer. Returns a socket handle (>= 0) owned by `pid`, or < 0.
 * The handle is NOT a POSIX fd -- sockets live in their own table so that
 * adding them could not perturb file.c's fd semantics. */
int  sock_open(const char *host, int port, int flags, int pid);

/* Current state as SOCK_P_* bits, with SOCK_E_* in the error byte (see
 * logit_abi.h). Never blocks, never consumes. < 0 for a bad handle. */
int  sock_poll_bits(int fd, int pid);

/* Queue up to `len` bytes for transmission. Returns how many were taken -- 0
 * when the queue is full, which is normal and not an error. */
int  sock_send(int fd, const void *buf, int len, int pid);

/* Take up to `max` received bytes. 0 = none available yet, -1 = the stream is
 * finished or the socket failed. */
int  sock_recv(int fd, void *buf, int max, int pid);

/* The negotiated ALPN protocol, NUL-terminated. Returns its length (0 = none). */
int  sock_alpn(int fd, char *buf, int max, int pid);

/* Close: flush what is queued, send FIN, release the handle. */
int  sock_close(int fd, int pid);

/* Release every socket owned by `pid`. Called when a process exits, so a
 * browser tab that dies mid-load cannot strand connections -- the failure mode
 * the g_net_busy watchdog exists to paper over on the blocking path. */
void sock_close_owner(int pid);

/* Advance every socket. Pumped from net_poll(). */
void sock_pump(void);

#endif /* LOGIT_SOCK_H */
