#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
#include <stdint.h>

/* HALF OF THIS HEADER WORKS, AND THE HALF IS AF_UNIX.
 *
 * WHAT THIS COMMENT USED TO SAY, because a reader who remembers it should know
 * why it changed. It said "NOT A WORKING SOCKET LAYER -- every function below
 * FAILS at runtime", and it gave two reasons: that making a socket an fd would
 * mean "teaching the fd layer (c/kernel/exec/file.c) a new file kind", and that
 * "there is no bind()/listen()/accept() on ANY path -- the kernel is a client
 * only". Both were true when it was written and neither is true now. file.c has
 * F_SOCK; c/net/core/lsock.c is the server-socket family behind it; and
 * c/net/core/unix.c is AF_UNIX on the same descriptors.
 *
 * SO, PRECISELY:
 *
 *   AF_UNIX / AF_LOCAL  -- REAL. socket, socketpair, bind, listen, accept,
 *                          connect, send, recv, shutdown, getsockname, and
 *                          read/write/close/dup2/fork on the descriptor,
 *                          because it is an ordinary fd. SOCK_STREAM,
 *                          SOCK_DGRAM and SOCK_SEQPACKET all work.
 *   AF_INET             -- socket() returns EAFNOSUPPORT, as before. The two
 *                          things in the way are named in c/apps/libc/src/
 *                          socket.c: sockaddr_in is network byte order where
 *                          this ABI is host order everywhere, and SYS_CONNECT
 *                          has no AF_INET implementation to call. A program
 *                          that needs the network today uses the native ABI
 *                          directly, as /bin/httpd does.
 *   getpeername, getsockopt, setsockopt, sendto/recvfrom WITH an address
 *                       -- refused with a specific errno, never faked. Each
 *                          refusal is argued at its definition.
 *
 * Nothing here is stubbed to success: a call that cannot do its job returns -1
 * with the errno a C program tests for by name. */

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_storage { sa_family_t ss_family; char __pad[128 - sizeof(sa_family_t)]; };

/* Address families. The numbers are Linux's, so a program carrying its own
 * table agrees with this one. Only AF_UNIX is implemented -- see the banner. */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_LOCAL    AF_UNIX
#define AF_INET     2
#define PF_UNIX     AF_UNIX
#define PF_LOCAL    AF_UNIX
#define PF_INET     AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3
/* SOCK_SEQPACKET is 5 on Linux and it is 5 here, and unlike SOCK_RAW it is
 * IMPLEMENTED -- for AF_UNIX. It is a connection with message boundaries; see
 * the note beside LOGIT_SOCK_SEQPACKET in include/abi/logit_abi.h for why it
 * exists at all (it fell out of the datagram record ring) and why it is not an
 * alias for SOCK_STREAM. */
#define SOCK_SEQPACKET 5

#define SOL_SOCKET  1
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define SO_ERROR     4
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define MSG_PEEK     0x01
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);
ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len);
ssize_t recvfrom(int fd, void *buf, size_t n, int flags, struct sockaddr *addr, socklen_t *len);
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen);
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int shutdown(int fd, int how);
int socketpair(int domain, int type, int protocol, int sv[2]);

#endif /* _SYS_SOCKET_H */
