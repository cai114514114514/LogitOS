#ifndef LOGIT_UNIX_H
#define LOGIT_UNIX_H

/* AF_UNIX: sockets that never touch a wire.
 *
 * WHY THIS FILE EXISTS. M18's stated goal is "run software not written for
 * LogitOS", and the first thing a Unix daemon does -- before it reads a config
 * file, before it forks, before it logs a line -- is open an AF_UNIX socket.
 * Measured on 2026-08-20: `AF_UNIX`, `AF_LOCAL` and `sockaddr_un` occurred
 * ZERO times in this tree. c/net/core knew AF_INET and nothing else, so every
 * such program stopped on its first socket() call with EAFNOSUPPORT.
 *
 * WHAT IT IS NOT. It is not a protocol and it is not on the network. Nothing
 * here calls into c/net/ip, c/net/transport or c/drivers/net, and none of it
 * needs a NIC: `lsock_create` refuses AF_INET when `net_up()` is false, and an
 * AF_UNIX socket deliberately reaches its own branch BEFORE that check. A
 * machine with no network still has to be able to run its own daemons.
 *
 * THE FD IS THE POINT, and it is the same argument lsock.h already makes for
 * accepted TCP connections: these are F_SOCK `struct file`s, so read/write/
 * close/dup2/fork all work on them and a socketpair can be handed to a child
 * exactly the way a pipe is. See lsock.c for how an AF_UNIX socket is
 * distinguished from an AF_INET one inside that single backing type.
 *
 * CONCURRENCY. Everything below runs under the big kernel lock, like the pipe
 * ring in c/kernel/exec/file.c and like lsock.c's own table -- there is one
 * acquisition site for kernel entry (c/kernel/cpu/interrupts.c) and none of
 * these calls is on syscall_is_bkl_free()'s allow-list. The waits DROP the BKL
 * across the park (sched_block_self_unlock does), which is why a blocked
 * reader does not wedge the machine. No lock of this file's own would add
 * anything today and one would have to be removed again when the BKL goes. */

struct usock;      /* opaque: the state behind an AF_UNIX struct file */
struct vcred;      /* c/fs/vfs_meta.h -- uid/gid, passed IN rather than fetched
                    * here, so the host gate can drive two different callers
                    * without a process table. */

/* Create an unbound, unconnected socket. `type` is LOGIT_SOCK_STREAM,
 * LOGIT_SOCK_DGRAM or LOGIT_SOCK_SEQPACKET. NULL + *err on failure. */
struct usock *unix_create(int type, int pid, int *err);

/* Claim `canon` (an ALREADY-RESOLVED absolute path -- see the namespace
 * argument at the top of unix.c for why this layer never sees a relative one).
 * Refuses a path another live socket holds. `cr` becomes the name's owner and
 * `umask` its creation mask; both are what connect() is then checked against. */
int  unix_bind(struct usock *s, const char *canon,
               const struct vcred *cr, unsigned umask);

int  unix_listen(struct usock *s, int backlog);

/* Pop one queued connection. Returns a NEW usock for the server side, or NULL
 * with *err (LSK_E_AGAIN when `nonblock` and the queue is empty). */
struct usock *unix_accept(struct usock *s, int nonblock, int *err);

/* Stream/seqpacket: queue this socket onto the listener at `canon`; returns as
 * soon as it is queued, without waiting for accept (Linux's behaviour, and the
 * reason a client may write before the server has accepted).
 * Datagram: record `canon` as the default destination for send/write.
 * `cr` is the CONNECTING process, checked for write permission on the name. */
int  unix_connect(struct usock *s, const char *canon, const struct vcred *cr);

long unix_read(struct usock *s, void *buf, long len, int nonblock);
long unix_write(struct usock *s, const void *buf, long len, int nonblock);

int  unix_shutdown(struct usock *s, int how);

/* The bound path, or "" -- getsockname() for AF_UNIX. */
int  unix_getsockname(struct usock *s, char *out, int max);

/* Two connected sockets with no name. Both are returned with a reference the
 * caller owns; unix_release each. */
int  unix_pair(int type, int pid, struct usock **a, struct usock **b, int *err);

/* Last close of the descriptor. Frees the name, wakes anybody parked on this
 * socket, and hands the peer an EOF. */
void unix_release(struct usock *s);

/* UNIXSTAT_* (include/abi/logit_abi.h), or -1 for an unknown selector. */
long unix_stat(int what);

#endif /* LOGIT_UNIX_H */
