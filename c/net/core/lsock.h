#ifndef LOGIT_LSOCK_H
#define LOGIT_LSOCK_H

#include <stdint.h>
#include "logit_abi.h"      /* struct logit_sockaddr, LSK_E_*, LOGIT_SO_* */

/* SERVER SOCKETS AS ORDINARY FILE DESCRIPTORS.
 *
 * c/net/core/sock.c is the client socket layer, and it says out loud that its
 * handles are NOT fds -- they live in their own table so that adding them could
 * not perturb file.c's fd semantics. That was the right call for a browser
 * fetching sub-resources with SYS_SOCK_*; it is the wrong one here.
 *
 * A SERVER SOCKET HAS TO BE AN FD. The whole value of accepting a connection is
 * handing it to something: dup2 it onto a child's stdin and exec a CGI, pass it
 * to a function that takes a descriptor, inherit it across fork. A socket with
 * its own private read/write calls can do none of that, and every program ever
 * written for it would be a program that only runs here.
 *
 * So this file is the F_SOCK backend: it owns the socket state, and
 * c/kernel/exec/file.c dispatches read/write/close into it by type. The three
 * lsock_file_* entry points are what that dispatch calls; everything else is
 * what c/kernel/exec/syscall.c calls for SYS_SOCKET..SYS_SENDTO.
 *
 * WHAT IS UNDERNEATH: TCP streams are c/net/transport/tcp.c listeners and
 * connections; datagram sockets are c/net/transport/udp.c bound ports. Nothing
 * here reimplements a protocol -- it is the fd-shaped face of the two that
 * exist. */

struct file;

/* --- the F_SOCK backend, called from c/kernel/exec/file.c --------------- */
long lsock_file_read(struct file *f, void *buf, long len);
long lsock_file_write(struct file *f, const void *buf, long len);
/* Last close of an F_SOCK description. Takes the raw `backing` pointer rather
 * than the file, because file_close() detaches the slot (and clears
 * f->backing) under its lock BEFORE running any teardown -- a teardown that
 * read f->backing afterwards would be reading whatever reclaimed the slot. */
void lsock_file_release_backing(void *backing);

/* --- the syscall face, called from c/kernel/exec/syscall.c -------------- */

/* A socket with no address yet. Returns an F_SOCK `struct file` with refcount 1
 * (the caller installs it in an fd slot), or NULL with *err set.
 *
 * LOGIT_SOCK_RAW additionally requires protocol == LOGIT_IPPROTO_ICMP (any
 * other protocol number is LSK_E_ARG -- there is nothing behind it to
 * deliver to) and a root credential for `pid`, checked against
 * c/fs/vfs_cred.c -- the one privilege boundary this machine has, reused
 * rather than a second one invented here -- and refused as LSK_E_PERM
 * otherwise. See c/net/core/raw.h for what a raw socket can and cannot do
 * once it exists (no IP_HDRINCL, no bind-to-filter-by-source). A raw socket
 * is usable immediately: unlike STREAM/DGRAM it is created directly into
 * S_RAW, not S_UNBOUND, so lsock_bind() on one returns LSK_E_STATE -- there
 * is no port to bind, and this machine has exactly one address to bind to
 * anyway. */
struct file *lsock_create(int domain, int type, int protocol, int pid, int *err);

int lsock_bind(struct file *f, const struct logit_sockaddr *a);
int lsock_listen(struct file *f, int backlog);

/* --- AF_UNIX (c/net/core/unix.c) ----------------------------------------- *
 *
 * bind/connect/getsockname take a PATH rather than a `struct logit_sockaddr`,
 * which is the whole reason they are separate entry points: an AF_UNIX address
 * is 108 bytes of name and does not fit in the 8-byte address this file's other
 * calls pass around. Everything ELSE about an AF_UNIX socket goes through the
 * calls above unchanged -- listen, accept, shutdown, read, write, close --
 * because it is the same F_SOCK `struct file`, and that is the point.
 *
 * `canon` is an ALREADY-RESOLVED absolute path: resolution needs the calling
 * process's cwd, so c/kernel/exec/syscall.c does it, once, the same way it does
 * for every other path-taking syscall. */
int lsock_bind_unix(struct file *f, const char *canon);
int lsock_connect_unix(struct file *f, const char *canon);
int lsock_getsockname_unix(struct file *f, char *out, int max);

/* Two connected, unnamed sockets. Both files come back with refcount 1 and the
 * caller installs them in fd slots; on failure NEITHER is created, because a
 * half-filled sv[] is something the caller cannot detect or clean up. */
int lsock_socketpair(int domain, int type, int protocol, int pid,
                     struct file **fa, struct file **fb, int *err);

/* Accept. Blocking unless the descriptor is O_NONBLOCK, in which case it
 * returns NULL with *err = LSK_E_AGAIN at once. `timeout_ms` 0 means wait
 * indefinitely. Returns a NEW F_SOCK file (refcount 1) for the connection. */
struct file *lsock_accept(struct file *f, struct logit_sockaddr *peer,
                          unsigned timeout_ms, int *err);

int lsock_getsockname(struct file *f, struct logit_sockaddr *out);
int lsock_getpeername(struct file *f, struct logit_sockaddr *out);
int lsock_setsockopt(struct file *f, int level, int optname, long value);
int lsock_shutdown(struct file *f, int how);

int lsock_recvfrom(struct file *f, void *buf, int max, struct logit_sockaddr *from);
int lsock_sendto(struct file *f, const void *buf, int len,
                 const struct logit_sockaddr *to);

/* One SOCKSTAT_* counter, or -1 for an unknown selector. */
long lsock_stat(int what);

/* Release every listening socket owned by `pid`. Called from process teardown
 * for the reason sock_close_owner exists: a server that dies must not leave its
 * port bound for the rest of the boot. */
void lsock_close_owner(int pid);

#endif /* LOGIT_LSOCK_H */
